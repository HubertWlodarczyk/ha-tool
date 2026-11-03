# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(dut_sim)

target_sources(app PRIVATE
  src/main.c
  src/pairing.c
  src/audio_rx.c
  src/unicast.c
  src/ctrl.c
  src/audio_services.c
  src/bsink.c
  ../common/src/cli_out.c
  ../common/src/cli_evt.c
  ../common/src/cli_parse.c
  ../common/src/hci_util.c
)

target_include_directories(app PRIVATE
  src
  ../common/include
)
