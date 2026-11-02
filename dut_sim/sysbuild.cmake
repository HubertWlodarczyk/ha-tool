# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

if(SB_CONFIG_NET_CORE_IMAGE_HCI_IPC)
  # Bluetooth controller (Zephyr LL) on the network core
  set(NET_APP hci_ipc)
  set(NET_APP_SRC_DIR ${ZEPHYR_BASE}/samples/bluetooth/${NET_APP})

  ExternalZephyrProject_Add(
    APPLICATION ${NET_APP}
    SOURCE_DIR  ${NET_APP_SRC_DIR}
    BOARD       ${SB_CONFIG_NET_CORE_BOARD}
  )

  set(${NET_APP}_EXTRA_CONF_FILE
    "${NET_APP_SRC_DIR}/extra-iso-bt_ll_sw_split.conf;${APP_DIR}/../common/netcore.conf;${APP_DIR}/netcore_rx.conf"
    CACHE INTERNAL ""
  )

  native_simulator_set_child_images(${DEFAULT_IMAGE} ${NET_APP})
endif()

native_simulator_set_final_executable(${DEFAULT_IMAGE})
