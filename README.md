# BLE Spectrum Monitor

A two-part system for watching 2.4 GHz BLE channel congestion live from a
browser:

1. **Firmware** (`firmware/`) for a XIAO-form-factor **nRF54L15** board. It runs
   radio energy-detect sweeps across all 40 BLE channels and streams the
   results out over a BLE "serial" link (Nordic UART Service, NUS).
2. **Web app** (`web/`) that connects to the board via **Web Bluetooth**, parses
   the stream, and visualises congestion live (bar chart + history heatmap +
   summary + CSV export).

```
┌──────────────┐   BLE / NUS    ┌──────────────────┐
│ nRF54L15      │ ─────────────▶ │ Chromium browser  │
│ (firmware/)   │  JSON sweeps   │ (web/, Web BT)    │
│  RSSI sweep   │ ◀───────────── │  charts + CSV     │
└──────────────┘   commands     └──────────────────┘
```

## Quick start

### Firmware

Requires nRF Connect SDK (NCS) **v2.6 or later**.

```sh
cd firmware
west build -b xiao_nrf54l15_sense/nrf54l15/cpuapp .
west flash
```

The project ships its own out-of-tree board definition for the **Seeed XIAO
nRF54L15 Sense** in `firmware/boards/seeed/xiao_nrf54l15_sense/`, so it builds
even on an SDK that predates the upstream board. On a recent Zephyr/NCS you can
use the upstream `xiao_nrf54l15/nrf54l15/cpuapp` target instead, or
`nrf54l15dk/nrf54l15/cpuapp` if you only have a Nordic DK.

See `firmware/README.md` for details, the command protocol, and the data format.

### Web app

Web Bluetooth requires a secure origin: serve over HTTPS, or from
`http://localhost`. No build step.

```sh
cd web
python3 -m http.server 8000
# open http://localhost:8000 in Chrome or Edge (desktop or Android)
```

Then: **Connect** → pick `BLE-SpectrumMon` → **Start**.

## Browser support

Web Bluetooth works in Chromium-based browsers (Chrome, Edge) on desktop and
Android. It is **not** available on iOS Safari or Firefox — the web app detects
this and shows a clear message instead of breaking.

## Repository layout

```
firmware/
  CMakeLists.txt
  prj.conf
  boards/seeed/xiao_nrf54l15_sense/   out-of-tree XIAO nRF54L15 Sense board
  src/
    main.c           NUS peripheral + command/sweep state machine
    radio_sweep.c    RADIO energy-detect (RSSI) sweep + channel→freq map
    radio_sweep.h
  README.md
web/
  index.html
  app.js             ES module — Web Bluetooth + state
  charts.js          ES module — chart setup + update helpers
  style.css
```

## Design choices & assumptions

- **Board target:** the project ships an out-of-tree **Seeed XIAO nRF54L15
  Sense** board definition (`firmware/boards/seeed/xiao_nrf54l15_sense/`,
  target `xiao_nrf54l15_sense/nrf54l15/cpuapp`), derived from the upstream
  Zephyr `xiao_nrf54l15` board. The upstream `xiao_nrf54l15/nrf54l15/cpuapp`
  and `nrf54l15dk/nrf54l15/cpuapp` targets also work.
- **Energy detection = Option B (sweep-then-stream MVP).** The firmware briefly
  takes the radio from the BLE stack to sweep all 40 channels (~a few ms, well
  within a connection interval), then streams the result. Option A (MPSL
  timeslot scanning that never interrupts the connection) is left as a clearly
  marked `TODO`/stub in `radio_sweep.c`.
- **Channel→frequency map** is an explicit lookup table; channels are labelled
  by BLE index, with advertising channels 37/38/39 highlighted in the UI.
- **MTU / fragmentation:** the firmware requests a larger MTU and fragments
  each sweep JSON to fit the negotiated payload; the web app reassembles by
  buffering until it sees `\n`.

## Documented TODOs (not blocking the MVP)

- **Option A** timeslot-based scanning (continuous scanning without ever
  pausing the BLE connection) — stub in `firmware/src/radio_sweep.c`.
- **Persistent history** beyond the session (IndexedDB) in the web app.
- **Multiple-monitor support** — aggregating sweeps from several boards placed
  around a space.
- **Authentication / pairing** — currently anyone in range can connect.
