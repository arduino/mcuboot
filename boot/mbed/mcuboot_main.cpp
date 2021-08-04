/*
 * Copyright (c) 2020 Embedded Planet
 * Copyright (c) 2020 ARM Limited
 * SPDX-License-Identifier: Apache-2.0
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
 * limitations under the License
 */

#if MCUBOOT_BOOTLOADER_BUILD

#include <stdlib.h>
#include "bootutil/bootutil.h"
#include "bootutil/image.h"
#include "hal/serial_api.h"
#include "mbed_application.h"

#if (MCUBOOT_CRYPTO_BACKEND == MBEDTLS)
#include "mbedtls/platform.h"
#elif (MCUBOOT_CRYPTO_BACKEND == TINYCRYPT)
#include "tinycrypt/ecc.h"
#endif

#if MBED_CONF_MBED_TRACE_ENABLE
#define MBED_TRACE_MAX_LEVEL TRACE_LEVEL_DEBUG
#define TRACE_GROUP "BL"
#include "mbed-trace/mbed_trace.h"
#else
#include "bootutil/bootutil_log.h"
#endif

#if (MCUBOOT_CRYPTO_BACKEND == TINYCRYPT)
/* XXX add this global definition for linking only
 * TinyCrypt is used for signature verification and ECIES using secp256r1 and AES encryption;
 * RNG is not required. So here we provide a stub.
 * See https://github.com/mcu-tools/mcuboot/pull/791#discussion_r514480098
 */

extern "C" {
int default_CSPRNG(uint8_t *dest, unsigned int size) { return 0; }
}
#endif

MBED_WEAK int target_init(void) {
    return 0;
}

MBED_WEAK int target_loop(void) {
    return 0;
}

MBED_WEAK int target_debug_init(void) {
    return 0;
}

MBED_WEAK int target_led_off(void) {
    return 0;
}

int main()
{
    int rc;

    target_debug_init();

#if MBED_CONF_MBED_TRACE_ENABLE
    mbed_trace_init();
#if MCUBOOT_LOG_BOOTLOADER_ONLY
    mbed_trace_include_filters_set("MCUb,BL");
#endif
#endif

#if MBED_CONF_MBED_TRACE_ENABLE
    tr_info("Starting MCUboot");
#else
    BOOT_LOG_INF("Starting MCUboot");
#endif

#if (MCUBOOT_CRYPTO_BACKEND == MBEDTLS)
    // Initialize mbedtls crypto for use by MCUboot
    mbedtls_platform_context unused_ctx;
    rc = mbedtls_platform_setup(&unused_ctx);
    if(rc != 0) {
#if MBED_CONF_MBED_TRACE_ENABLE
        tr_error("Failed to setup Mbed TLS, error: %d", rc);
#else
        BOOT_LOG_ERR("Failed to setup Mbed TLS, error: %d", rc);
#endif
        exit(rc);
    }
#elif (MCUBOOT_CRYPTO_BACKEND == TINYCRYPT)
    uECC_set_rng(0);
#endif

    rc = target_init();
    if(rc != 0) {
#if  MCUBOOT_APPLICATION_HOOKS
#if MBED_CONF_MBED_TRACE_ENABLE
        tr_info("%s", "Boot process stopped\n");
#else
        BOOT_LOG_INF("%s", "Boot process stopped\n");
#endif
        target_loop();
#else
#if MBED_CONF_MBED_TRACE_ENABLE
        tr_error("Failed to initialize target, error: %d", rc);
#else
        BOOT_LOG_ERR("Failed to initialize target, error: %d", rc);
#endif
        exit(rc);
#endif
    }

    struct boot_rsp rsp;
    rc = boot_go(&rsp);
    if(rc != 0) {
#if MBED_CONF_MBED_TRACE_ENABLE
        tr_error("Failed to locate firmware image, error: %d\n", rc);
#else
        BOOT_LOG_ERR("Failed to locate firmware image, error: %d\n", rc);
#endif
#if  MCUBOOT_APPLICATION_HOOKS
        target_loop();
#else
        exit(rc);
#endif
    }

    uint32_t address = rsp.br_image_off + rsp.br_hdr->ih_hdr_size;

    // Workaround: The extra \n ensures the last trace gets flushed
    // before mbed_start_application() destroys the stack and jumps
    // to the application
#if MBED_CONF_MBED_TRACE_ENABLE
    tr_info("Booting firmware image at 0x%x\n", address);
#else
    BOOT_LOG_INF("Booting firmware image at 0x%x\n", address);
#endif

    target_led_off();

    // Run the application in the primary slot
    // Add header size offset to calculate the actual start address of application
    mbed_start_application(address);
}

#endif // MCUBOOT_BOOTLOADER_BUILD
