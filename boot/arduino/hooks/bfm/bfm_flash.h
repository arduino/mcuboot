/*
 * Copyright (c) 2026 Arduino SA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* On-chip BFM writer for PIC32CK. Addresses are absolute (0x08......). */

#ifndef BFM_FLASH_H_
#define BFM_FLASH_H_

#include <stddef.h>
#include <stdint.h>

/* DS 31.2.4.2: 128 KB, two panels, 16 × 4 KB pages each, LBWP/UBWP per page. */
#define BFM_BASE       0x08000000U
#define BFM_SIZE       (128U * 1024U)
#define BFM_LOWER_SIZE (64U * 1024U) /* LBWP covers this; UBWP covers the rest */
#define BFM_PAGE_SIZE  4096U
#define BFM_ROW_SIZE   1024U

#define BFM_UPPER_BASE (BFM_BASE + BFM_LOWER_SIZE)

/* CFM User Configuration pages — do NOT swap with BFM (DS 31.2.17.1). */
#define CFM_UCFG1     0x0A000000U /* panel 1 */
#define CFM_UCFG2     0x0A008000U /* panel 2 */
#define CFM_PAGE_SIZE 4096U
#define CFM_QUAD_SIZE 32U /* one 256-bit ECC word */

/* Unprotect all BFM pages. Returns -EACCES if LOCK bits prevent it. */
int bfm_unlock(void);

/* Unprotect only Upper Boot (the spare slot). */
int bfm_unlock_spare(void);

int bfm_erase_page(uint32_t addr);

uint32_t bfm_swap_read(void);
int bfm_cfg_unlock(void);
int bfm_cfg_erase_page(uint32_t addr);
/* Quad Write one 256-bit ECC word (not Row Write — would program erased words). */
int bfm_cfg_quad_write(uint32_t addr, const uint32_t data[8]);

/* Streaming writer for DFU. begin(base, SIZE not end), progressive erase. */
int bfm_stream_begin(uint32_t base, uint32_t limit);
int bfm_stream_write(const void *data, size_t len);
int bfm_stream_finish(void);
uint32_t bfm_stream_written(void);

#endif /* BFM_FLASH_H_ */
