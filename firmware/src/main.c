/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * BLE Spectrum Monitor — firmware entry point.
 *
 * Advertises as a Nordic UART Service (NUS) peripheral named
 * "BLE-SpectrumMon". While connected it runs a small state machine that sweeps
 * RSSI across all 40 BLE channels and streams the result as one newline-
 * delimited JSON object per sweep over NUS TX, fragmented to the current MTU.
 *
 * Command protocol (NUS RX, ASCII, newline-terminated, case-insensitive):
 *   START            begin continuous sweeps
 *   STOP             pause sweeps, stay connected
 *   INTERVAL <ms>    set delay between sweeps (default 1000, min 100)
 *   PING             reply with "PONG"
 *   <anything else>  reply with "ERR unknown"
 *
 * Structurally derived from the nRF Connect SDK peripheral_uart sample.
 */

#include <zephyr/kernel.h>
#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>

#include <bluetooth/services/nus.h>

#include "radio_sweep.h"

LOG_MODULE_REGISTER(spectrum_mon, LOG_LEVEL_INF);

/* ------------------------------------------------------------------ config */

#define DEVICE_NAME      CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN  (sizeof(DEVICE_NAME) - 1)

#define SWEEP_INTERVAL_DEFAULT_MS 1000
#define SWEEP_INTERVAL_MIN_MS     100

/* JSON for a 40-channel sweep is ~280-320 bytes; round up for safety. */
#define JSON_BUF_SIZE 384

/* ------------------------------------------------------------------ state */

static struct bt_conn *current_conn;

/* Negotiated ATT payload (MTU - 3). Updated on the MTU exchange callback. */
static atomic_t nus_mtu_payload = ATOMIC_INIT(20);

/* Sweep control, shared between the command handler and the sweep thread. */
static atomic_t sweeping = ATOMIC_INIT(0);
static atomic_t sweep_interval_ms = ATOMIC_INIT(SWEEP_INTERVAL_DEFAULT_MS);

/* Wakes the sweep thread when START / INTERVAL change the schedule. */
static K_SEM_DEFINE(sweep_kick, 0, 1);

/* ------------------------------------------------------------ advertising */

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_VAL),
};

/* --------------------------------------------------------------- TX helper */

/*
 * Send a buffer over NUS TX, fragmenting to the current MTU so payloads larger
 * than one notification (e.g. a full sweep JSON) are delivered in order. The
 * web app reassembles by buffering until it sees a '\n'.
 */
static int nus_send_fragmented(const uint8_t *data, uint16_t len)
{
	if (!current_conn) {
		return -ENOTCONN;
	}

	uint16_t chunk = (uint16_t)atomic_get(&nus_mtu_payload);
	if (chunk < 1) {
		chunk = 20;
	}

	uint16_t offset = 0;
	while (offset < len) {
		uint16_t this_len = MIN(chunk, (uint16_t)(len - offset));
		int err = bt_nus_send(current_conn, &data[offset], this_len);
		if (err) {
			LOG_WARN("bt_nus_send failed (err %d) at offset %u",
				 err, offset);
			return err;
		}
		offset += this_len;
	}
	return 0;
}

static void nus_send_line(const char *line)
{
	nus_send_fragmented((const uint8_t *)line, (uint16_t)strlen(line));
}

/* ------------------------------------------------------- command handling */

/* Compare an n-byte buffer to a literal, case-insensitively. */
static bool ci_equal(const uint8_t *buf, uint16_t len, const char *lit)
{
	size_t lit_len = strlen(lit);
	if (len != lit_len) {
		return false;
	}
	for (size_t i = 0; i < lit_len; i++) {
		if (tolower((int)buf[i]) != tolower((int)lit[i])) {
			return false;
		}
	}
	return true;
}

static void handle_command(const uint8_t *cmd, uint16_t len)
{
	/* Trim trailing CR/LF and surrounding whitespace. */
	while (len > 0 && (cmd[len - 1] == '\r' || cmd[len - 1] == '\n' ||
			   cmd[len - 1] == ' ' || cmd[len - 1] == '\t')) {
		len--;
	}
	while (len > 0 && (*cmd == ' ' || *cmd == '\t')) {
		cmd++;
		len--;
	}
	if (len == 0) {
		return;
	}

	if (ci_equal(cmd, len, "START")) {
		atomic_set(&sweeping, 1);
		k_sem_give(&sweep_kick);
		LOG_INF("START");
		return;
	}

	if (ci_equal(cmd, len, "STOP")) {
		atomic_set(&sweeping, 0);
		LOG_INF("STOP");
		return;
	}

	if (ci_equal(cmd, len, "PING")) {
		nus_send_line("PONG\n");
		return;
	}

	/* INTERVAL <ms> */
	if (len >= 8 &&
	    (tolower(cmd[0]) == 'i') && (tolower(cmd[1]) == 'n') &&
	    (tolower(cmd[2]) == 't') && (tolower(cmd[3]) == 'e') &&
	    (tolower(cmd[4]) == 'r') && (tolower(cmd[5]) == 'v') &&
	    (tolower(cmd[6]) == 'a') && (tolower(cmd[7]) == 'l')) {
		const uint8_t *p = cmd + 8;
		uint16_t rem = len - 8;
		while (rem > 0 && (*p == ' ' || *p == '\t')) {
			p++;
			rem--;
		}
		/* NUL-terminate into a small stack buffer for strtol. */
		char num[12] = {0};
		uint16_t n = MIN(rem, (uint16_t)(sizeof(num) - 1));
		memcpy(num, p, n);
		long ms = strtol(num, NULL, 10);
		if (ms < SWEEP_INTERVAL_MIN_MS) {
			ms = SWEEP_INTERVAL_MIN_MS;
		}
		atomic_set(&sweep_interval_ms, (atomic_val_t)ms);
		k_sem_give(&sweep_kick);
		LOG_INF("INTERVAL %ld", ms);
		return;
	}

	nus_send_line("ERR unknown\n");
}

/* NUS RX: data received from the central. */
static void nus_received_cb(struct bt_conn *conn, const uint8_t *const data,
			    uint16_t len)
{
	ARG_UNUSED(conn);
	handle_command(data, len);
}

static struct bt_nus_cb nus_cb = {
	.received = nus_received_cb,
};

/* ----------------------------------------------------- connection plumbing */

static void mtu_exchange_cb(struct bt_conn *conn, uint8_t err,
			    struct bt_gatt_exchange_params *params)
{
	ARG_UNUSED(params);
	if (err) {
		LOG_WARN("MTU exchange failed (err %u)", err);
		return;
	}
	uint16_t mtu = bt_gatt_get_mtu(conn);
	/* Usable notification payload is MTU minus the 3-byte ATT header. */
	uint16_t payload = (mtu > 3) ? (mtu - 3) : 20;
	atomic_set(&nus_mtu_payload, payload);
	LOG_INF("MTU exchanged: %u (payload %u)", mtu, payload);
}

static struct bt_gatt_exchange_params mtu_exchange_params = {
	.func = mtu_exchange_cb,
};

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (err) {
		LOG_ERR("Connection failed (err %u)", err);
		return;
	}

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Connected: %s", addr);

	current_conn = bt_conn_ref(conn);
	atomic_set(&nus_mtu_payload, 20); /* until the exchange completes */

	/* Ask for a bigger MTU so sweep JSON needs fewer notifications. */
	int rc = bt_gatt_exchange_mtu(conn, &mtu_exchange_params);
	if (rc) {
		LOG_WARN("MTU exchange request failed (err %d)", rc);
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Disconnected: %s (reason %u)", addr, reason);

	/* Pause sweeping; keep the configured interval for the next session. */
	atomic_set(&sweeping, 0);

	if (current_conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}
	atomic_set(&nus_mtu_payload, 20);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

/* ----------------------------------------------------------- sweep thread */

#define SWEEP_THREAD_STACK 2048
#define SWEEP_THREAD_PRIO  5

static K_THREAD_STACK_DEFINE(sweep_stack, SWEEP_THREAD_STACK);
static struct k_thread sweep_thread_data;

/* Build the sweep JSON line into @p out. Returns the string length. */
static int build_sweep_json(char *out, size_t out_size,
			    int64_t uptime, const int8_t *rssi)
{
	int n = snprintf(out, out_size, "{\"t\":%lld,\"rssi\":[",
			 (long long)uptime);
	if (n < 0 || (size_t)n >= out_size) {
		return -1;
	}

	for (int i = 0; i < BLE_NUM_CHANNELS; i++) {
		int m = snprintf(out + n, out_size - n, "%s%d",
				 (i == 0) ? "" : ",", rssi[i]);
		if (m < 0 || (size_t)(n + m) >= out_size) {
			return -1;
		}
		n += m;
	}

	int m = snprintf(out + n, out_size - n, "]}\n");
	if (m < 0 || (size_t)(n + m) >= out_size) {
		return -1;
	}
	return n + m;
}

static void sweep_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	static int8_t rssi[BLE_NUM_CHANNELS];
	static char json[JSON_BUF_SIZE];

	for (;;) {
		if (!atomic_get(&sweeping) || !current_conn) {
			/* Idle until START / a new connection kicks us. */
			k_sem_take(&sweep_kick, K_FOREVER);
			continue;
		}

		int64_t t0 = k_uptime_get();
		radio_sweep_run(rssi);

		int len = build_sweep_json(json, sizeof(json), t0, rssi);
		if (len > 0) {
			nus_send_fragmented((const uint8_t *)json, (uint16_t)len);
		} else {
			LOG_ERR("sweep JSON did not fit in buffer");
		}

		/*
		 * Wait for the configured interval, but wake early if a command
		 * changes the schedule (so e.g. STOP takes effect promptly).
		 */
		uint32_t interval = (uint32_t)atomic_get(&sweep_interval_ms);
		k_sem_take(&sweep_kick, K_MSEC(interval));
	}
}

/* ----------------------------------------------------------------- main */

int main(void)
{
	int err;

	err = bt_nus_init(&nus_cb);
	if (err) {
		LOG_ERR("Failed to init NUS (err %d)", err);
		return err;
	}

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Bluetooth init failed (err %d)", err);
		return err;
	}
	LOG_INF("Bluetooth initialised");

	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad),
			      sd, ARRAY_SIZE(sd));
	if (err) {
		LOG_ERR("Advertising failed to start (err %d)", err);
		return err;
	}
	LOG_INF("Advertising as \"%s\"", DEVICE_NAME);

	k_thread_create(&sweep_thread_data, sweep_stack,
			K_THREAD_STACK_SIZEOF(sweep_stack),
			sweep_thread_fn, NULL, NULL, NULL,
			SWEEP_THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&sweep_thread_data, "sweep");

	return 0;
}
