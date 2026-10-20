/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB DFU entrance and exit for MCUboot on the Nano XL Link, as a standalone
 * Zephyr module (-DEXTRA_ZEPHYR_MODULES). Requires CONFIG_MCUBOOT_ACTION_HOOKS,
 * CONFIG_BOOT_GO_HOOKS and CONFIG_BOOT_USB_DFU_BOOT_MODE.
 *
 * A double tap, the loader's 1200-bps touch and a missing bootable image all
 * end up in the retention boot mode, which MCUboot turns into USB DFU.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/retained_mem.h>
#include <zephyr/retention/bootmode.h>
#include <zephyr/sys/sys_io.h>
#include <cmsis_core.h>

#include "bootutil/mcuboot_status.h"
#include "bootutil/boot_hooks.h"
#include "bootutil/fault_injection_hardening.h"

/* Pin slot addresses so drift between the variant's mcuboot.overlay and its
 * -common.overlay is a build error. */
#define SLOT_ABS(label) \
	(DT_REG_ADDR(DT_GPARENT(DT_NODELABEL(label))) + DT_REG_ADDR(DT_NODELABEL(label)))

BUILD_ASSERT(SLOT_ABS(slot0_partition) == 0x0C000000,
	     "slot0 moved: keep the variant's mcuboot.overlay and -common.overlay in sync");
BUILD_ASSERT(SLOT_ABS(slot1_partition) == 0x0C100000,
	     "slot1 moved: keep the variant's mcuboot.overlay and -common.overlay in sync");

/* --- Double tap -------------------------------------------------------------- */

#define DOUBLE_TAP_WINDOW_MS 500
#define DOUBLE_TAP_OFFSET    0

static const uint8_t double_tap_armed[] = {0x44, 0x54, 0x41, 0x50}; /* "DTAP" */
static const uint8_t double_tap_clear[sizeof(double_tap_armed)];

static const struct device *const retained_ram = DEVICE_DT_GET(DT_NODELABEL(retained_ram));
static bool tapped;
static bool dfu_session; /* this boot waits in USB DFU */

/*
 * Everything between the reset and the moment the cookie is armed is a dead
 * zone where a second tap goes unnoticed, and on this SoC clock bring-up alone
 * takes hundreds of milliseconds. So arm before it, through the retained-RAM
 * driver (CONFIG_RETAINED_MEM_INIT_PRIORITY=0).
 */
static int double_tap_arm(void)
{
	uint8_t value[sizeof(double_tap_armed)];

	(void)retained_mem_read(retained_ram, DOUBLE_TAP_OFFSET, value, sizeof(value));
	tapped = (memcmp(value, double_tap_armed, sizeof(value)) == 0);
	(void)retained_mem_write(retained_ram, DOUBLE_TAP_OFFSET,
				 tapped ? double_tap_clear : double_tap_armed, sizeof(value));

	return 0;
}
SYS_INIT(double_tap_arm, PRE_KERNEL_1, 1);
BUILD_ASSERT(CONFIG_RETAINED_MEM_INIT_PRIORITY < 1,
	     "the retained-RAM driver must be up before double_tap_arm()");

/* Before main(), which is where MCUboot reads (and clears) the boot mode. */
static int double_tap_check(void)
{
	if (tapped) {
		(void)bootmode_set(BOOT_MODE_TYPE_BOOTLOADER);
	} else {
		/* Hold the window open so a second tap finds the cookie armed. */
		k_msleep(DOUBLE_TAP_WINDOW_MS);
		(void)retained_mem_write(retained_ram, DOUBLE_TAP_OFFSET, double_tap_clear,
					 sizeof(double_tap_clear));
	}

	dfu_session = (bootmode_check(BOOT_MODE_TYPE_BOOTLOADER) == 1);

	return 0;
}
SYS_INIT(double_tap_check, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/* --- DFU exit ---------------------------------------------------------------- */

/* USBHS_POWER.SOFTCONN survives NVIC_SystemReset(): drop it so the host sees
 * the detach. usbd_disable() cannot be used, it blocks on a bus reset. */
#define USBHS_POWER_ADDR (DT_REG_ADDR(DT_NODELABEL(zephyr_udc0)) + 0x1001U)
#define USBHS_SOFTCONN   BIT(6)

static void usb_detach_and_reset(void)
{
	sys_write8(sys_read8(USBHS_POWER_ADDR) & ~USBHS_SOFTCONN, USBHS_POWER_ADDR);
	__DSB();
	k_busy_wait(20000);

	NVIC_SystemReset();
}

/*
 * Reached right after a USB DFU session ends (boot-mode DFU waits forever, so
 * it only returns on a completed download). Chainloading now would hand the
 * loader a USB controller MCUboot left configured, and its UDC driver stops
 * answering SET_ADDRESS. The download is already marked pending, so reset
 * instead: the next boot installs it with USB untouched. This also keeps the
 * old bootloader from chainloading after a BFM update.
 */
fih_ret boot_go_hook(struct boot_rsp *rsp)
{
	ARG_UNUSED(rsp);

	if (dfu_session) {
		/* Let the host finish with the device before the bus drops. */
		k_msleep(2000);
		usb_detach_and_reset();
	}

	FIH_RET(FIH_BOOT_HOOK_REGULAR);
}

/*
 * Without a bootable image MCUboot would wait for DFU with the watchdog
 * running and FIH_PANIC after the download. Re-enter through the boot mode
 * instead: the watchdog stays off, DFU waits indefinitely and a completed
 * download goes straight on to boot_go().
 */
void mcuboot_status_change(mcuboot_status_type_t status)
{
	if (status == MCUBOOT_STATUS_NO_BOOTABLE_IMAGE_FOUND &&
	    bootmode_set(BOOT_MODE_TYPE_BOOTLOADER) == 0) {
		NVIC_SystemReset();
	}
}
