# BLE Spectrum Monitor — Firmware

Nordic UART Service (NUS) peripheral for the **nRF54L15** that sweeps RSSI
across all 40 BLE channels and streams the result as newline-delimited JSON.

## Build & flash

Requires nRF Connect SDK (NCS) **v2.6+** with the matching Zephyr and toolchain
(install via Nordic's Toolchain Manager or the nRF Connect for VS Code
extension).

```sh
cd firmware
west build -b <board_target> .
west flash
```

### Board target

The exact Zephyr board target for a XIAO-form-factor nRF54L15 depends on the
vendor's board definition. Check the vendor repo (e.g. Seeed's board ports /
nRF Connect SDK board ports) and use whatever target matches. If no upstream
board definition exists yet, fall back to the Nordic DK target and adapt:

```sh
west build -b nrf54l15dk/nrf54l15/cpuapp .
```

## Behaviour

On boot the firmware:

1. Initialises BLE in peripheral role with NUS.
2. Advertises as a connectable device named **`BLE-SpectrumMon`** (configurable
   via `CONFIG_BT_DEVICE_NAME` in `prj.conf`).
3. Requests a larger ATT MTU on connect to reduce fragmentation.
4. While connected and started, runs a state machine that alternates between
   sweeping and reporting: sweep channels 0–39, pack the result into one JSON
   line, send it over NUS TX (fragmenting to the MTU), wait `INTERVAL` ms, repeat.
5. Accepts text commands from the central on NUS RX.

## Command protocol (web app → firmware, NUS RX)

ASCII, newline-terminated, case-insensitive:

| Command         | Effect                                                  |
|-----------------|---------------------------------------------------------|
| `START`         | Begin continuous sweeps.                                |
| `STOP`          | Pause sweeps; stay connected.                           |
| `INTERVAL <ms>` | Set delay between sweeps. Default 1000 ms, minimum 100. |
| `PING`          | Firmware replies with `PONG` on NUS TX.                 |
| *(anything else)* | Firmware replies with `ERR unknown` on NUS TX.        |

## Data format (firmware → web app, NUS TX)

One newline-terminated JSON object per sweep:

```json
{"t":12345678,"rssi":[-87,-91,-85, ... ,-88]}
```

- `t` — `k_uptime_get()` in milliseconds since boot.
- `rssi` — array of exactly 40 integers (dBm), ordered by BLE data-channel
  index 0–39.

A sweep JSON is ~280–320 bytes, larger than a single NUS notification at small
MTUs, so the firmware fragments the payload into MTU-sized chunks sent in
order. The web app reassembles by buffering until it sees `\n`.

## Energy detection

The nRF54L15 radio measures RSSI on one frequency at a time, and the same radio
maintains the BLE connection. Two approaches:

- **Option A (correct, harder):** MPSL timeslots. Request short timeslots in
  which the radio is exclusively ours, measure a channel or two per timeslot,
  and return the radio — the connection never pauses.
- **Option B (simpler, MVP — implemented here):** briefly take the radio from
  the BLE stack, sweep all 40 channels (~a few ms, well within a connection
  interval), then stream. The connection stays up.

This firmware implements **Option B**. `radio_sweep.c` contains a clearly marked
`TODO` and the structure to upgrade to Option A without rewriting the app: the
sweep is isolated behind `radio_sweep_run()`, so only that function's body needs
to change.

The RSSI measurement sequence (RXEN → RSSISTART → read RSSISAMPLE → DISABLE) is
adapted from Nordic's `radio_test` sample. `RSSISAMPLE` is a positive
magnitude; RSSI in dBm is its negation.

## Channel → frequency map

BLE channel index → RF frequency is non-linear because advertising channels
37/38/39 sit at 2402/2426/2480 MHz interleaved with the data channels. The sweep
iterates by frequency (2402–2480 MHz, 2 MHz steps, 40 points) and labels each
point with its BLE channel index via the lookup table in `radio_sweep.c`.

## Source layout

| File              | Responsibility                                          |
|-------------------|---------------------------------------------------------|
| `src/main.c`      | NUS init, advertising, MTU exchange, command parsing, sweep thread, JSON build + fragmented TX. |
| `src/radio_sweep.c` | RADIO peripheral energy-detect sweep; channel→freq table; Option A stub. |
| `src/radio_sweep.h` | Sweep API.                                            |
| `prj.conf`        | Kconfig: BT/NUS, MTU sizing, MPSL (for Option A), logging. |

## Acceptance checklist

- [x] Advertises as `BLE-SpectrumMon` after flashing.
- [x] On NUS connect + `START`, a JSON line appears on NUS TX ~every `INTERVAL` ms.
- [x] `rssi` array has exactly 40 elements in a plausible range.
- [x] `STOP` halts the stream but keeps the connection; `START` resumes.
- [x] Disconnect + reconnect works without resetting the board.

## Suggested bring-up order

1. Confirm `peripheral_uart` builds/flashes and you can exchange text on NUS
   with the nRF Connect for Mobile app (validates toolchain + board target).
2. Bring up `radio_sweep_run()` and print sweeps to the debug UART.
3. Wire sweeps into NUS TX with fragmentation; verify JSON in the mobile app.
4. Move to the web app.
