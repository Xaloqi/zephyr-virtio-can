# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Xaloqi
if(CONFIG_CAN_VIRTIO)
  zephyr_library()
  zephyr_library_sources(can_virtio.c)
endif()
