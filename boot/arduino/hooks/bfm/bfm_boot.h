/*
 * Copyright (c) 2026 Arduino SA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * BFM Dual-Boot slot management (DS 31.2.17.2). Two 64K slots, Boot ROM picks
 * by sequence number stored in each panel's CFM User CFG page. Spare is always
 * at 0x08010000 (FCW honours BFSWAP for writes); CFM does NOT swap.
 */

#ifndef BFM_BOOT_H_
#define BFM_BOOT_H_

#include <stdbool.h>
#include <stdint.h>

#include "bfm_flash.h"

struct bfm_boot_state {
	bool bfswap;           /* SWAP.BFSWAP: 0 = panel 1 is the active slot */
	bool swap_locked;      /* SWAP.BFSLOCK: promotion is impossible until reset */
	uint32_t active_ucfg;  /* User CFG page of the running slot */
	uint32_t spare_ucfg;   /* User CFG page of the slot at 0x08010000 */
	uint16_t active_seq;   /* sequence number of the running slot */
	uint16_t spare_seq;
	bool active_seq_valid; /* true ^ complement == 0xFFFF */
	bool spare_seq_valid;
};

#define BFM_SPARE_SLOT  BFM_UPPER_BASE
#define BFM_SLOT_SIZE   BFM_LOWER_SIZE

void bfm_boot_get_state(struct bfm_boot_state *state);
bool bfm_boot_spare_image_valid(void);
/* Bump spare's sequence number above active. Power-fail safe: old image boots
 * until the new sequence number lands. Does NOT reset. */
int bfm_boot_promote_spare(void);

#endif /* BFM_BOOT_H_ */
