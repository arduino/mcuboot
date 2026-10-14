/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB DFU alternate that writes the spare BFM Dual-Boot slot (0x08010000) and
 * promotes it, so MCUboot can update itself without a probe.
 *
 * It joins the DFU interface CONFIG_BOOT_USB_DFU already registers: alternates
 * are ordered by image id, and "slot_bfm" sorts after "slot1_image" ('_' >
 * '1'), giving alt 0 = slot1 (application), alt 1 = Bootloader.
 *
 * On manifestation (size == 0): flush, validate the vector table and bump the
 * spare slot's sequence number. boot_go_hook() in hooks.c then resets into the
 * new bootloader.
 */

#include <zephyr/kernel.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_dfu.h>

#include "bootutil/bootutil_log.h"

#include "bfm_flash.h"
#include "bfm_boot.h"

BOOT_LOG_MODULE_DECLARE(mcuboot);

static int bfm_dfu_read(void *const priv,
			const uint32_t block, const uint16_t size,
			uint8_t buf[static CONFIG_USBD_DFU_TRANSFER_SIZE])
{
	ARG_UNUSED(priv);
	ARG_UNUSED(block);
	ARG_UNUSED(size);
	ARG_UNUSED(buf);
	return 0;
}

static int bfm_dfu_write(void *const priv,
			 const uint32_t block, const uint16_t size,
			 const uint8_t buf[static CONFIG_USBD_DFU_TRANSFER_SIZE])
{
	ARG_UNUSED(priv);
	int ret;

	if (block == 0) {
		ret = bfm_stream_begin(BFM_SPARE_SLOT, BFM_SLOT_SIZE);
		if (ret) {
			BOOT_LOG_ERR("BFM stream begin failed: %d", ret);
			return ret;
		}
	}

	if (size == 0) {
		ret = bfm_stream_finish();
		if (ret) {
			BOOT_LOG_ERR("BFM stream finish failed: %d", ret);
			return ret;
		}
		if (!bfm_boot_spare_image_valid()) {
			BOOT_LOG_ERR("BFM spare image not bootable");
			return -ENOEXEC;
		}
		ret = bfm_boot_promote_spare();
		if (ret) {
			BOOT_LOG_ERR("BFM promote failed: %d", ret);
			return ret;
		}
		BOOT_LOG_INF("BFM update complete: %u bytes, spare promoted",
			     bfm_stream_written());
		return 0;
	}

	return bfm_stream_write(buf, size);
}

static bool bfm_dfu_next(void *const priv,
			 const enum usb_dfu_state state,
			 const enum usb_dfu_state next)
{
	ARG_UNUSED(priv);
	ARG_UNUSED(state);
	ARG_UNUSED(next);

	return true;
}

USBD_DFU_DEFINE_IMG(slot_bfm, "Bootloader", NULL,
		    bfm_dfu_read, bfm_dfu_write, bfm_dfu_next);
