/*
 * Copyright (c) 2012-2014 Wind River Systems, Inc.
 * Copyright (c) 2020 Arm Limited
 * Copyright (c) 2021-2023 Nordic Semiconductor ASA
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/timer/system_timer.h>
#include <soc.h>
#include <zephyr/linker/linker-defs.h>

#include "target.h"
#include "bootutil/bootutil_log.h"

BOOT_LOG_MODULE_DECLARE(mcuboot);

#if defined(CONFIG_BOOT_SERIAL_PIN_RESET) || defined(CONFIG_BOOT_FIRMWARE_LOADER_PIN_RESET)
#include <zephyr/drivers/hwinfo.h>
#endif

#if defined(CONFIG_BOOT_SERIAL_BOOT_MODE) || defined(CONFIG_BOOT_FIRMWARE_LOADER_BOOT_MODE)
#include <zephyr/retention/bootmode.h>
#endif

/* Validate serial recovery configuration */
#ifdef CONFIG_MCUBOOT_SERIAL
#if !defined(CONFIG_BOOT_SERIAL_ENTRANCE_GPIO) && \
    !defined(CONFIG_BOOT_SERIAL_WAIT_FOR_DFU) && \
    !defined(CONFIG_BOOT_SERIAL_BOOT_MODE) && \
    !defined(CONFIG_BOOT_SERIAL_NO_APPLICATION) && \
    !defined(CONFIG_BOOT_SERIAL_PIN_RESET)
#error "Serial recovery selected without an entrance mode set"
#endif
#endif

/* Validate firmware loader configuration */
#ifdef CONFIG_BOOT_FIRMWARE_LOADER
#if !defined(CONFIG_BOOT_FIRMWARE_LOADER_ENTRANCE_GPIO) && \
    !defined(CONFIG_BOOT_FIRMWARE_LOADER_BOOT_MODE) && \
    !defined(CONFIG_BOOT_FIRMWARE_LOADER_NO_APPLICATION) && \
    !defined(CONFIG_BOOT_FIRMWARE_LOADER_PIN_RESET)
#error "Firmware loader selected without an entrance mode set"
#endif
#endif

#ifdef CONFIG_MCUBOOT_INDICATION_LED_BLINK_FADING
#include <zephyr/drivers/pwm.h>

static const struct pwm_dt_spec pwm_led = PWM_DT_SPEC_GET(DT_ALIAS(mcuboot_fade_led0));

#define FADE_DELAY_MS 10
#define FADE_STEPS 50
#define FADE_THREAD_STACK_SIZE 1024

K_THREAD_STACK_DEFINE(fade_led_stack, FADE_THREAD_STACK_SIZE);
static struct k_thread fade_led_thread;
static atomic_t fade_led_running;

static void blink_fade_led(void *arg0, void *arg1, void *arg2) {
  uint32_t pulse_width = 0;
  bool increasing = true;

  ARG_UNUSED(arg0);
  ARG_UNUSED(arg1);
  ARG_UNUSED(arg2);

  if (!pwm_is_ready_dt(&pwm_led)) {
    LOG_ERR("Error: PWM device %s is not ready", pwm_led.dev->name);
    return;
  }

  uint32_t step_size = pwm_led.period / FADE_STEPS;

  while (atomic_get(&fade_led_running)) {
    int ret = pwm_set_pulse_dt(&pwm_led, pulse_width);
    if (ret < 0) {
      LOG_ERR("Error %d: failed to set pulse width", ret);
      break;
    }

    if (increasing) {
      pulse_width += step_size;

      if (pulse_width >= pwm_led.period) {
        pulse_width = pwm_led.period;
        increasing = false;
      }
    } else {
      if (pulse_width <= step_size) {
        pulse_width = 0;
        increasing = true;
      } else {
        pulse_width -= step_size;
      }
    }
    k_msleep(FADE_DELAY_MS);
  }

  int ret = pwm_set_pulse_dt(&pwm_led, 0);
  if (ret < 0) {
    LOG_ERR("Error %d: failed to turn off LED", ret);
  }
}

void io_led_start_blink(void) {
  atomic_set(&fade_led_running, 1);
  k_thread_create(&fade_led_thread, fade_led_stack,
                  K_THREAD_STACK_SIZEOF(fade_led_stack), blink_fade_led, NULL,
                  NULL, NULL, 1, 0, K_NO_WAIT);
}

void io_led_stop_blink(void) {
  atomic_clear(&fade_led_running);
  k_thread_join(&fade_led_thread, K_FOREVER);
}
#endif

#ifdef CONFIG_MCUBOOT_INDICATION_LED

/*
 * The led0 devicetree alias is optional. If present, we'll use it
 * to turn on the LED whenever the button is pressed.
 */
#if DT_NODE_EXISTS(DT_ALIAS(mcuboot_led0))
#define LED0_NODE DT_ALIAS(mcuboot_led0)
#endif

#if DT_NODE_HAS_STATUS(LED0_NODE, okay) && DT_NODE_HAS_PROP(LED0_NODE, gpios)
static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
#else
/* A build error here means your board isn't set up to drive an LED. */
#error "Unsupported board: led0 devicetree alias is not defined"
#endif

void io_led_init(void)
{
    if (!device_is_ready(led0.port)) {
        BOOT_LOG_ERR("Didn't find LED device referred by the LED0_NODE\n");
        return;
    }

    gpio_pin_configure_dt(&led0, GPIO_OUTPUT);
    gpio_pin_set_dt(&led0, 0);
}

void io_led_set(int value)
{
    gpio_pin_set_dt(&led0, value);
}
#endif /* CONFIG_MCUBOOT_INDICATION_LED */

#if defined(CONFIG_BOOT_SERIAL_ENTRANCE_GPIO) || defined(CONFIG_BOOT_USB_DFU_GPIO) || \
    defined(CONFIG_BOOT_FIRMWARE_LOADER_ENTRANCE_GPIO)

#if defined(CONFIG_MCUBOOT_SERIAL)
#define BUTTON_0_DETECT_DELAY CONFIG_BOOT_SERIAL_DETECT_DELAY
#elif defined(CONFIG_BOOT_FIRMWARE_LOADER)
#define BUTTON_0_DETECT_DELAY CONFIG_BOOT_FIRMWARE_LOADER_DETECT_DELAY
#else
#define BUTTON_0_DETECT_DELAY CONFIG_BOOT_USB_DFU_DETECT_DELAY
#endif

#define BUTTON_0_NODE DT_ALIAS(mcuboot_button0)

#if DT_NODE_EXISTS(BUTTON_0_NODE) && DT_NODE_HAS_PROP(BUTTON_0_NODE, gpios)
static const struct gpio_dt_spec button0 = GPIO_DT_SPEC_GET(BUTTON_0_NODE, gpios);
#else
#error "Serial recovery/USB DFU button must be declared in device tree as 'mcuboot_button0'"
#endif

bool io_detect_pin(void)
{
    int rc;
    int pin_active;

    if (!device_is_ready(button0.port)) {
        BOOT_LOG_DBG("GPIO device is not ready.");
        return false;
    }

    rc = gpio_pin_configure_dt(&button0, GPIO_INPUT);
    if (rc != 0) {
        BOOT_LOG_DBG("Failed to initialize boot detect pin.");
        return false;
    }

    rc = gpio_pin_get_dt(&button0);
    pin_active = rc;

    if (rc < 0) {
        BOOT_LOG_DBG("Failed to read boot detect pin.");
        return false;
    }


    if (pin_active) {
        if (BUTTON_0_DETECT_DELAY > 0) {
#ifdef CONFIG_MULTITHREADING
            k_sleep(K_MSEC(50));
#else
            k_busy_wait(50000);
#endif

            /* Get the uptime for debounce purposes. */
            int64_t timestamp = k_uptime_get();

            for(;;) {
                rc = gpio_pin_get_dt(&button0);
                pin_active = rc;
                if (rc < 0) {
                    BOOT_LOG_DBG("Failed to read boot detect pin.");
                    return false;
                }

                /* Get delta from when this started */
                uint32_t delta = k_uptime_get() -  timestamp;

                /* If not pressed OR if pressed > debounce period, stop. */
                if (delta >= BUTTON_0_DETECT_DELAY || !pin_active) {
                    break;
                }

                /* Delay 1 ms */
#ifdef CONFIG_MULTITHREADING
                k_sleep(K_MSEC(1));
#else
                k_busy_wait(1000);
#endif
            }
        }
    }

    return (bool)pin_active;
}
#endif

#if defined(CONFIG_BOOT_SERIAL_PIN_RESET) || defined(CONFIG_BOOT_FIRMWARE_LOADER_PIN_RESET)
bool io_detect_pin_reset(void)
{
    uint32_t reset_cause;
    int rc;

    rc = hwinfo_get_reset_cause(&reset_cause);

    if (rc == 0 && (reset_cause & RESET_PIN)) {
        (void)hwinfo_clear_reset_cause();
        return true;
    }

    return false;
}
#endif

#if defined(CONFIG_BOOT_SERIAL_BOOT_MODE) || defined(CONFIG_BOOT_FIRMWARE_LOADER_BOOT_MODE)
bool io_detect_boot_mode(void)
{
    int32_t boot_mode;

    boot_mode = bootmode_check(BOOT_MODE_TYPE_BOOTLOADER);

    if (boot_mode == 1) {
        /* Boot mode to stay in bootloader, clear status and enter serial
         * recovery mode
         */
        bootmode_clear();

        return true;
    }

    return false;
}
#endif
