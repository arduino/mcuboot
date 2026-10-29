/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Breathing status LED for MCUboot serial recovery mode, ported from the
 * behaviour of ArduinoCore-zephyr's samples/dfu_boot bootloader so both look
 * the same in the hand.
 *
 * Part of the extra/mcuboot/hooks module; it is driven from that module's
 * mcuboot_status_change() in hooks.c, which see for why the callback does not
 * live here.
 *
 * It owns the LED outright, so build with CONFIG_MCUBOOT_INDICATION_LED=n -
 * that option would otherwise drive the same pin steadily on and fight this.
 */

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>

#include "hooks.h"

#define LED_NODE DT_ALIAS(mcuboot_led0)

#if !DT_NODE_EXISTS(LED_NODE) || !DT_NODE_HAS_PROP(LED_NODE, gpios)
#error "mcuboot-led0 alias with a gpios property is required by the status_led module"
#endif

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED_NODE, gpios);

/*
 * The LED is a plain GPIO with no PWM timer wired to it, so the fade is a
 * software PWM. Unlike dfu_boot - which can busy-wait, because there it runs
 * in main() with USB serviced by interrupts underneath - MCUboot's recovery
 * command loop (boot_serial_read_console()) never blocks in a way that would
 * yield to a lower-priority thread. So this runs at a *cooperative* priority
 * (above main's) and *sleeps* between edges: it preempts the command loop for
 * the few microseconds each edge takes and is descheduled the rest of the
 * time, costing almost no CPU and never stalling recovery.
 */
#define BREATHE_CYCLE_MS   2000U /* one full fade in + fade out */
#define BREATHE_CARRIER_US 4000U /* 250 Hz, well above visible flicker */

/*
 * Brightness resolution is capped by the system tick: sleeping (rather than
 * busy-waiting) cannot resolve a sub-tick pulse.
 */
#define TICK_US        (USEC_PER_SEC / CONFIG_SYS_CLOCK_TICKS_PER_SEC)
#define BREATHE_LEVELS (BREATHE_CARRIER_US / TICK_US)

BUILD_ASSERT(BREATHE_LEVELS >= 8,
	     "system tick too slow to fade smoothly; raise "
	     "CONFIG_SYS_CLOCK_TICKS_PER_SEC or BREATHE_CARRIER_US");

#define BREATHE_STACK_SIZE 768

static K_THREAD_STACK_DEFINE(breathe_stack, BREATHE_STACK_SIZE);
static struct k_thread breathe_thread_data;
static bool breathing;

static void breathe_thread(void *p1, void *p2, void *p3)
{
	/*
	 * Carrier periods to hold each brightness level, so that ramping up
	 * and back down takes BREATHE_CYCLE_MS in total.
	 */
	uint32_t reps = ((BREATHE_CYCLE_MS * USEC_PER_MSEC) / 2U) /
			(BREATHE_LEVELS * BREATHE_CARRIER_US);

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	if (reps == 0U) {
		reps = 1U;
	}

	while (true) {
		/* rising: 0 -> LEVELS (fade in), then LEVELS -> 0 (fade out) */
		for (int rising = 1; rising >= 0; rising--) {
			for (uint32_t step = 0; step <= BREATHE_LEVELS; step++) {
				uint32_t level = rising ? step : (BREATHE_LEVELS - step);
				uint32_t on_us = level * TICK_US;
				uint32_t off_us = BREATHE_CARRIER_US - on_us;

				for (uint32_t rep = 0; rep < reps; rep++) {
					if (on_us > 0U) {
						(void)gpio_pin_set_dt(&led, 1);
						k_usleep(on_us);
					}
					if (off_us > 0U) {
						(void)gpio_pin_set_dt(&led, 0);
						k_usleep(off_us);
					}
				}
			}
		}
	}
}

void mcuboot_hooks_led_breathe(void)
{
	if (breathing) {
		return;
	}

	if (!gpio_is_ready_dt(&led) ||
	    gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE) != 0) {
		return;
	}

	breathing = true;

	k_thread_create(&breathe_thread_data, breathe_stack,
			K_THREAD_STACK_SIZEOF(breathe_stack), breathe_thread,
			NULL, NULL, NULL,
			/* Just above main(), so waking to drive an edge always
			 * preempts the recovery command loop. */
			K_PRIO_COOP(CONFIG_NUM_COOP_PRIORITIES - 1), 0, K_NO_WAIT);
	k_thread_name_set(&breathe_thread_data, "mcuboot_led");
}
