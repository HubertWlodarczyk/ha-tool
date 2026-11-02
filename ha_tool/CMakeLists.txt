# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ha_tool)

target_sources(app PRIVATE
  src/main.c
  src/sys_cmds.c
  src/human_evt.c
  src/cli_table.c
  src/dev.c
  src/dev_cmds.c
  src/dev_link_cmds.c
  src/audio_tx.c
  src/lc3_frames.c
  src/phone.c
  src/phone_cmds.c
  src/unicast.c
  src/call.c
  src/media.c
  src/volume.c
  src/hacontrol.c
  src/bcast/model.c
  src/bcast/budget.c
  src/bcast/source.c
  src/bcast/args.c
  src/bcast/validate.c
  src/bcast/cmds.c
  src/bcast/run_cmds.c
  src/presets.c
  ../common/src/cli_out.c
  ../common/src/cli_evt.c
  ../common/src/cli_parse.c
  ../common/src/cli_tree.c
  ../common/src/hci_util.c
)

# Real board only: application core at 128 MHz
if(CONFIG_BOARD_NRF5340DK)
  target_sources(app PRIVATE src/board.c)
endif()

target_include_directories(app PRIVATE
  src
  ../common/include
)
