/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/usbd_msg.h>

#include "bootutil/bootutil_log.h"
#include "bootutil/bootutil_public.h"
#include "usbd_dfu.h"

#if defined(CONFIG_BOOT_USB_DFU_ERASE_TRAILING_AREA) || defined(CONFIG_BOOT_USB_DFU_DIRECT_PRIMARY)
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/dfu/mcuboot.h>
#include <sysflash/sysflash.h>
#endif

#if defined(CONFIG_BOOT_USB_DFU_DIRECT_PRIMARY)
#include "flash_map_backend/flash_map_backend.h"
#endif

BOOT_LOG_MODULE_DECLARE(mcuboot);

USBD_DEVICE_DEFINE(boot_dfu_usbd,
		   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   CONFIG_BOOT_USB_DFU_VID,
		   CONFIG_BOOT_USB_DFU_PID);

USBD_DESC_LANG_DEFINE(boot_dfu_lang);
USBD_DESC_MANUFACTURER_DEFINE(boot_dfu_mfr,
			      CONFIG_BOOT_USB_DFU_MANUFACTURER_STRING);
USBD_DESC_PRODUCT_DEFINE(boot_dfu_product,
			 CONFIG_BOOT_USB_DFU_PRODUCT_STRING);
IF_ENABLED(CONFIG_HWINFO, (USBD_DESC_SERIAL_NUMBER_DEFINE(boot_dfu_sn)));

static const uint8_t boot_dfu_attributes =
	IS_ENABLED(CONFIG_BOOT_USB_DFU_SELF_POWERED) ? USB_SCD_SELF_POWERED : 0;

USBD_DESC_CONFIG_DEFINE(boot_dfu_fs_cfg_desc, "DFU FS Configuration");
USBD_CONFIGURATION_DEFINE(boot_dfu_fs_config,
			  boot_dfu_attributes,
			  CONFIG_BOOT_USB_DFU_MAX_POWER,
			  &boot_dfu_fs_cfg_desc);

#if USBD_SUPPORTS_HIGH_SPEED
USBD_DESC_CONFIG_DEFINE(boot_dfu_hs_cfg_desc, "DFU HS Configuration");
USBD_CONFIGURATION_DEFINE(boot_dfu_hs_config,
			  boot_dfu_attributes,
			  CONFIG_BOOT_USB_DFU_MAX_POWER,
			  &boot_dfu_hs_cfg_desc);
#endif

K_SEM_DEFINE(boot_dfu_complete, 0, 1);

static bool boot_dfu_initialized;

static void boot_dfu_msg_cb(struct usbd_context *const ctx,
			    const struct usbd_msg *const msg)
{
	ARG_UNUSED(ctx);

	if (msg->type == USBD_MSG_DFU_DOWNLOAD_COMPLETED) {
		k_sem_give(&boot_dfu_complete);
	}
}

static int boot_dfu_register_fs(void)
{
	int err;

	err = usbd_add_configuration(&boot_dfu_usbd, USBD_SPEED_FS,
				     &boot_dfu_fs_config);
	if (err) {
		BOOT_LOG_ERR("Failed to add DFU FS configuration: %d", err);
		return err;
	}

	err = usbd_register_class(&boot_dfu_usbd, "dfu_dfu", USBD_SPEED_FS, 1);
	if (err) {
		BOOT_LOG_ERR("Failed to register DFU class (FS): %d", err);
		return err;
	}

	err = usbd_device_set_code_triple(&boot_dfu_usbd, USBD_SPEED_FS, 0, 0, 0);
	if (err) {
		BOOT_LOG_ERR("Failed to set DFU code triple (FS): %d", err);
	}

	return err;
}

#if USBD_SUPPORTS_HIGH_SPEED
static int boot_dfu_register_hs(void)
{
	int err;

	err = usbd_add_configuration(&boot_dfu_usbd, USBD_SPEED_HS,
				     &boot_dfu_hs_config);
	if (err) {
		BOOT_LOG_ERR("Failed to add DFU HS configuration: %d", err);
		return err;
	}

	err = usbd_register_class(&boot_dfu_usbd, "dfu_dfu", USBD_SPEED_HS, 1);
	if (err) {
		BOOT_LOG_ERR("Failed to register DFU class (HS): %d", err);
		return err;
	}

	err = usbd_device_set_code_triple(&boot_dfu_usbd, USBD_SPEED_HS, 0, 0, 0);
	if (err) {
		BOOT_LOG_ERR("Failed to set DFU code triple (HS): %d", err);
	}

	return err;
}
#endif

static int boot_dfu_init(void)
{
	int err;

	err = usbd_add_descriptor(&boot_dfu_usbd, &boot_dfu_lang);
	if (err) {
		BOOT_LOG_ERR("Failed to add DFU language descriptor: %d", err);
		return err;
	}

	err = usbd_add_descriptor(&boot_dfu_usbd, &boot_dfu_mfr);
	if (err) {
		BOOT_LOG_ERR("Failed to add DFU manufacturer descriptor: %d", err);
		return err;
	}

	err = usbd_add_descriptor(&boot_dfu_usbd, &boot_dfu_product);
	if (err) {
		BOOT_LOG_ERR("Failed to add DFU product descriptor: %d", err);
		return err;
	}

	IF_ENABLED(CONFIG_HWINFO, (
		err = usbd_add_descriptor(&boot_dfu_usbd, &boot_dfu_sn);
		if (err) {
			BOOT_LOG_ERR("Failed to add DFU serial number descriptor: %d", err);
			return err;
		}
	))

#if USBD_SUPPORTS_HIGH_SPEED
	if (usbd_caps_speed(&boot_dfu_usbd) == USBD_SPEED_HS) {
		err = boot_dfu_register_hs();
		if (err) {
			return err;
		}
	}
#endif

	err = boot_dfu_register_fs();
	if (err) {
		return err;
	}

	usbd_self_powered(&boot_dfu_usbd,
			  boot_dfu_attributes & USB_SCD_SELF_POWERED);

	err = usbd_msg_register_cb(&boot_dfu_usbd, boot_dfu_msg_cb);
	if (err) {
		BOOT_LOG_ERR("Failed to register DFU message callback: %d", err);
		return err;
	}

	err = usbd_init(&boot_dfu_usbd);
	if (err) {
		BOOT_LOG_ERR("Failed to initialize USB DFU: %d", err);
	}

	return err;
}

int boot_usb_dfu_enable(void)
{
	int err;

	if (!boot_dfu_initialized) {
		err = boot_dfu_init();
		if (err) {
			return err;
		}

		boot_dfu_initialized = true;
	}

	err = usbd_enable(&boot_dfu_usbd);
	if (err && err != -EALREADY) {
		BOOT_LOG_ERR("Failed to enable USB DFU: %d", err);
	}

	return err;
}

struct usbd_context *boot_usb_dfu_get_context(void)
{
	return &boot_dfu_usbd;
}

#if defined(CONFIG_BOOT_USB_DFU_ERASE_TRAILING_AREA)
/*
 * Erase the unused flash between the end of the just-downloaded image
 * (header + body + TLVs) and the start of the image trailer in the
 * secondary slot. This removes any leftover data from a previously
 * downloaded image that was larger than the current one. The trailer
 * itself (swap status fields and magic) is never touched.
 */
static void boot_usb_dfu_erase_trailing_area(void)
{
	const struct flash_area *fa;
	struct image_header hdr;
	struct image_tlv_info tlv_info;
	const struct flash_parameters *fparams;
	uint32_t image_end;
	uint32_t trailer_off;
	uint32_t erase_start;
	uint32_t erase_end;
	int rc;

	rc = flash_area_open(FLASH_AREA_IMAGE_SECONDARY(0), &fa);
	if (rc) {
		BOOT_LOG_ERR("Failed to open secondary slot: %d", rc);
		return;
	}

	rc = flash_area_read(fa, 0, &hdr, sizeof(hdr));
	if (rc || hdr.ih_magic != IMAGE_MAGIC) {
		BOOT_LOG_ERR("Downloaded image has no valid header, skipping erase");
		goto done;
	}

	/* Walk past the image body and TLV area(s) to find where the image ends. */
	image_end = hdr.ih_hdr_size + hdr.ih_img_size;

	rc = flash_area_read(fa, image_end, &tlv_info, sizeof(tlv_info));
	if (rc) {
		BOOT_LOG_ERR("Failed to read TLV info: %d", rc);
		goto done;
	}

	if (tlv_info.it_magic == IMAGE_TLV_PROT_INFO_MAGIC) {
		if (hdr.ih_protect_tlv_size != tlv_info.it_tlv_tot) {
			BOOT_LOG_ERR("Protected TLV size mismatch, skipping erase");
			goto done;
		}

		image_end += tlv_info.it_tlv_tot;

		rc = flash_area_read(fa, image_end, &tlv_info, sizeof(tlv_info));
		if (rc) {
			BOOT_LOG_ERR("Failed to read TLV info: %d", rc);
			goto done;
		}
	} else if (hdr.ih_protect_tlv_size != 0) {
		BOOT_LOG_ERR("Missing protected TLV info, skipping erase");
		goto done;
	}

	if (tlv_info.it_magic != IMAGE_TLV_INFO_MAGIC) {
		BOOT_LOG_ERR("Invalid TLV info magic, skipping erase");
		goto done;
	}

	image_end += tlv_info.it_tlv_tot;

	/*
	 * Start of the image trailer status fields; never erase past this.
	 * This mirrors the boundary already used by Zephyr's own flash_img
	 * progressive-erase logic (scramble_mcuboot_trailer()) and is
	 * independent of the swap algorithm: it covers only the
	 * swap_type/copy_done/image_ok/swap_size fields and the trailer
	 * magic, not the much larger, swap-algorithm-only per-sector swap
	 * status area, which is unused in overwrite-only mode and holds
	 * only in-progress swap bookkeeping in swap modes.
	 */
	trailer_off = (uint32_t)boot_get_trailer_status_offset(fa->fa_size);

	if (image_end >= trailer_off) {
		/* Image fills (almost) the whole slot, nothing to erase. */
		goto done;
	}

	fparams = flash_get_parameters(fa->fa_dev);

	if (flash_params_get_erase_cap(fparams) & FLASH_ERASE_C_EXPLICIT) {
		struct flash_pages_info page;

		/* Round the start up to the next page so no image byte is erased. */
		rc = flash_get_page_info_by_offs(fa->fa_dev, fa->fa_off + image_end, &page);
		if (rc) {
			BOOT_LOG_ERR("Failed to get page info: %d", rc);
			goto done;
		}

		if ((uint32_t)(page.start_offset - fa->fa_off) == image_end) {
			erase_start = image_end;
		} else {
			erase_start = (uint32_t)(page.start_offset - fa->fa_off) + page.size;
		}

		/*
		 * Round the end down to the page holding the trailer, so the
		 * trailer is never touched.
		 */
		rc = flash_get_page_info_by_offs(fa->fa_dev, fa->fa_off + trailer_off, &page);
		if (rc) {
			BOOT_LOG_ERR("Failed to get page info: %d", rc);
			goto done;
		}

		erase_end = (uint32_t)(page.start_offset - fa->fa_off);
	} else {
		uint32_t write_block_size = fparams->write_block_size;

		erase_start = (image_end + write_block_size - 1) & ~(write_block_size - 1);
		erase_end = trailer_off & ~(write_block_size - 1);
	}

	if (erase_start >= erase_end) {
		goto done;
	}

	rc = flash_area_flatten(fa, erase_start, erase_end - erase_start);
	if (rc) {
		BOOT_LOG_ERR("Failed to erase trailing area: %d", rc);
	} else {
		BOOT_LOG_INF("Erased trailing area [0x%x, 0x%x)", erase_start, erase_end);
	}

done:
	flash_area_close(fa);
}
#endif /* CONFIG_BOOT_USB_DFU_ERASE_TRAILING_AREA */

#if defined(CONFIG_BOOT_USB_DFU_DIRECT_PRIMARY)
/* Erase the MCUboot sector at off (negative: counted from the end of the area) */
static int boot_usb_dfu_erase_sector(const struct flash_area *fa, off_t off)
{
	struct flash_sector sector;
	int rc;

	if (off < 0) {
		off += flash_area_get_size(fa);
	}

	rc = flash_area_get_sector(fa, off, &sector);
	if (rc == 0) {
		rc = flash_area_flatten(fa, flash_sector_get_off(&sector),
					flash_sector_get_size(&sector));
	}

	return rc;
}

/*
 * The download went straight into the primary slot: drop every swap state so
 * the next boot runs it as is, instead of swapping in (or reverting to) what
 * the secondary slot still holds.
 */
static void boot_usb_dfu_drop_swap_state(void)
{
	const struct flash_area *pri;
	const struct flash_area *sec;
	struct flash_sector first;
	int rc;

	rc = flash_area_open(FLASH_AREA_IMAGE_PRIMARY(0), &pri);
	if (rc == 0) {
		rc = boot_usb_dfu_erase_sector(pri, -1);
		flash_area_close(pri);
	}
	if (rc) {
		BOOT_LOG_ERR("Failed to erase primary trailer: %d", rc);
	}

	rc = flash_area_open(FLASH_AREA_IMAGE_SECONDARY(0), &sec);
	if (rc == 0) {
		/* An image header can sit in the first or (swap-offset) second sector */
		rc = boot_usb_dfu_erase_sector(sec, 0);
		if (rc == 0 && flash_area_get_sector(sec, 0, &first) == 0) {
			rc = boot_usb_dfu_erase_sector(sec, flash_sector_get_size(&first));
		}
		if (rc == 0) {
			rc = boot_usb_dfu_erase_sector(sec, -1);
		}
		flash_area_close(sec);
	}
	if (rc) {
		BOOT_LOG_ERR("Failed to erase secondary slot state: %d", rc);
	}
}
#endif /* CONFIG_BOOT_USB_DFU_DIRECT_PRIMARY */

void boot_usb_dfu_wait(k_timeout_t delay)
{
	if (k_sem_take(&boot_dfu_complete, delay) != 0) {
		return;
	}

	BOOT_LOG_INF("USB DFU completed");

#if !defined(CONFIG_SINGLE_APPLICATION_SLOT)
#if defined(CONFIG_BOOT_USB_DFU_ERASE_TRAILING_AREA)
	boot_usb_dfu_erase_trailing_area();
#endif

#if defined(CONFIG_BOOT_USB_DFU_DIRECT_PRIMARY)
	boot_usb_dfu_drop_swap_state();
	/* Let the host read the final DFU status before USB goes away */
	k_msleep(2000);
#else
	/* The DFU backend only writes the image into the secondary slot. Unless
	 * the swap is requested here the download is silently ignored on the
	 * next boot.
	 */
	if (boot_set_pending(IS_ENABLED(CONFIG_BOOT_USB_DFU_PERMANENT_DOWNLOAD))) {
		BOOT_LOG_ERR("Failed to request upgrade to the downloaded image");
	}
#endif
#endif
}
