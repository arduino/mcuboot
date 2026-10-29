# Copyright (c) Arduino s.r.l. and/or its affiliated companies
# SPDX-License-Identifier: Apache-2.0

# Under sysbuild every image sees EXTRA_ZEPHYR_MODULES; only MCUboot wants this.
if(NOT CONFIG_MCUBOOT)
  return()
endif()

zephyr_library()

if(CONFIG_SOC_PIC32CK)
zephyr_library_sources(
  hooks.c
  # USB DFU alternate writing the spare BFM Dual-Boot slot.
  bfm/bfm_flash.c
  bfm/bfm_boot.c
  bfm/bfm_dfu.c
)
zephyr_library_include_directories(bfm)
endif()

# Board-specific headers (usb_soft_disconnect.h) live in the ArduinoCore
# variant directory. Pass -DARDUINO_VARIANT_DIR=<path> from the build command
# so the hooks module stays board-agnostic.
if(NOT DEFINED ARDUINO_VARIANT_DIR)
  message(FATAL_ERROR "ARDUINO_VARIANT_DIR must be set (path to the variant directory)")
endif()
zephyr_library_include_directories(${ARDUINO_VARIANT_DIR})

zephyr_library_sources(hooks.c)
zephyr_library_sources(status_led.c)
zephyr_library_link_libraries(MCUBOOT_BOOTUTIL)
