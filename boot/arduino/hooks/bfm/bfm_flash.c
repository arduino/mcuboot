/*
 * Copyright (c) 2026 Arduino SA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * On-chip BFM writer — bypasses the Zephyr flash driver (singleton bound to
 * PFM, and its ISR can't fire under irq_lock). DS60001795G constraints:
 * (1) erase stalls ALL panels → sequencer is __ramfunc, (2) SRCADDR must be
 * SRAM → staging buffer, (3) WRKEY is one-shot per CTRLA write.
 */

#include <zephyr/kernel.h>
#include <errno.h>
#include <string.h>

#include <soc.h>
#include <zephyr/devicetree.h>
#include <zephyr/irq.h>
#include <zephyr/sys/util.h>

#include "bfm_flash.h"

#define FCW ((fcw_registers_t *)DT_REG_ADDR_BY_NAME(DT_NODELABEL(nvmctrl), fcw))

/* KEYCODE 0x91C32C in the top 24 bits; the low byte selects what it unlocks. */
#define FCW_WRKEY  0x91C32C01U /* CTRLA, one shot */
#define FCW_CFGKEY 0x91C32C04U /* CTRLB, PWPx, LBWP, UBWP, UOWP, CWP; sticky */

#define NVMOP_QUAD_WRITE 0x2U
#define NVMOP_ROW_WRITE  0x3U
#define NVMOP_PAGE_ERASE 0x4U

#define FCW_INTFLAG_ERR_Msk                                                                        \
	(FCW_INTFLAG_KEYERR_Msk | FCW_INTFLAG_CFGERR_Msk | FCW_INTFLAG_FIFOERR_Msk |               \
	 FCW_INTFLAG_BUSERR_Msk | FCW_INTFLAG_WPERR_Msk | FCW_INTFLAG_OPERR_Msk |                  \
	 FCW_INTFLAG_SECERR_Msk | FCW_INTFLAG_BORERR_Msk | FCW_INTFLAG_WRERR_Msk)

/* DS 31.2.15.1: always PREPG=1 — mixed use yields undefined endurance. */
#define BFM_CTRLA_PREPG FCW_CTRLA_PREPG_Msk

/* k_busy_wait() lives in flash — can't call from __ramfunc mid-erase. */
#define BFM_SPIN_LIMIT 40000000U

/* SRCADDR staging — Row Write sources data over the bus matrix from SRAM. */
static uint8_t bfm_stage[BFM_ROW_SIZE] __aligned(8);

static uint32_t bfm_intflag;

uint32_t bfm_swap_read(void)
{
	return FCW->FCW_SWAP;
}

/* DS 31.2.13.2 sequencer. No calls/rodata/logging — must stay in SRAM. */
static __ramfunc uint32_t fcw_nvmop(uint32_t nvmop, uint32_t addr, uint32_t srcaddr,
				    uint32_t use_src, const uint32_t *data, int *timed_out)
{
	fcw_registers_t *regs = FCW;
	uint32_t spin;
	uint32_t flags;

	*timed_out = 0;

	for (spin = 0; (regs->FCW_STATUS & FCW_STATUS_BUSY_Msk) != 0U; spin++) {
		if (spin >= BFM_SPIN_LIMIT) {
			*timed_out = 1;
			return 0U;
		}
	}

	/* Arbitrate against the Zephyr driver, which takes the same mutex. */
	regs->FCW_MUTEX = FCW_MUTEX_LOCK(1) | FCW_MUTEX_OWNER(1);

	if (use_src != 0U) {
		regs->FCW_SRCADDR = srcaddr;
	}
	if (data != NULL) {
		/* Quad Write sources from DATA[0..7], not SRCADDR. */
		for (uint32_t i = 0; i < 8U; i++) {
			regs->FCW_DATA[i] = data[i];
		}
	}
	regs->FCW_ADDR = addr;

	regs->FCW_INTFLAG = FCW_INTFLAG_Msk; /* write-1-to-clear */

	/* Immediately before CTRLA: writing CTRLA is what clears the key. */
	regs->FCW_KEY = FCW_WRKEY;
	regs->FCW_CTRLA = BFM_CTRLA_PREPG | FCW_CTRLA_NVMOP(nvmop);

	for (spin = 0; (regs->FCW_STATUS & FCW_STATUS_BUSY_Msk) != 0U; spin++) {
		if (spin >= BFM_SPIN_LIMIT) {
			*timed_out = 1;
			return 0U;
		}
	}

	flags = regs->FCW_INTFLAG;

	/*
	 * Clear before returning, so that when interrupts are unlocked the
	 * driver's nvmctrl_isr() does not wake on a DONE that was ours.
	 */
	regs->FCW_INTFLAG = FCW_INTFLAG_Msk;
	regs->FCW_MUTEX = FCW_MUTEX_LOCK(0) | FCW_MUTEX_OWNER(0);

	return flags;
}

static int bfm_nvmop(uint32_t nvmop, uint32_t addr, uint32_t srcaddr, uint32_t use_src,
		     const uint32_t *data)
{
	unsigned int key;
	int timed_out;

	key = irq_lock();
	bfm_intflag = fcw_nvmop(nvmop, addr, srcaddr, use_src, data, &timed_out);
	irq_unlock(key);

	if (timed_out != 0) {
		return -ETIMEDOUT;
	}
	if ((bfm_intflag & FCW_INTFLAG_ERR_Msk) != 0U) {
		return -EIO;
	}
	if ((bfm_intflag & FCW_INTFLAG_DONE_Msk) == 0U) {
		/* No error, no DONE = KEY-protected write silently dropped. */
		return -EACCES;
	}

	return 0;
}

/* LBWP/UBWP follow Lower/Upper Boot regions, not physical panels. */
static int bwp_unlock(bool lower, bool upper)
{
	fcw_registers_t *regs = FCW;
	uint32_t lbwp;
	uint32_t ubwp;
	unsigned int key;
	uint32_t spin;

	for (spin = 0; (regs->FCW_STATUS & FCW_STATUS_BUSY_Msk) != 0U; spin++) {
		if (spin >= BFM_SPIN_LIMIT) {
			return -ETIMEDOUT;
		}
	}

	key = irq_lock();
	regs->FCW_INTFLAG = FCW_INTFLAG_Msk;
	/* One CFGKEY covers both registers -- it is sticky, unlike WRKEY. */
	regs->FCW_KEY = FCW_CFGKEY;
	if (lower) {
		regs->FCW_LBWP = 0U;
	}
	if (upper) {
		regs->FCW_UBWP = 0U;
	}
	lbwp = regs->FCW_LBWP;
	ubwp = regs->FCW_UBWP;
	bfm_intflag = regs->FCW_INTFLAG;
	regs->FCW_INTFLAG = FCW_INTFLAG_Msk;
	irq_unlock(key);

	if ((bfm_intflag & FCW_INTFLAG_ERR_Msk) != 0U) {
		return -EIO;
	}

	/* Plain SFR write, no NVMOP — read-back is the only test. */
	if (lower && ((lbwp & 0xFFFFU) != 0U)) {
		return -EACCES;
	}
	if (upper && ((ubwp & 0xFFFFU) != 0U)) {
		return -EACCES;
	}

	return 0;
}

int bfm_unlock(void)
{
	return bwp_unlock(true, true);
}

int bfm_unlock_spare(void)
{
	return bwp_unlock(false, true);
}

int bfm_erase_page(uint32_t addr)
{
	if ((addr % BFM_PAGE_SIZE) != 0U || addr < BFM_BASE ||
	    addr > (BFM_BASE + BFM_SIZE - BFM_PAGE_SIZE)) {
		return -EINVAL;
	}

	return bfm_nvmop(NVMOP_PAGE_ERASE, addr, 0U, 0U, NULL);
}

/* CFM User Configuration pages — Quad Write for ECC, one 256-bit word at a time. */
static int cfg_addr_ok(uint32_t addr, uint32_t align)
{
	if ((addr % align) != 0U) {
		return 0;
	}

	return (addr >= CFM_UCFG1 && addr < CFM_UCFG1 + CFM_PAGE_SIZE) ||
	       (addr >= CFM_UCFG2 && addr < CFM_UCFG2 + CFM_PAGE_SIZE);
}

int bfm_cfg_unlock(void)
{
	fcw_registers_t *regs = FCW;
	unsigned int key;
	uint32_t cwp;

	key = irq_lock();
	regs->FCW_KEY = FCW_CFGKEY;
	regs->FCW_CWP &= ~(FCW_CWP_UC1WP_Msk | FCW_CWP_UC2WP_Msk);
	cwp = regs->FCW_CWP;
	irq_unlock(key);

	if ((cwp & (FCW_CWP_UC1WP_Msk | FCW_CWP_UC2WP_Msk)) != 0U) {
		return -EACCES;
	}

	return 0;
}

int bfm_cfg_erase_page(uint32_t addr)
{
	if (!cfg_addr_ok(addr, CFM_PAGE_SIZE)) {
		return -EINVAL;
	}

	return bfm_nvmop(NVMOP_PAGE_ERASE, addr, 0U, 0U, NULL);
}

int bfm_cfg_quad_write(uint32_t addr, const uint32_t data[8])
{
	static uint32_t quad[8]; /* DATA[] must be in SRAM */

	if (!cfg_addr_ok(addr, CFM_QUAD_SIZE) || data == NULL) {
		return -EINVAL;
	}

	for (uint32_t i = 0; i < 8U; i++) {
		quad[i] = data[i];
	}

	return bfm_nvmop(NVMOP_QUAD_WRITE, addr, 0U, 0U, quad);
}

/* Stage into SRAM, pad with 0xFF, Row Write. */
static int bfm_stage_and_write(uint32_t addr, const void *src, size_t chunk)
{
	if (chunk < BFM_ROW_SIZE) {
		memset(bfm_stage + chunk, 0xFF, BFM_ROW_SIZE - chunk);
	}
	memcpy(bfm_stage, src, chunk);

	return bfm_nvmop(NVMOP_ROW_WRITE, addr, (uint32_t)(uintptr_t)bfm_stage, 1U, NULL);
}

/* --- streaming writer: accumulates DFU transfers into 1 KB rows,
 *     progressive erase per page, finish() erases the remainder. */

static struct {
	uint32_t base;
	uint32_t limit;
	uint32_t offset;   /* bytes committed to flash */
	uint32_t buffered; /* bytes held in stream_row */
	bool active;
} stream;

static uint8_t stream_row[BFM_ROW_SIZE] __aligned(8);

static int stream_flush_row(size_t len)
{
	uint32_t row_addr = stream.base + stream.offset;
	int ret;

	/* First row of a page: erase it before anything lands in it. */
	if ((stream.offset % BFM_PAGE_SIZE) == 0U) {
		ret = bfm_erase_page(row_addr);
		if (ret != 0) {
			return ret;
		}
	}

	ret = bfm_stage_and_write(row_addr, stream_row, len);
	if (ret != 0) {
		return ret;
	}

	stream.offset += BFM_ROW_SIZE;
	stream.buffered = 0;

	return 0;
}

int bfm_stream_begin(uint32_t base, uint32_t limit)
{
	int ret;

	if ((base % BFM_PAGE_SIZE) != 0U || (limit % BFM_PAGE_SIZE) != 0U || limit == 0U) {
		return -EINVAL;
	}
	if (base < BFM_BASE || (base - BFM_BASE) + limit > BFM_SIZE) {
		return -EINVAL;
	}

	/*
	 * Only the spare region is unprotected. A defect in this writer then
	 * cannot reach the image currently executing -- LBWP stays armed for
	 * the whole transfer.
	 */
	ret = (base == BFM_UPPER_BASE) ? bfm_unlock_spare() : bfm_unlock();
	if (ret != 0) {
		return ret;
	}

	stream.base = base;
	stream.limit = limit;
	stream.offset = 0;
	stream.buffered = 0;
	stream.active = true;

	return 0;
}

int bfm_stream_write(const void *data, size_t len)
{
	const uint8_t *src = data;

	if (!stream.active) {
		return -EPERM;
	}
	if (data == NULL) {
		return -EINVAL;
	}
	if (stream.offset + stream.buffered + len > stream.limit) {
		return -EFBIG;
	}

	while (len > 0U) {
		size_t chunk = MIN(BFM_ROW_SIZE - stream.buffered, len);
		int ret;

		memcpy(&stream_row[stream.buffered], src, chunk);
		stream.buffered += chunk;
		src += chunk;
		len -= chunk;

		if (stream.buffered < BFM_ROW_SIZE) {
			continue;
		}

		ret = stream_flush_row(BFM_ROW_SIZE);
		if (ret != 0) {
			stream.active = false;
			return ret;
		}
	}

	return 0;
}

int bfm_stream_finish(void)
{
	uint32_t next_page;
	int ret;

	if (!stream.active) {
		return -EPERM;
	}

	if (stream.buffered > 0U) {
		ret = stream_flush_row(stream.buffered);
		if (ret != 0) {
			stream.active = false;
			return ret;
		}
	}

	/*
	 * Erase whatever the image did not cover. Without this a shorter image
	 * would inherit the tail of a longer one, which still passes a
	 * vector-table check because that only looks at the front.
	 */
	next_page = ROUND_UP(stream.offset, BFM_PAGE_SIZE);
	while (next_page < stream.limit) {
		ret = bfm_erase_page(stream.base + next_page);
		if (ret != 0) {
			stream.active = false;
			return ret;
		}
		next_page += BFM_PAGE_SIZE;
	}

	stream.active = false;

	return 0;
}

uint32_t bfm_stream_written(void)
{
	return stream.offset;
}

