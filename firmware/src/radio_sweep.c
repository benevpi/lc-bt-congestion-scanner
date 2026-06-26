/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Radio energy-detect (RSSI) sweep across the 40 BLE channels.
 *
 * The RSSI measurement sequence is lifted from the nRF Connect SDK
 * `radio_test` sample: enable RX on a single frequency, kick RSSISTART, wait
 * for RSSIEND, read RSSISAMPLE (a positive number = -dBm), then disable.
 */

#include "radio_sweep.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_radio.h>

LOG_MODULE_REGISTER(radio_sweep, LOG_LEVEL_INF);

/*
 * BLE channel index -> RF centre frequency (MHz).
 *
 * The radio numbers channels by frequency; BLE numbers them logically. The
 * advertising channels (37, 38, 39) sit at 2402, 2426 and 2480 MHz, which is
 * why the mapping is non-linear. This table makes the relationship explicit so
 * the web app can label channels by their BLE index.
 *
 * Data channels 0-10  : 2404 .. 2424 MHz
 * Data channels 11-36 : 2428 .. 2478 MHz
 * Adv   channel  37   : 2402 MHz
 * Adv   channel  38   : 2426 MHz
 * Adv   channel  39   : 2480 MHz
 */
static const uint16_t ble_freq_table[BLE_NUM_CHANNELS] = {
	/* 0..10 */
	2404, 2406, 2408, 2410, 2412, 2414, 2416, 2418, 2420, 2422, 2424,
	/* 11..36 */
	2428, 2430, 2432, 2434, 2436, 2438, 2440, 2442, 2444, 2446, 2448,
	2450, 2452, 2454, 2456, 2458, 2460, 2462, 2464, 2466, 2468, 2470,
	2472, 2474, 2476, 2478,
	/* 37, 38, 39 (advertising) */
	2402, 2426, 2480,
};

uint16_t ble_channel_to_freq_mhz(uint8_t ch_index)
{
	if (ch_index >= BLE_NUM_CHANNELS) {
		return 0;
	}
	return ble_freq_table[ch_index];
}

/* Conservative ramp-up time for RX to settle before sampling RSSI. */
#define RADIO_RX_RAMPUP_US 140
#define RADIO_RSSI_TIMEOUT_US 50

/**
 * @brief Measure RSSI on a single RF frequency.
 *
 * @param freq_mhz Centre frequency, 2400-2500 MHz.
 * @return RSSI in dBm (negative), or -128 on failure.
 */
static int8_t measure_rssi_mhz(uint16_t freq_mhz)
{
	if (freq_mhz < 2400 || freq_mhz > 2500) {
		return -128;
	}

	/* Make sure the radio is idle before we take it over. */
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
	while (!nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_DISABLED)) {
		/* spin: disable is fast */
	}

	/* FREQUENCY register is offset from 2400 MHz. */
	nrf_radio_frequency_set(NRF_RADIO, freq_mhz);

	/* Enable the receiver. */
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_READY);
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RXEN);

	/* Wait for RX to be ready (with a bounded busy-wait fallback). */
	uint32_t spins = 0;
	while (!nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_READY)) {
		if (++spins > 100000) {
			break;
		}
	}
	k_busy_wait(RADIO_RX_RAMPUP_US);

	/* Trigger an RSSI measurement and wait for it to finish. */
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_RSSIEND);
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RSSISTART);

	spins = 0;
	while (!nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_RSSIEND)) {
		if (++spins > 100000) {
			break;
		}
	}

	/* RSSISAMPLE is a positive magnitude; RSSI in dBm = -RSSISAMPLE. */
	uint8_t sample = nrf_radio_rssi_sample_get(NRF_RADIO);
	int8_t rssi_dbm = -(int8_t)sample;

	/* Tear the receiver back down so the BLE stack can reclaim the radio. */
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
	spins = 0;
	while (!nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_DISABLED)) {
		if (++spins > 100000) {
			break;
		}
	}

	return rssi_dbm;
}

void radio_sweep_run(int8_t rssi_out[BLE_NUM_CHANNELS])
{
	/*
	 * Option B (MVP): briefly take the radio away from the BLE stack and
	 * sweep all 40 channels. A full sweep is only a few milliseconds, so
	 * the connection survives. We mask interrupts during the sweep to keep
	 * the soft device / controller from touching the radio mid-measurement.
	 *
	 * TODO (Option A): replace the body of this function with an MPSL
	 * timeslot request. Inside each granted timeslot, run measure_rssi_mhz()
	 * for one (or a few) channels, then return the radio. This keeps the BLE
	 * connection rock-solid even at short connection intervals and removes
	 * the need to mask interrupts here. See Nordic's MPSL timeslot sample
	 * (`mpsl/timeslot`) for the request/callback plumbing.
	 */
	unsigned int key = irq_lock();

	for (uint8_t ch = 0; ch < BLE_NUM_CHANNELS; ch++) {
		rssi_out[ch] = measure_rssi_mhz(ble_freq_table[ch]);
	}

	irq_unlock(key);
}
