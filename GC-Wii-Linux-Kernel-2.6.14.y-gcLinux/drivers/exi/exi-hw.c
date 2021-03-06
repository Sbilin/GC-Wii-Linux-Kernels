/*
 * drivers/exi/exi-hw.c
 *
 * Nintendo GameCube EXpansion Interface support. Hardware routines.
 * Copyright (C) 2004-2005 The GameCube Linux Team
 * Copyright (C) 2004,2005 Todd Jeffreys <todd@voidpointer.org>
 * Copyright (C) 2005 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

/*
 * IMPLEMENTATION NOTES
 *
 * The EXI Layer provides the following primitives:
 *
 * 	op			atomic?
 * 	------------------	-------
 * 	select			yes
 * 	deselect		yes
 * 	transfer		yes/no (1)
 *
 * These primitives are encapsulated in several APIs.
 * Some APIs are backwards-compatible with the old gcn-exi-lite framework.
 * See include/linux/exi.h for additional information.
 *
 * 1. Kernel Contexts
 *
 * User, softirq and hardirq contexts are supported, with some limitations.
 *
 * Launching EXI operations in softirq or hardirq context requires kernel
 * coordination to ensure channels are free before use.
 *
 * The EXI Layer Event System delivers events in softirq context, but it already
 * makes provisions to ensure that channels are useable by the event handlers.
 * Events are delivered only when the channels on the event handler
 * channel mask are all deselected. This allows one to run EXI commands in
 * softirq context from the EXI event handlers.
 *
 * "select" operations in user context will sleep if necessary until the
 * channel gets deselected. 
 *
 *
 * 2. Transfers
 * 
 * The EXI Layer provides a transfer API to perform read and write
 * operations.
 * By default, transfers partially or totally suitable for DMA will be
 * partially or totally processed through DMA. The EXI Layer takes care of 
 * splitting a transfer in several pieces so the best transfer method is
 * used each time.
 *
 * (1) A immediate mode transfer is atomic, but a DMA transfer is not.
 */

//#define EXI_DEBUG 1

#include <linux/types.h>
#include <linux/dma-mapping.h>
#include <linux/wait.h>
#include <linux/delay.h>
#include <asm/io.h>

#include <linux/exi.h>
#include "exi-hw.h"


#ifdef EXI_DEBUG
#  define DBG(fmt, args...) \
	  printk(KERN_ERR "%s: " fmt, __FUNCTION__ , ## args)
#else
#  define DBG(fmt, args...)
#endif

extern wait_queue_head_t exi_bus_waitq;

static void exi_tasklet(unsigned long param);

/* for compatibility with the old exi-lite framework */
unsigned long exi_running = 0;

/*
 * These are the available exi channels.
 */
static struct exi_channel exi_channels[EXI_MAX_CHANNELS] = {
	[0] = {
		.channel = 0,
		.lock = SPIN_LOCK_UNLOCKED,
		.io_lock = SPIN_LOCK_UNLOCKED,
		.io_base = EXI_IO_BASE(0),
		.select_lock = SPIN_LOCK_UNLOCKED,
		.wait_queue = __WAIT_QUEUE_HEAD_INITIALIZER(
				exi_channels[0].wait_queue),
	},
	[1] = {
		.channel = 1,
		.lock = SPIN_LOCK_UNLOCKED,
		.io_lock = SPIN_LOCK_UNLOCKED,
		.io_base = EXI_IO_BASE(1),
		.select_lock = SPIN_LOCK_UNLOCKED,
		.wait_queue = __WAIT_QUEUE_HEAD_INITIALIZER(
				exi_channels[1].wait_queue),
	},
	[2] = {
		.channel = 2,
		.lock = SPIN_LOCK_UNLOCKED,
		.io_lock = SPIN_LOCK_UNLOCKED,
		.io_base = EXI_IO_BASE(2),
		.select_lock = SPIN_LOCK_UNLOCKED,
		.wait_queue = __WAIT_QUEUE_HEAD_INITIALIZER(
				exi_channels[2].wait_queue),
	},
};

/* handy iterator for exi channels */
#define exi_channel_for_each(pos) \
	for(pos = &exi_channels[0]; pos < &exi_channels[EXI_MAX_CHANNELS]; \
		pos++)

/* conversions between channel numbers and exi channel structures */
#define __to_exi_channel(channel) (&exi_channels[channel])
#define __to_channel(exi_channel) (exi_channel->channel)

/**
 *	to_exi_channel  -  returns an exi_channel given a channel number
 *	@channel:	channel number
 *
 *	Return the exi_channel structure associated to a given channel.
 */
struct exi_channel *to_exi_channel(unsigned int channel)
{
	if (channel > EXI_MAX_CHANNELS)
		return NULL;

	return __to_exi_channel(channel);
}

/**
 *	to_channel  -  returns a channel number given an exi channel
 *	@exi_channel:	channel
 *
 *	Return the channel number for a given exi_channel structure.
 */
unsigned int to_channel(struct exi_channel *exi_channel)
{
	BUG_ON(exi_channel == NULL);

	return __to_channel(exi_channel);
}

/*
 * Internal. Initialize an exi_channel structure.
 */
void exi_channel_init(struct exi_channel *exi_channel, unsigned int channel)
{
	memset(exi_channel, 0, sizeof(*exi_channel));

	exi_channel->channel = channel;
	spin_lock_init(&exi_channel->lock);
	spin_lock_init(&exi_channel->select_lock);
	spin_lock_init(&exi_channel->io_lock);
	exi_channel->io_base = EXI_IO_BASE(channel);
	init_waitqueue_head(&exi_channel->wait_queue);

	tasklet_init(&exi_channel->tasklet,
		     exi_tasklet, (unsigned long)exi_channel);
}


/**
 *	exi_select_raw  -  selects a device on an exi channel
 *	@exi_channel:	channel
 *	@device:	device number on channel
 *	@freq:		clock frequency index
 *
 *	Select a given device on a specified EXI channel by setting its
 *	CS line, and use the specified clock frequency when doing transfers.
 */
void exi_select_raw(struct exi_channel *exi_channel, unsigned int device,
		   unsigned int freq)
{
	u32 __iomem *csr_reg = exi_channel->io_base + EXI_CSR;
	u32 csr;
	unsigned long flags;

	BUG_ON(device > EXI_DEVICES_PER_CHANNEL ||
	       freq > EXI_MAX_FREQ);

	/*
	 * Preserve interrupt masks while setting the CS line bits.
	 */
	spin_lock_irqsave(&exi_channel->io_lock, flags);
	csr = readl(csr_reg);
	csr &= (EXI_CSR_EXTINMASK | EXI_CSR_TCINTMASK | EXI_CSR_EXIINTMASK);
	csr |= ((1<<device) << 7) | (freq << 4);
	writel(csr, csr_reg);
	spin_unlock_irqrestore(&exi_channel->io_lock, flags);
}


/**
 *	exi_deselect_raw  -  deselects all devices on an exi channel
 *	@exi_channel:	channel
 *
 *	Deselect any device previously selected on the specified EXI
 *	channel by unsetting all CS lines.
 */
void exi_deselect_raw(struct exi_channel *exi_channel)
{
	u32 __iomem *csr_reg = exi_channel->io_base + EXI_CSR;
	u32 csr;
	unsigned long flags;

	/*
	 * Preserve interrupt masks while clearing the CS line bits.
	 */
	spin_lock_irqsave(&exi_channel->io_lock, flags);
	csr = readl(csr_reg);
	csr &= (EXI_CSR_EXTINMASK | EXI_CSR_TCINTMASK | EXI_CSR_EXIINTMASK);
	writel(csr, csr_reg);
	spin_unlock_irqrestore(&exi_channel->io_lock, flags);
}

/**
 *	exi_transfer_raw  -  performs an exi transfer using immediate mode
 *	@exi_channel:	channel
 *	@data:		pointer to data being read/writen
 *	@len:		length of data
 *	@mode:		direction of transfer (EXI_OP_READ or EXI_OP_WRITE)
 *
 *	Read or write data on a given EXI channel.
 *
 */
void exi_transfer_raw(struct exi_channel *exi_channel,
		      void *data, size_t len, int mode)
{
	while(len >= 4) {
		__exi_transfer_raw_u32(exi_channel, data, mode);
		data += 4;
		len -= 4;
	}

	switch(len) {
	case 1:
		__exi_transfer_raw_u8(exi_channel, data, mode);
		break;
	case 2:
		__exi_transfer_raw_u16(exi_channel, data, mode);
		break;
	case 3:
		/* XXX optimize this case */
		__exi_transfer_raw_u16(exi_channel, data, mode);
		__exi_transfer_raw_u8(exi_channel, data+2, mode);
		break;
	default:
		break;
	}
}

/*
 * Internal. Start a transfer using "interrupt-driven immediate" mode.
 */
static void exi_start_idi_transfer_raw(struct exi_channel *exi_channel,
				       void *data, size_t len, int mode)
{
	void __iomem *io_base = exi_channel->io_base;
	u32 __iomem *csr_reg = io_base + EXI_CSR;
	u32 val = ~0;
	unsigned long flags;

	BUG_ON(len < 1 || len > 4);

	if ((mode & EXI_OP_WRITE)) {
		switch(len) {
			case 1:
				val = *((u8*)data) << 24;
				break;
			case 2:
				val = *((u16*)data) << 16;
				break;
			case 3:
				val = *((u16*)data) << 16;
				val |= *((u8*)data+2) << 8;
				break;
			case 4:
				val = *((u32*)data);
				break;
			default:
				break;
		}
	}

	writel(val, io_base + EXI_DATA);

	/* enable the Transfer Complete interrupt */
	spin_lock_irqsave(&exi_channel->io_lock, flags);
	writel(readl(csr_reg) | EXI_CSR_TCINTMASK, csr_reg);
	spin_unlock_irqrestore(&exi_channel->io_lock, flags);

	/* start the transfer */
	writel(EXI_CR_TSTART | EXI_CR_TLEN(len) | (mode&0xf), io_base + EXI_CR);
}

/*
 * Internal. Finish a transfer using "interrupt-driven immediate" mode.
 */
static void exi_end_idi_transfer_raw(struct exi_channel *exi_channel,
				     void *data, size_t len, int mode)
{
	void __iomem *io_base = exi_channel->io_base;
	u32 val = ~0;

	BUG_ON(len < 1 || len > 4);

	if ((mode&0xf) != EXI_OP_WRITE) {
		val = readl(io_base + EXI_DATA);
		switch(len) {
			case 1:
				*((u8*)data) = (u8)(val >> 24);
				break;
			case 2:
				*((u16*)data) = (u16)(val >> 16);
				break;
			case 3:
				*((u16*)data) = (u16)(val >> 16);
				*((u8*)data+2) = (u8)(val >> 8);
				break;
			case 4:
				*((u32*)data) = (u32)(val);
				break;
			default:
				break;
		}
	}
}

/*
 * Internal. Start a transfer using DMA mode.
 */
static void exi_start_dma_transfer_raw(struct exi_channel *exi_channel,
				       dma_addr_t data, size_t len, int mode)
{
	void __iomem *io_base = exi_channel->io_base;
	u32 __iomem *csr_reg = io_base + EXI_CSR;
	unsigned long flags;

	BUG_ON((data & EXI_DMA_ALIGN) != 0 ||
	       (len & EXI_DMA_ALIGN) != 0);

	/*
	 * We clear the DATA register here to avoid confusing some
	 * special hardware, like SD cards.
	 */
	writel(~0, io_base + EXI_DATA);

	/* setup address and length of transfer */
	writel(data, io_base + EXI_MAR);
	writel(len, io_base + EXI_LENGTH);

	/* enable the Transfer Complete interrupt */
	spin_lock_irqsave(&exi_channel->io_lock, flags);
	writel(readl(csr_reg) | EXI_CSR_TCINTMASK, csr_reg);
	spin_unlock_irqrestore(&exi_channel->io_lock, flags);

	/* start the transfer */
	writel(EXI_CR_TSTART | EXI_CR_DMA | (mode&0xf), io_base + EXI_CR);
}


/*
 * Internal. Busy-wait until a DMA mode transfer operation completes.
 */
static void exi_wait_for_transfer_raw(struct exi_channel *exi_channel)
{
	u32 __iomem *cr_reg = exi_channel->io_base + EXI_CR;
	u32 __iomem *csr_reg = exi_channel->io_base + EXI_CSR;
	unsigned long flags;

	/* we don't want TCINTs to disturb us while waiting */
	spin_lock_irqsave(&exi_channel->io_lock, flags);
	writel(readl(csr_reg) & ~EXI_CSR_TCINTMASK, csr_reg);
	spin_unlock_irqrestore(&exi_channel->io_lock, flags);

	/* busy-wait for transfer complete */
	while(readl(cr_reg) & EXI_CR_TSTART)
		cpu_relax();

	/* ack the Transfer Complete interrupt */
	spin_lock_irqsave(&exi_channel->io_lock, flags);
	writel(readl(csr_reg) | EXI_CSR_TCINT, csr_reg);
	spin_unlock_irqrestore(&exi_channel->io_lock, flags);
}

static void exi_command_done(struct exi_command *cmd);

/*
 * Internal. Check if an exi channel has delayed work to do.
 */
static void exi_check_pending_work(void)
{
	struct exi_channel *exi_channel;

	exi_channel_for_each(exi_channel) {
		if (exi_channel->csr) {
			tasklet_schedule(&exi_channel->tasklet);
		}
	}
}

/*
 * Internal. Finish a DMA transfer.
 * Caller holds the channel lock.
 */
static void exi_end_dma_transfer(struct exi_channel *exi_channel)
{
	struct exi_command *cmd;

	cmd = exi_channel->queued_cmd;
	if (cmd) {
		BUG_ON(!(exi_channel->flags & EXI_DMABUSY));

		exi_channel->flags &= ~EXI_DMABUSY;
		dma_unmap_single(&exi_channel->device_selected->dev,
				 cmd->dma_addr, cmd->dma_len,
				 (cmd->opcode == EXI_OP_READ)?
				 DMA_FROM_DEVICE:DMA_TO_DEVICE);

		exi_channel->queued_cmd = NULL;
	}
}

/*
 * Internal. Finish an "interrupt-driven immediate" transfer.
 * Caller holds the channel lock.
 *
 * If more data is pending transfer, schedules a new transfer.
 * Returns zero if no more transfers are required, non-zero otherwise.
 *
 */
static int exi_end_idi_transfer(struct exi_channel *exi_channel)
{
	struct exi_command *cmd;
	int len, offset;
	unsigned int balance = 16 /* / sizeof(u32) */;

	cmd = exi_channel->queued_cmd;
	if (cmd) {
		BUG_ON((exi_channel->flags & EXI_DMABUSY));

		len = (cmd->bytes_left > 4)?4:cmd->bytes_left;
		offset = cmd->len - cmd->bytes_left;
		exi_end_idi_transfer_raw(exi_channel,
					 cmd->data + offset, len,
					 cmd->opcode);
		cmd->bytes_left -= len;

		if (balance && cmd->bytes_left > 0) {
			offset += len;
			len = (cmd->bytes_left > balance)?
					balance:cmd->bytes_left;
			exi_transfer_raw(exi_channel,
					 cmd->data + offset, len, cmd->opcode);
			cmd->bytes_left -= len;
		}

		if (cmd->bytes_left > 0) {
			offset = cmd->len - cmd->bytes_left;
			len = (cmd->bytes_left > 4)?4:cmd->bytes_left;

			exi_start_idi_transfer_raw(exi_channel,
						   cmd->data + offset, len,
						   cmd->opcode);
		} else {
			exi_channel->queued_cmd = NULL;
		}
	}

	return (exi_channel->queued_cmd)?1:0;
}

/*
 * Internal. Wait until a single transfer completes, and launch callbacks
 * when the whole transfer is completed.
 */
static int exi_wait_for_transfer_one(struct exi_channel *exi_channel)
{
	struct exi_command *cmd;
	unsigned long flags;
	int pending = 0;

	spin_lock_irqsave(&exi_channel->lock, flags);

	exi_wait_for_transfer_raw(exi_channel);

	cmd = exi_channel->queued_cmd;
	if (cmd) {
		if ((exi_channel->flags & EXI_DMABUSY)) {
			/* dma transfers need just one transfer */
			exi_end_dma_transfer(exi_channel);
		} else {
			pending = exi_end_idi_transfer(exi_channel);
		}

		spin_unlock_irqrestore(&exi_channel->lock, flags);

		if (!pending)
			exi_command_done(cmd);
		goto out;
	}

	spin_unlock_irqrestore(&exi_channel->lock, flags);
out:
	return pending;
}

/*
 * Internal. Wait until a full transfer completes and launch callbacks.
 */
static void exi_wait_for_transfer(struct exi_channel *exi_channel)
{
	while(exi_wait_for_transfer_one(exi_channel))
		cpu_relax();
}

/*
 * Internal. Call any done hooks.
 */
static void exi_command_done(struct exi_command *cmd)
{
	/* if specified, call the completion routine */
	if (cmd->done) {
		cmd->done(cmd);
	}
}

/*
 * Internal. Perform a select.
 * Caller holds the channel lock.
 */
static inline int exi_cmd_select(struct exi_command *cmd)
{
	struct exi_channel *exi_channel = cmd->exi_channel;
	struct exi_device *exi_device;
	int retval = 0;

	BUG_ON(cmd->data == NULL);
	if (unlikely(exi_is_selected(exi_channel))) {
		/*
		 * This may happen when called directly from interrupt context,
		 * without using the EXI event handler system.
		 * In such cases, the caller _should_ be able to deal with that.
		 */
		retval = -EBUSY;
		goto out;
	}

	spin_lock(&exi_channel->select_lock);

	/* cmd->data contains the device to select */
	exi_device = cmd->data;

	exi_channel->device_selected = exi_device;
	exi_channel->flags |= EXI_SELECTED;

	DBG("channel=%d, device=%d, freq=%d\n",
	    exi_channel->channel, exi_device->eid.device,
	    exi_device->frequency);

	exi_select_raw(exi_channel, exi_device->eid.device,
		       exi_device->frequency);
out:
	return retval;
}

/*
 * Internal. Perform a deselect.
 * Caller holds the channel lock.
 */
static inline void exi_cmd_deselect(struct exi_command *cmd)
{
	struct exi_channel *exi_channel = cmd->exi_channel;

	WARN_ON(!exi_is_selected(exi_channel));

	DBG("channel=%d\n", exi_channel->channel);

	exi_deselect_raw(exi_channel);

	exi_channel->flags &= ~EXI_SELECTED;
	exi_channel->device_selected = NULL;

	spin_unlock(&exi_channel->select_lock);
}

/*
 * Internal. Perform the post non-DMA transfer associated to a DMA transfer.
 */
static void exi_cmd_post_transfer(struct exi_command *cmd)
{
	struct exi_channel *exi_channel = cmd->exi_channel;
	struct exi_command *post_cmd = &exi_channel->post_cmd;

	DBG("channel=%d\n", exi_channel->channel);

	exi_transfer_raw(exi_channel, post_cmd->data, post_cmd->len,
			 post_cmd->opcode);

	cmd->done_data = post_cmd->done_data;
	cmd->done = post_cmd->done;
	exi_op_nop(post_cmd, exi_channel);
	exi_command_done(cmd);
}


#define exi_align_next(x) (void *) \
			  (((unsigned long)(x)+EXI_DMA_ALIGN)&~EXI_DMA_ALIGN)
#define exi_align_prev(x) (void *) \
			  ((unsigned long)(x)&~EXI_DMA_ALIGN)
#define exi_is_aligned(x) (void *) \
			  (!((unsigned long)(x)&EXI_DMA_ALIGN))

/*
 * Internal. Perform a transfer.
 * Caller holds the channel lock.
 */
static int exi_cmd_transfer(struct exi_command *cmd)
{
	static u8 exi_aligned_transfer_buf[EXI_DMA_ALIGN+1]
				 __attribute__ ((aligned (EXI_DMA_ALIGN+1)));
	struct exi_channel *exi_channel = cmd->exi_channel;
	struct exi_command *post_cmd = &exi_channel->post_cmd;
	void *pre_data, *data, *post_data;
	unsigned int pre_len, len, post_len;
	int opcode;
	int retval = 0;

	BUG_ON(!exi_is_selected(exi_channel));

	len = cmd->len;
	if (!len)
		goto done;

	DBG("channel=%d, opcode=%d\n", exi_channel->channel, cmd->opcode);

	opcode = cmd->opcode;
	data = cmd->data;

	/* interrupt driven immediate transfer... */
	if ((cmd->flags & EXI_CMD_IDI)) {
		exi_channel->queued_cmd = cmd;
		exi_channel->flags &= ~EXI_DMABUSY;

		cmd->bytes_left = cmd->len;
		len = (cmd->bytes_left > 4)?4:cmd->bytes_left;
		exi_start_idi_transfer_raw(exi_channel, data, len, opcode);

		retval = 1; /* wait */
		goto done;
	}

	/*
	 * We can't do DMA transfers unless we have at least 32 bytes.
	 * And we won't do DMA transfers if user requests that.
	 */
	if (len < EXI_DMA_ALIGN+1 || (cmd->flags & EXI_CMD_NODMA)) {
		exi_transfer_raw(exi_channel, data, len, opcode);
		goto done;
	}

	/*
	 * |_______________|______...______|_______________| DMA alignment
	 *     <--pre_len--><---- len -----><-post_len->
	 *     +-----------+------...------+-----------+
	 *     | pre_data  | data          | post_data |
	 *     | non-DMA   | DMA           | non-DMA   |
	 *     +-----------+------...------+-----------+
	 *       < 32 bytes  N*32 bytes      < 32 bytes
	 *     |<--------->|<-----...----->|<--------->|
	 *     <-------------- cmd->len --------------->
	 */

	pre_data = data;
	post_data = exi_align_prev(pre_data+len);
	data = exi_align_next(pre_data);

	pre_len = data - pre_data;
	post_len = (pre_data + len) - post_data;
	len = post_data - data;

	/*
	 * Coalesce pre and post data transfers if no DMA transfer is possible.
	 */
	if (!len) {
		/*
		 * Maximum transfer size here is 31+31=62 bytes.
		 */

		/*
		 * On transfer sizes greater than or equal to 32 bytes
		 * we can optimize the transfer by performing a 32-byte
		 * DMA transfer using a specially aligned temporary buffer,
		 * followed by a non-DMA transfer for the remaining bytes.
		 */
		if ( pre_len + post_len > EXI_DMA_ALIGN ) {
			post_len = pre_len + post_len - (EXI_DMA_ALIGN+1);
			post_data = pre_data + EXI_DMA_ALIGN+1;
			len = EXI_DMA_ALIGN+1;
			data = exi_aligned_transfer_buf;
			memcpy(data, pre_data, EXI_DMA_ALIGN+1);
			pre_len = 0;
		} else {
			exi_transfer_raw(exi_channel, pre_data,
					 pre_len + post_len, opcode);
			goto done;
		}
	}

	/*
	 * The first unaligned chunk can't use DMA.
	 */
	if (pre_len > 0) {
		/*
		 * Maximum transfer size here is 31 bytes.
		 */
		exi_transfer_raw(exi_channel, pre_data, pre_len, opcode);
	}

	/*
	 * Perform a DMA transfer on the aligned data, followed by a non-DMA
	 * data transfer on the remaining data.
	 */
	if (post_len > 0) {
		/*
		 * Maximum transfer size here will be 31 bytes.
		 */
		exi_op_transfer(post_cmd, exi_channel,
				post_data, post_len, opcode);
		post_cmd->done_data = cmd->done_data;
		post_cmd->done = cmd->done;
		cmd->done_data = NULL;
		cmd->done = exi_cmd_post_transfer;
	}

	exi_channel->queued_cmd = cmd;
	exi_channel->flags |= EXI_DMABUSY;

	cmd->dma_len = len;
	cmd->dma_addr = dma_map_single(&exi_channel->device_selected->dev,
				       data, len,
				       (cmd->opcode == EXI_OP_READ)?
				       DMA_FROM_DEVICE:DMA_TO_DEVICE);

	exi_start_dma_transfer_raw(exi_channel, cmd->dma_addr, len, opcode);

	retval = 1; /* wait */

done:
	return retval;
}

/**
 *	exi_run_command  -  executes a single exi command
 *	@cmd:	the command to execute
 *
 *	Context: user, softirq, hardirq
 *
 *	Run just one command.
 *
 */
static int exi_run_command(struct exi_command *cmd)
{
	struct exi_channel *exi_channel = cmd->exi_channel;
	unsigned long flags;
	int retval = 0;

	BUG_ON(exi_channel == NULL);

	spin_lock_irqsave(&exi_channel->lock, flags);

	/* ensure atomic operations are serialized */
	while (exi_channel->queued_cmd) {
		DBG("cmd %d while dma in flight on channel %d\n",
		    cmd->opcode, exi_channel->channel);
		spin_unlock_irqrestore(&exi_channel->lock, flags);
		exi_wait_for_transfer(exi_channel);
		spin_lock_irqsave(&exi_channel->lock, flags);
	}

	switch(cmd->opcode) {
	case EXI_OP_NOP:
		break;
	case EXI_OP_SELECT:
		while (exi_is_selected(exi_channel) && !in_atomic()) {
			spin_unlock_irqrestore(&exi_channel->lock, flags);
			DBG("select sleeping...\n");
			wait_event(exi_channel->wait_queue,
				   !exi_is_selected(exi_channel));
			spin_lock_irqsave(&exi_channel->lock, flags);
		}
		retval = exi_cmd_select(cmd);
		break;
	case EXI_OP_DESELECT:
		exi_cmd_deselect(cmd);
		wake_up(&exi_channel->wait_queue);
		break;
	case EXI_OP_READ:
	case EXI_OP_WRITE:
		retval = exi_cmd_transfer(cmd);
		break;
	default:
		retval = -ENOSYS;
		break;
	}

	spin_unlock_irqrestore(&exi_channel->lock, flags);

	/*
	 * We check for delayed work every time the channel becomes idle.
	 */
	if (cmd->opcode == EXI_OP_DESELECT)
		exi_check_pending_work();

	/* command completed */
	if (retval == 0)
		exi_command_done(cmd);

	return retval;
}


/*
 * Internal. Completion routine.
 */
static void exi_wait_done(struct exi_command *cmd)
{
	complete(cmd->done_data);
}

/*
 * Internal. Run a command and wait.
 * Might sleep if called from user context. Otherwise will busy-wait.
 */
static int exi_run_command_and_wait(struct exi_command *cmd)
{
	DECLARE_COMPLETION(complete);
	int retval;

	cmd->done_data = &complete;
	cmd->done = exi_wait_done;
	retval = exi_run_command(cmd);
	if (retval > 0) {
		if (in_atomic() || irqs_disabled()) {
			exi_wait_for_transfer(cmd->exi_channel);
		} else {
			wait_for_completion(&complete);
		}
		retval = 0;
	}
	return retval;
}


/**
 *	exi_select  -  selects a exi device
 *	@exi_device:	exi device being selected
 *
 *	Selects a given EXI device.
 */
int exi_select(struct exi_device *exi_device)
{
	struct exi_command cmd;

	exi_op_select(&cmd, exi_device);
	return exi_run_command(&cmd);
}

/**
 *	exi_deselect  -  deselects all devices on an exi channel
 *	@exi_channel:	channel
 *
 *	Deselects all EXI devices on the given channel.
 *
 */
void exi_deselect(struct exi_channel *exi_channel)
{
	struct exi_command cmd;

	exi_op_deselect(&cmd, exi_channel);
	exi_run_command(&cmd);
}

/**
 *	exi_transfer  -  Performs a read or write EXI transfer.
 *	@exi_channel:	channel
 *	@data:		pointer to data being read/written
 *	@len:		length of data
 *	@opcode:	operation code (EXI_OP_READ or EXI_OP_WRITE)
 *
 *	Read or write data on a given EXI channel.
 */
void exi_transfer(struct exi_channel *exi_channel, void *data, size_t len,
		   int opcode, unsigned long flags)
{
	struct exi_command cmd;

	exi_op_transfer(&cmd, exi_channel, data, len, opcode);
	cmd.flags |= flags;
	exi_run_command_and_wait(&cmd);
}

/*
 * Internal. Count number of busy exi channels given a channel mask.
 * Caller holds the channel lock.
 */
static inline int exi_count_busy_channels(unsigned int channel_mask)
{
	struct exi_channel *exi_channel;
	unsigned int channel = 0;
	int count = 0;

	while(channel_mask && channel < EXI_MAX_CHANNELS) {
		if ((channel_mask & (1<<channel))) {
			channel_mask &= ~(1<<channel);
			exi_channel = __to_exi_channel(channel);
			if (exi_is_selected(exi_channel))
				count++;
		}
		channel++;
	}

	return count;
}

/*
 * Internal. Determine if we can trigger an exi event.
 * Caller holds the channel lock.
 */
static inline int exi_can_trigger_event(struct exi_channel *exi_channel,
					unsigned int event_id)
{
	struct exi_event_handler *event;

	event = &exi_channel->events[event_id];
	return !exi_count_busy_channels(event->channel_mask);
}

/*
 * Internal. Trigger an exi event.
 */
static inline int exi_trigger_event(struct exi_channel *exi_channel,
				    unsigned int event_id)
{
	struct exi_event_handler *event;
	exi_event_handler_t handler;
	int retval = 0;

	event = &exi_channel->events[event_id];
	handler = event->handler;
	if (handler) {
		retval = handler(exi_channel, event_id, event->data);
	}
	return retval;
}

/*
 * Internal. Conditionally trigger an exi event.
 */
static void exi_cond_trigger_event(struct exi_channel *exi_channel,
				   unsigned int event_id, int csr_mask)
{
	unsigned long flags;

	spin_lock_irqsave(&exi_channel->lock, flags);
	if (exi_can_trigger_event(exi_channel, event_id)) {
		if ((exi_channel->csr & csr_mask)) {
			exi_channel->csr &= ~csr_mask;
			spin_unlock_irqrestore(&exi_channel->lock, flags);
			exi_trigger_event(exi_channel, event_id);
			goto out;
		}
	}
	spin_unlock_irqrestore(&exi_channel->lock, flags);

out:
	return;
}

/*
 * Internal. Tasklet used to execute delayed work.
 */
static void exi_tasklet(unsigned long param)
{
	struct exi_channel *exi_channel = (struct exi_channel *)param;

	DBG("channel=%d, csr=%08lx\n", exi_channel->channel, exi_channel->csr);

	if (exi_channel->queued_cmd) {
		DBG("tasklet while xfer in flight on channel %d, csr = %08lx\n",
		    exi_channel->channel, exi_channel->csr);
	}

	/*
	 * We won't lauch event handlers if any of the channels we 
	 * provided on event registration is in use.
	 */

	exi_cond_trigger_event(exi_channel, EXI_EVENT_TC, EXI_CSR_TCINT);
	exi_cond_trigger_event(exi_channel, EXI_EVENT_IRQ, EXI_CSR_EXIINT);
	exi_cond_trigger_event(exi_channel, EXI_EVENT_INSERT, EXI_CSR_EXTIN);
}

/*
 * Internal. Interrupt handler for EXI interrupts.
 */
static irqreturn_t exi_irq_handler(int irq, void *dev_id, struct pt_regs *regs)
{
	struct exi_channel *exi_channel;
	u32 __iomem *csr_reg;
	u32 csr, status, mask;
	unsigned long flags;

	exi_channel_for_each(exi_channel) {
		csr_reg = exi_channel->io_base + EXI_CSR;

		/*
		 * Determine if we have pending interrupts on this channel,
		 * and which ones.
		 */
		spin_lock_irqsave(&exi_channel->io_lock, flags);

		csr = readl(csr_reg);
		mask = csr & (EXI_CSR_EXTINMASK |
			      EXI_CSR_TCINTMASK | EXI_CSR_EXIINTMASK);
		status = csr & (mask << 1);
		if (!status) {
			spin_unlock_irqrestore(&exi_channel->io_lock, flags);
			continue;
		}

		/* XXX do not signal TC events for now... */
		exi_channel->csr |= (status & ~EXI_CSR_TCINT);

		DBG("channel=%d, csr=%08lx\n", exi_channel->channel,
		    exi_channel->csr);

		/* ack all for this channel */
                writel(csr | status, csr_reg);

		spin_unlock_irqrestore(&exi_channel->io_lock, flags);

		if ((status & EXI_CSR_TCINT)) {
			exi_wait_for_transfer_one(exi_channel);
		}
		if ((status & EXI_CSR_EXTIN)) {
			wake_up(&exi_bus_waitq);
		}

		if (exi_channel->csr && !exi_is_selected(exi_channel)) {
			tasklet_schedule(&exi_channel->tasklet);
		}
	}
	return IRQ_HANDLED;
}

/*
 * Internal. Enable an exi event.
 */
static int exi_enable_event(struct exi_channel *exi_channel,
			    unsigned int event_id)
{
	u32 __iomem *csr_reg = exi_channel->io_base + EXI_CSR;
	u32 csr;
	unsigned long flags;

	spin_lock_irqsave(&exi_channel->io_lock, flags);
	csr = readl(csr_reg);

	/* ack and enable the associated interrupt */
	switch (event_id) {
	case EXI_EVENT_INSERT:
		writel(csr | (EXI_CSR_EXTIN | EXI_CSR_EXTINMASK), csr_reg);
		break;
	case EXI_EVENT_TC:
		//writel(csr | (EXI_CSR_TCINT | EXI_CSR_TCINTMASK), csr_reg);
		break;
	case EXI_EVENT_IRQ:
		writel(csr | (EXI_CSR_EXIINT | EXI_CSR_EXIINTMASK), csr_reg);
		break;
	}
	spin_unlock_irqrestore(&exi_channel->io_lock, flags);
	return 0;
}

/*
 * Internal. Disable an exi event.
 */
static int exi_disable_event(struct exi_channel *exi_channel,
			     unsigned int event_id)
{
	u32 __iomem *csr_reg = exi_channel->io_base + EXI_CSR;
	u32 csr;
	unsigned long flags;

	spin_lock_irqsave(&exi_channel->io_lock, flags);
	csr = readl(csr_reg);

	/* ack and disable the associated interrupt */
	switch (event_id) {
	case EXI_EVENT_INSERT:
		writel((csr | EXI_CSR_EXTIN) & ~EXI_CSR_EXTINMASK, csr_reg);
		break;
	case EXI_EVENT_TC:
		//writel((csr | EXI_CSR_TCINT) & ~EXI_CSR_TCINTMASK, csr_reg);
		break;
	case EXI_EVENT_IRQ:
		writel((csr | EXI_CSR_EXIINT) & ~EXI_CSR_EXIINTMASK, csr_reg);
		break;
	}
	spin_unlock_irqrestore(&exi_channel->io_lock, flags);
	return 0;
}


/**
 *	exi_event_register  -  Registers an event on a given channel.
 *	@exi_channel:	channel
 *	@event_id:	event id
 *	@handler:	event handler
 *	@data:		data passed to event handler
 *
 *	Register a handler to be called whenever a specified event happens
 *	on the given channel.
 */
int exi_event_register(struct exi_channel *exi_channel, unsigned int event_id,
		       exi_event_handler_t handler, void *data,
		       unsigned int channel_mask)
{
	struct exi_event_handler *event;
	int retval = 0;

	BUG_ON(event_id > EXI_MAX_EVENTS);

	event = &exi_channel->events[event_id];

	spin_lock(&exi_channel->lock);
	if (event->handler) {
		retval = -EBUSY;
		goto out;
	}
	event->handler = handler;
	event->data = data;
	event->channel_mask = channel_mask;
	exi_enable_event(exi_channel, event_id);

out:
	spin_unlock(&exi_channel->lock);
	return retval;
}


/**
 *	exi_event_unregister  -  Unregisters an event on a given channel.
 *	@exi_channel:	channel
 *	@event_id:	event id
 *
 *	Unregister a previously registered event handler.
 */
int exi_event_unregister(struct exi_channel *exi_channel, unsigned int event_id)
{
	struct exi_event_handler *event;
	int retval = 0;

	BUG_ON(event_id > EXI_MAX_EVENTS);

	event = &exi_channel->events[event_id];

	spin_lock(&exi_channel->lock);
	exi_disable_event(exi_channel, event_id);
	event->handler = NULL;
	event->data = NULL;
	event->channel_mask = 0;
	spin_unlock(&exi_channel->lock);

	return retval;
}

/*
 * Internal. Quiesce a channel.
 */
static void exi_quiesce_channel(struct exi_channel *exi_channel, u32 csr_mask)
{
	/* wait for dma transfers to complete */
	exi_wait_for_transfer_raw(exi_channel);

	/* ack and mask all interrupts */
	writel(EXI_CSR_TCINT  | EXI_CSR_EXIINT | EXI_CSR_EXTIN | csr_mask,
	       exi_channel->io_base + EXI_CSR);
}

/*
 * Internal. Quiesce all channels.
 */
static void exi_quiesce_all_channels(u32 csr_mask)
{
	struct exi_channel *exi_channel;

	exi_channel_for_each(exi_channel) {
		exi_quiesce_channel(exi_channel, csr_mask);
	}
}

/**
 *	exi_get_id  -  Returns the EXI ID of a device
 *	@exi_channel:	channel
 *	@device:	device number on channel
 *	@freq:		clock frequency index
 *
 *	Returns the EXI ID of an EXI device on a given channel.
 *	Might sleep.
 */
u32 exi_get_id(struct exi_device *exi_device)
{
	struct exi_channel *exi_channel = exi_device->exi_channel;
	u32 __iomem *csr_reg = exi_channel->io_base + EXI_CSR;
	u32 id = EXI_ID_INVALID;
	u16 cmd = 0;

	/* ask for the EXI id */
	exi_dev_select(exi_device);
	exi_dev_write(exi_device, &cmd, sizeof(cmd));
	exi_dev_read(exi_device, &id, sizeof(id));
	exi_dev_deselect(exi_device);

	/*
	 * We return a EXI_ID_NONE if there is some unidentified device
	 * inserted in memcard slot A or memcard slot B.
	 * This, for example, allows the SD/MMC driver to see cards.
	 */
	if (id == EXI_ID_INVALID) {
		if ((__to_channel(exi_channel) == 0 ||
		     __to_channel(exi_channel) == 1)
		    && exi_device->eid.device == 0) {
			if (readl(csr_reg) & EXI_CSR_EXT) {
				id = EXI_ID_NONE;
			}
		} 
	}

	return id;
}

/*
 * Tells if there is a device inserted in one of the memory card slots.
 */
int exi_get_ext_line(struct exi_channel *exi_channel)
{
	u32 __iomem *csr_reg = exi_channel->io_base + EXI_CSR;
	return (readl(csr_reg) & EXI_CSR_EXT)?1:0;
}

/*
 * Saves the current insertion status of a given channel.
 */
void exi_update_ext_status(struct exi_channel *exi_channel)
{
	if (exi_get_ext_line(exi_channel)) {
		exi_channel->flags |= EXI_EXT;
	} else {
		exi_channel->flags &= ~EXI_EXT;
	}
}

/*
 * Pseudo-Internal. Initialize basic channel structures and hardware.
 */
int exi_hw_init(char *module_name)
{
	struct exi_channel *exi_channel;
	int channel;
	int retval;

	for(channel = 0; channel < EXI_MAX_CHANNELS; channel++) {
		exi_channel = __to_exi_channel(channel);

		/* initialize a channel structure */
		exi_channel_init(exi_channel, channel);
	}

	/* calm down the hardware and allow extractions */
	exi_quiesce_all_channels(EXI_CSR_EXTINMASK);

	/* register the exi interrupt handler */
        retval = request_irq(EXI_IRQ, exi_irq_handler, 0, module_name, NULL);
        if (retval) {
		exi_printk(KERN_ERR, "unable to register irq%d\n", EXI_IRQ);
        }

	return retval;
}

/*
 * Pseudo-Internal. 
 */
void exi_hw_exit(void)
{
	exi_quiesce_all_channels(0);
	free_irq(EXI_IRQ, NULL);
}


EXPORT_SYMBOL(to_exi_channel);
EXPORT_SYMBOL(to_channel);

EXPORT_SYMBOL(exi_select_raw);
EXPORT_SYMBOL(exi_deselect_raw);
EXPORT_SYMBOL(exi_transfer_raw);

EXPORT_SYMBOL(exi_select);
EXPORT_SYMBOL(exi_deselect);
EXPORT_SYMBOL(exi_transfer);

EXPORT_SYMBOL(exi_get_id);
EXPORT_SYMBOL(exi_event_register);
EXPORT_SYMBOL(exi_event_unregister);

