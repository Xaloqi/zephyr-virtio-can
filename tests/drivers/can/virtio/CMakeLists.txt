# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Xaloqi
cmake_minimum_required(VERSION 3.20.0)
# The module under test is this repository.
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_SOURCE_DIR}/../../../..)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(can_virtio_test)
target_sources(app PRIVATE src/main.c src/fake_virtio.c)
