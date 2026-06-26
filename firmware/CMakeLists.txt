# SPDX-License-Identifier: Apache-2.0
#
# BLE Spectrum Monitor firmware
# Builds a Nordic UART Service peripheral that sweeps the 2.4 GHz band and
# streams per-channel RSSI as newline-delimited JSON.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ble_spectrum_monitor)

target_sources(app PRIVATE
  src/main.c
  src/radio_sweep.c
)

target_include_directories(app PRIVATE src)
