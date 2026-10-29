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


/* --- DFU exit -- PIC32CK only ---------------------------------------------- */

#if defined(CONFIG_SOC_PIC32CK)

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
#endif /* CONFIG_SOC_PIC32CK */

/*
 * The single mcuboot_status_change() implementation for Arduino boards, and
 * the USB teardown that has to happen before the chainload.
 *
 * This is a standalone Zephyr module injected into MCUboot's build with
 * -DEXTRA_ZEPHYR_MODULES=<this dir> (the pattern MCUboot itself demonstrates in
 * samples/runtime-source/zephyr/hooks), rather than a patch against the vendored
 * mcuboot tree: MCUboot already calls mcuboot_status_change() at every state
 * transition worth acting on, so nothing in its sources has to change and there
 * is nothing to re-apply after a `west update`. Requires
 * CONFIG_MCUBOOT_ACTION_HOOKS=y.
 *
 * MCUboot permits exactly one definition of that callback, which is why the
 * breathing LED (status_led.c) and the USB shutdown below are dispatched from
 * here instead of each owning their own.
 */

#include <zephyr/kernel.h>

#ifdef CONFIG_USB_DEVICE_STACK_NEXT
#include <zephyr/usb/usbd.h>
#include <usb_soft_disconnect.h>
#endif

#include "bootutil/mcuboot_status.h"

#include "hooks.h"

#ifdef CONFIG_USB_DEVICE_STACK_NEXT
#endif

/*
 * Hand the incoming image a USB bus it can enumerate on.
 *
 * MCUboot's do_boot() disables USB only under CONFIG_USB_DEVICE_STACK - the
 * legacy stack - so with the next stack nothing lets go of the bus before
 * the chainload.
 *
 * What matters is not the software state (the incoming image zeroes .bss and
 * re-initialises everything anyway) but the D+ pull-up: chainloading does not
 * reset USBHS, so DPRPU stays asserted, the host never sees a detach/attach
 * edge, and the loader comes up running but invisible on USB.
 *
 * usb_soft_disconnect() is the variant's own helper, shared rather than
 * copied so there is one definition of where the pull-up control lives.
 * Deliberately NOT usbd_disable(): per that header it blocks forever if a bus
 * reset is in flight, which would hang the bootloader instead of booting it.
 * Pending UDC interrupts are not a concern either —
 * CONFIG_MCUBOOT_CLEANUP_ARM_CORE=y disables and clears the NVIC immediately
 * after this returns.
 */
static void usb_teardown(void)
{
	usb_soft_disconnect();
}

/*
 * Bring USB up, but only once recovery has actually been entered.
 *
 * Zephyr's CDC_ACM_SERIAL_INITIALIZE_AT_BOOT registers the class and calls
 * usbd_init() from a SYS_INIT; CONFIG_CDC_ACM_SERIAL_ENABLE_AT_BOOT=n stops
 * it also calling usbd_enable(), which is what asserts the D+ pull-up. Doing
 * that here means the bootloader is only visible on USB when there is
 * something for a host to talk to.
 *
 * It matters because MCUboot sits in the double-reset window for
 * CONFIG_BOOT_SERIAL_DOUBLE_RESET_WINDOW_MS on EVERY boot. Enumerating for
 * that window puts a phantom port on the bus: anything the host sent to it
 * during that window (notably a 1200-bps touch, which only the loader acts
 * on) went nowhere. USB is now only asserted in recovery mode.
 *
 * The context belongs to Zephyr's cdc_acm_serial.c and is static there, so
 * it is reached by iterating the section rather than by name.
 */
static void usb_bringup(void)
{
	STRUCT_SECTION_FOREACH(usbd_context, ctx) {
		(void)usbd_enable(ctx);
	}
}
#else
static inline void usb_teardown(void)
{
}

static inline void usb_bringup(void)
{
}
#endif /* CONFIG_USB_DEVICE_STACK_NEXT */

void mcuboot_status_change(mcuboot_status_type_t status)
{
	switch (status) {
	case MCUBOOT_STATUS_SERIAL_DFU_ENTERED:
	case MCUBOOT_STATUS_USB_DFU_ENTERED:
		/* Before boot_console_init(), which is where MCUboot opens the
		 * CDC-ACM device it is about to serve recovery on. */
		usb_bringup();
		mcuboot_hooks_led_breathe();
		break;

	case MCUBOOT_STATUS_BOOTABLE_IMAGE_FOUND:
		/*
		 * Reported from main() immediately before do_boot(), which is
		 * the last point at which this image is still running.
		 *
		 * With USB brought up only in recovery this is a no-op on the
		 * normal path — the pull-up was never asserted — and the
		 * chainload is that much cleaner for it. Kept because it makes
		 * the handoff correct if USB is ever enabled here again.
		 */
		usb_teardown();
		break;

	/*
	 * Without a bootable image MCUboot would wait for DFU with the watchdog
	 * running and FIH_PANIC after the download. Re-enter through the boot mode
	 * instead: the watchdog stays off, DFU waits indefinitely and a completed
	 * download goes straight on to boot_go().
	 */
	case MCUBOOT_STATUS_NO_BOOTABLE_IMAGE_FOUND:
		/* No bootable image found, enter bootloader mode */
		if (bootmode_set(BOOT_MODE_TYPE_BOOTLOADER) == 0) {
			NVIC_SystemReset();
		}
		break;

	default:
		break;
	}
}
