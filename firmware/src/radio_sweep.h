/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Radio energy-detect (RSSI) sweep across the 40 BLE channels.
 */

#ifndef RADIO_SWEEP_H_
#define RADIO_SWEEP_H_

#include <stdint.h>

/* Number of BLE channels (37 data + 3 advertising). */
#define BLE_NUM_CHANNELS 40

/**
 * @brief Map a BLE channel index (0-39) to its RF centre frequency in MHz.
 *
 * Data channels 0-36 and advertising channels 37/38/39 are interleaved on the
 * physical band. This returns the actual 2402-2480 MHz centre frequency.
 *
 * @param ch_index BLE channel index, 0-39.
 * @return Centre frequency in MHz, or 0 if @p ch_index is out of range.
 */
uint16_t ble_channel_to_freq_mhz(uint8_t ch_index);

/**
 * @brief Perform a full energy-detect sweep across all 40 BLE channels.
 *
 * Option B (MVP): the BLE stack is briefly idle while the RADIO peripheral is
 * driven directly. A full sweep is only a few milliseconds, well within a
 * connection interval. See radio_sweep.c for the Option A (MPSL timeslot) TODO.
 *
 * @param rssi_out Caller-provided array of at least BLE_NUM_CHANNELS entries.
 *                 Filled with measured RSSI in dBm, indexed by BLE channel
 *                 index 0-39. Channels that fail to measure are set to -128.
 */
void radio_sweep_run(int8_t rssi_out[BLE_NUM_CHANNELS]);

#endif /* RADIO_SWEEP_H_ */
