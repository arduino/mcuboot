# Copyright (c) Arduino s.r.l. and/or its affiliated companies
# SPDX-License-Identifier: Apache-2.0

# Under sysbuild every image sees EXTRA_ZEPHYR_MODULES; only MCUboot wants this.
if(NOT CONFIG_MCUBOOT)
  return()
endif()

zephyr_library()
zephyr_library_sources(
  hooks.c
  # USB DFU alternate writing the spare BFM Dual-Boot slot.
  bfm/bfm_flash.c
  bfm/bfm_boot.c
  bfm/bfm_dfu.c
)
zephyr_library_include_directories(bfm)
zephyr_library_link_libraries(MCUBOOT_BOOTUTIL)
