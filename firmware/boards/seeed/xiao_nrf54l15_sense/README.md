# Board: Seeed XIAO nRF54L15 Sense (out-of-tree)

Out-of-tree Zephyr / nRF Connect SDK board definition for the
**Seeed Studio XIAO nRF54L15 Sense**, used by this project's firmware.

Build target:

```
xiao_nrf54l15_sense/nrf54l15/cpuapp
```

```sh
cd firmware
west build -b xiao_nrf54l15_sense/nrf54l15/cpuapp .
west flash
```

## Why this exists

The XIAO nRF54L15 (and the Sense variant) is **upstream** in Zephyr as the board
`xiao_nrf54l15` (target `xiao_nrf54l15/nrf54l15/cpuapp`). The upstream board
already describes the Sense's onboard IMU and PDM microphone, so on a recent
enough Zephyr/NCS you can simply build against `xiao_nrf54l15/nrf54l15/cpuapp`.

This directory vendors an equivalent definition under a distinct
`xiao_nrf54l15_sense` name so the project builds even on an SDK that predates the
upstream board, and so the Sense-specific hardware (IMU, mic, power rails) is
documented in one place. It is found automatically because the application
directory (`firmware/`) is added to `BOARD_ROOT` by the Zephyr build.

If your SDK already ships `xiao_nrf54l15`, prefer the upstream target and treat
this as a reference / fallback.

## SoC

- Nordic **nRF54L15**: 128 MHz Arm Cortex-M33 (`cpuapp`) + RISC-V FLPR
  (`cpuflpr`), 1.5 MB NVM (RRAM). This definition provides the `cpuapp` target.

## Onboard hardware (Sense)

| Function          | Part           | Bus / pins                              |
|-------------------|----------------|-----------------------------------------|
| 6-axis IMU        | LSM6DS3TR-C    | I2C `i2c30` @ 0x6A; INT on P0.02; binding `st,lsm6dsl`; alias `imu0` |
| Digital mic (PDM) | MSM261DGT006   | `pdm20`: CLK P1.12, DIN P1.13; alias `dmic20` |
| User LED          | —              | P2.00 (active low); alias `led0`        |
| User button       | —              | P0.00 (pull-up, active low); alias `sw0`|
| IMU/PDM power rail | regulator-fixed| enable P0.01 (boot-on)                  |
| RF switch ctl/pwr | regulator-fixed| P2.05 / P2.03                           |
| Battery rail      | regulator-fixed| enable P1.15                            |

## XIAO header pin map (`xiao_d` connector)

| Header | nRF54L15 pin |   | Header | nRF54L15 pin |
|--------|--------------|---|--------|--------------|
| D0     | P1.04        |   | D8     | P2.01 (SCK)  |
| D1     | P1.05        |   | D9     | P2.04 (MISO) |
| D2     | P1.06        |   | D10    | P2.02 (MOSI) |
| D3     | P1.07        |   | D11    | P0.03        |
| D4     | P1.10 (SDA)  |   | D12    | P0.04        |
| D5     | P1.11 (SCL)  |   | D13    | P2.10        |
| D6     | P2.08 (TX)   |   | D14    | P2.09        |
| D7     | P2.07 (RX)   |   | D15    | P2.06        |

Default bus aliases: `xiao_i2c` → `i2c22`, `xiao_spi` → `spi00`,
`xiao_serial` → `uart21`, `xiao_adc` → `adc`.

Console / log output is on `uart20` (TX P1.09, RX P1.08), separate from the
XIAO header serial.

## Relevance to this firmware

The BLE Spectrum Monitor only needs the radio and BLE stack, so it does not use
the IMU, microphone, or XIAO header buses. Those are described here for
completeness and so the board target is reusable. Debug logging goes out on the
`uart20` console.

## Files

| File | Purpose |
|------|---------|
| `board.yml` | Board name / vendor / SoC declaration. |
| `board.cmake` | Flash/debug runner args (J-Link, OpenOCD, nrfutil, nrfjprog). |
| `Kconfig.xiao_nrf54l15_sense` | Board → SoC selection. |
| `Kconfig.defconfig` | Board-level Kconfig defaults (BT controller). |
| `pre_dt_board.cmake` | DTC flag tweaks. |
| `seeed_xiao_connector.dtsi` | XIAO header GPIO map. |
| `xiao_nrf54l15_sense-pinctrl.dtsi` | Pin-control states for UART/I2C/SPI/PDM. |
| `xiao_nrf54l15_sense_common.dtsi` | LEDs, buttons, bus/ADC config, aliases. |
| `xiao_nrf54l15_sense_nrf54l15_cpuapp.dts` | cpuapp board DTS (regulators, sensors, chosen). |
| `xiao_nrf54l15_sense_nrf54l15_cpuapp.yaml` | Twister/board metadata. |
| `xiao_nrf54l15_sense_nrf54l15_cpuapp_defconfig` | Board default Kconfig. |

Derived from the upstream Zephyr `seeed/xiao_nrf54l15` board (Apache-2.0).
