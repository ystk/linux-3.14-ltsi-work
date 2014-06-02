/*
 * SH RSPI driver
 *
 * Copyright (C) 2012, 2013  Renesas Solutions Corp.
 * Copyright (C) 2014 Glider bvba
 *
 * Based on spi-sh.c:
 * Copyright (C) 2011 Renesas Solutions Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/of_device.h>
#include <linux/pm_runtime.h>
#include <linux/sh_dma.h>
#include <linux/spi/spi.h>
#include <linux/spi/rspi.h>

#define RSPI_SPCR		0x00	/* Control Register */
#define RSPI_SSLP		0x01	/* Slave Select Polarity Register */
#define RSPI_SPPCR		0x02	/* Pin Control Register */
#define RSPI_SPSR		0x03	/* Status Register */
#define RSPI_SPDR		0x04	/* Data Register */
#define RSPI_SPSCR		0x08	/* Sequence Control Register */
#define RSPI_SPSSR		0x09	/* Sequence Status Register */
#define RSPI_SPBR		0x0a	/* Bit Rate Register */
#define RSPI_SPDCR		0x0b	/* Data Control Register */
#define RSPI_SPCKD		0x0c	/* Clock Delay Register */
#define RSPI_SSLND		0x0d	/* Slave Select Negation Delay Register */
#define RSPI_SPND		0x0e	/* Next-Access Delay Register */
#define RSPI_SPCR2		0x0f	/* Control Register 2 (SH only) */
#define RSPI_SPCMD0		0x10	/* Command Register 0 */
#define RSPI_SPCMD1		0x12	/* Command Register 1 */
#define RSPI_SPCMD2		0x14	/* Command Register 2 */
#define RSPI_SPCMD3		0x16	/* Command Register 3 */
#define RSPI_SPCMD4		0x18	/* Command Register 4 */
#define RSPI_SPCMD5		0x1a	/* Command Register 5 */
#define RSPI_SPCMD6		0x1c	/* Command Register 6 */
#define RSPI_SPCMD7		0x1e	/* Command Register 7 */
#define RSPI_SPCMD(i)		(RSPI_SPCMD0 + (i) * 2)
#define RSPI_NUM_SPCMD		8
#define RSPI_RZ_NUM_SPCMD	4
#define QSPI_NUM_SPCMD		4

/* RSPI on RZ only */
#define RSPI_SPBFCR		0x20	/* Buffer Control Register */
#define RSPI_SPBFDR		0x22	/* Buffer Data Count Setting Register */

/* QSPI only */
#define QSPI_SPBFCR		0x18	/* Buffer Control Register */
#define QSPI_SPBDCR		0x1a	/* Buffer Data Count Register */
#define QSPI_SPBMUL0		0x1c	/* Transfer Data Length Multiplier Setting Register 0 */
#define QSPI_SPBMUL1		0x20	/* Transfer Data Length Multiplier Setting Register 1 */
#define QSPI_SPBMUL2		0x24	/* Transfer Data Length Multiplier Setting Register 2 */
#define QSPI_SPBMUL3		0x28	/* Transfer Data Length Multiplier Setting Register 3 */
#define QSPI_SPBMUL(i)		(QSPI_SPBMUL0 + (i) * 4)

/* SPCR - Control Register */
#define SPCR_SPRIE		0x80	/* Receive Interrupt Enable */
#define SPCR_SPE		0x40	/* Function Enable */
#define SPCR_SPTIE		0x20	/* Transmit Interrupt Enable */
#define SPCR_SPEIE		0x10	/* Error Interrupt Enable */
#define SPCR_MSTR		0x08	/* Master/Slave Mode Select */
#define SPCR_MODFEN		0x04	/* Mode Fault Error Detection Enable */
/* RSPI on SH only */
#define SPCR_TXMD		0x02	/* TX Only Mode (vs. Full Duplex) */
#define SPCR_SPMS		0x01	/* 3-wire Mode (vs. 4-wire) */
/* QSPI on R-Car M2 only */
#define SPCR_WSWAP		0x02	/* Word Swap of read-data for DMAC */
#define SPCR_BSWAP		0x01	/* Byte Swap of read-data for DMAC */

/* SSLP - Slave Select Polarity Register */
#define SSLP_SSL1P		0x02	/* SSL1 Signal Polarity Setting */
#define SSLP_SSL0P		0x01	/* SSL0 Signal Polarity Setting */

/* SPPCR - Pin Control Register */
#define SPPCR_MOIFE		0x20	/* MOSI Idle Value Fixing Enable */
#define SPPCR_MOIFV		0x10	/* MOSI Idle Fixed Value */
#define SPPCR_SPOM		0x04
#define SPPCR_SPLP2		0x02	/* Loopback Mode 2 (non-inverting) */
#define SPPCR_SPLP		0x01	/* Loopback Mode (inverting) */

#define SPPCR_IO3FV		0x04	/* Single-/Dual-SPI Mode IO3 Output Fixed Value */
#define SPPCR_IO2FV		0x04	/* Single-/Dual-SPI Mode IO2 Output Fixed Value */

/* SPSR - Status Register */
#define SPSR_SPRF		0x80	/* Receive Buffer Full Flag */
#define SPSR_TEND		0x40	/* Transmit End */
#define SPSR_SPTEF		0x20	/* Transmit Buffer Empty Flag */
#define SPSR_PERF		0x08	/* Parity Error Flag */
#define SPSR_MODF		0x04	/* Mode Fault Error Flag */
#define SPSR_IDLNF		0x02	/* RSPI Idle Flag */
#define SPSR_OVRF		0x01	/* Overrun Error Flag (RSPI only) */

/* SPSCR - Sequence Control Register */
#define SPSCR_SPSLN_MASK	0x07	/* Sequence Length Specification */

/* SPSSR - Sequence Status Register */
#define SPSSR_SPECM_MASK	0x70	/* Command Error Mask */
#define SPSSR_SPCP_MASK		0x07	/* Command Pointer Mask */

/* SPDCR - Data Control Register */
#define SPDCR_TXDMY		0x80	/* Dummy Data Transmission Enable */
#define SPDCR_SPLW1		0x40	/* Access Width Specification (RZ) */
#define SPDCR_SPLW0		0x20	/* Access Width Specification (RZ) */
#define SPDCR_SPLLWORD		(SPDCR_SPLW1 | SPDCR_SPLW0)
#define SPDCR_SPLWORD		SPDCR_SPLW1
#define SPDCR_SPLBYTE		SPDCR_SPLW0
#define SPDCR_SPLW		0x20	/* Access Width Specification (SH) */
#define SPDCR_SPRDTD		0x10	/* Receive Transmit Data Select (SH) */
#define SPDCR_SLSEL1		0x08
#define SPDCR_SLSEL0		0x04
#define SPDCR_SLSEL_MASK	0x0c	/* SSL1 Output Select (SH) */
#define SPDCR_SPFC1		0x02
#define SPDCR_SPFC0		0x01
#define SPDCR_SPFC_MASK		0x03	/* Frame Count Setting (1-4) (SH) */

/* SPCKD - Clock Delay Register */
#define SPCKD_SCKDL_MASK	0x07	/* Clock Delay Setting (1-8) */

/* SSLND - Slave Select Negation Delay Register */
#define SSLND_SLNDL_MASK	0x07	/* SSL Negation Delay Setting (1-8) */

/* SPND - Next-Access Delay Register */
#define SPND_SPNDL_MASK		0x07	/* Next-Access Delay Setting (1-8) */

/* SPCR2 - Control Register 2 */
#define SPCR2_PTE		0x08	/* Parity Self-Test Enable */
#define SPCR2_SPIE		0x04	/* Idle Interrupt Enable */
#define SPCR2_SPOE		0x02	/* Odd Parity Enable (vs. Even) */
#define SPCR2_SPPE		0x01	/* Parity Enable */

/* SPCMDn - Command Registers */
#define SPCMD_SCKDEN		0x8000	/* Clock Delay Setting Enable */
#define SPCMD_SLNDEN		0x4000	/* SSL Negation Delay Setting Enable */
#define SPCMD_SPNDEN		0x2000	/* Next-Access Delay Enable */
#define SPCMD_LSBF		0x1000	/* LSB First */
#define SPCMD_SPB_MASK		0x0f00	/* Data Length Setting */
#define SPCMD_SPB_8_TO_16(bit)	(((bit - 1) << 8) & SPCMD_SPB_MASK)
#define SPCMD_SPB_8BIT		0x0000	/* QSPI only */
#define SPCMD_SPB_16BIT		0x0100
#define SPCMD_SPB_20BIT		0x0000
#define SPCMD_SPB_24BIT		0x0100
#define SPCMD_SPB_32BIT		0x0200
#define SPCMD_SSLKP		0x0080	/* SSL Signal Level Keeping */
#define SPCMD_SPIMOD_MASK	0x0060	/* SPI Operating Mode (QSPI only) */
#define SPCMD_SPIMOD1		0x0040
#define SPCMD_SPIMOD0		0x0020
#define SPCMD_SPIMOD_SINGLE	0
#define SPCMD_SPIMOD_DUAL	SPCMD_SPIMOD0
#define SPCMD_SPIMOD_QUAD	SPCMD_SPIMOD1
#define SPCMD_SPRW		0x0010	/* SPI Read/Write Access (Dual/Quad) */
#define SPCMD_SSLA_MASK		0x0030	/* SSL Assert Signal Setting (RSPI) */
#define SPCMD_BRDV_MASK		0x000c	/* Bit Rate Division Setting */
#define SPCMD_CPOL		0x0002	/* Clock Polarity Setting */
#define SPCMD_CPHA		0x0001	/* Clock Phase Setting */

/* SPBFCR - Buffer Control Register */
#define SPBFCR_TXRST		0x80	/* Transmit Buffer Data Reset */
#define SPBFCR_RXRST		0x40	/* Receive Buffer Data Reset */
#define SPBFCR_TXTRG_MASK	0x30	/* Transmit Buffer Data Triggering Number */
#define SPBFCR_RXTRG_MASK	0x07	/* Receive Buffer Data Triggering Number */

struct rspi_data {
	void __iomem *addr;
	u32 max_speed_hz;
	struct spi_master *master;
	wait_queue_head_t wait;
	struct clk *clk;
	u16 spcmd;
	u8 spsr;
	u8 sppcr;
	int rx_irq, tx_irq;
	const struct spi_ops *ops;

	/* for dmaengine */
	struct dma_chan *chan_tx;
	struct dma_chan *chan_rx;

	unsigned dma_callbacked:1;
	unsigned byte_access:1;
};

static void rspi_write8(const struct rspi_data *rspi, u8 data, u16 offset)
{
	iowrite8(data, rspi->addr + offset);
}

static void rspi_write16(const struct rspi_data *rspi, u16 data, u16 offset)
{
	iowrite16(data, rspi->addr + offset);
}

static void rspi_write32(const struct rspi_data *rspi, u32 data, u16 offset)
{
	iowrite32(data, rspi->addr + offset);
}

static u8 rspi_read8(const struct rspi_data *rspi, u16 offset)
{
	return ioread8(rspi->addr + offset);
}

static u16 rspi_read16(const struct rspi_data *rspi, u16 offset)
{
	return ioread16(rspi->addr + offset);
}

static void rspi_write_data(const struct rspi_data *rspi, u16 data)
{
	if (rspi->byte_access)
		rspi_write8(rspi, data, RSPI_SPDR);
	else /* 16 bit */
		rspi_write16(rspi, data, RSPI_SPDR);
}

static u16 rspi_read_data(const struct rspi_data *rspi)
{
	if (rspi->byte_access)
		return rspi_read8(rspi, RSPI_SPDR);
	else /* 16 bit */
		return rspi_read16(rspi, RSPI_SPDR);
}

/* optional functions */
struct spi_ops {
	int (*set_config_register)(struct rspi_data *rspi, int access_size);
	int (*transfer_one)(struct spi_master *master, struct spi_device *spi,
			    struct spi_transfer *xfer);
	u16 mode_bits;
	u16 flags;
};

/*
 * functions for RSPI on legacy SH
 */
static int rspi_set_config_register(struct rspi_data *rspi, int access_size)
{
	int spbr;

	/* Sets output mode, MOSI signal, and (optionally) loopback */
	rspi_write8(rspi, rspi->sppcr, RSPI_SPPCR);

	/* Sets transfer bit rate */
	spbr = DIV_ROUND_UP(clk_get_rate(rspi->clk),
			    2 * rspi->max_speed_hz) - 1;
	rspi_write8(rspi, clamp(spbr, 0, 255), RSPI_SPBR);

	/* Disable dummy transmission, set 16-bit word access, 1 frame */
	rspi_write8(rspi, 0, RSPI_SPDCR);
	rspi->byte_access = 0;

	/* Sets RSPCK, SSL, next-access delay value */
	rspi_write8(rspi, 0x00, RSPI_SPCKD);
	rspi_write8(rspi, 0x00, RSPI_SSLND);
	rspi_write8(rspi, 0x00, RSPI_SPND);

	/* Sets parity, interrupt mask */
	rspi_write8(rspi, 0x00, RSPI_SPCR2);

	/* Sets SPCMD */
	rspi->spcmd |= SPCMD_SPB_8_TO_16(access_size);
	rspi_write16(rspi, rspi->spcmd, RSPI_SPCMD0);

	/* Sets RSPI mode */
	rspi_write8(rspi, SPCR_MSTR, RSPI_SPCR);

	return 0;
}

/*
 * functions for RSPI on RZ
 */
static int rspi_rz_set_config_register(struct rspi_data *rspi, int access_size)
{
	int spbr;

	/* Sets output mode, MOSI signal, and (optionally) loopback */
	rspi_write8(rspi, rspi->sppcr, RSPI_SPPCR);

	/* Sets transfer bit rate */
	spbr = DIV_ROUND_UP(clk_get_rate(rspi->clk),
			    2 * rspi->max_speed_hz) - 1;
	rspi_write8(rspi, clamp(spbr, 0, 255), RSPI_SPBR);

	/* Disable dummy transmission, set byte access */
	rspi_write8(rspi, SPDCR_SPLBYTE, RSPI_SPDCR);
	rspi->byte_access = 1;

	/* Sets RSPCK, SSL, next-access delay value */
	rspi_write8(rspi, 0x00, RSPI_SPCKD);
	rspi_write8(rspi, 0x00, RSPI_SSLND);
	rspi_write8(rspi, 0x00, RSPI_SPND);

	/* Sets SPCMD */
	rspi->spcmd |= SPCMD_SPB_8_TO_16(access_size);
	rspi_write16(rspi, rspi->spcmd, RSPI_SPCMD0);

	/* Sets RSPI mode */
	rspi_write8(rspi, SPCR_MSTR, RSPI_SPCR);

	return 0;
}

/*
 * functions for QSPI
 */
static int qspi_set_config_register(struct rspi_data *rspi, int access_size)
{
	int spbr;

	/* Sets output mode, MOSI signal, and (optionally) loopback */
	rspi_write8(rspi, rspi->sppcr, RSPI_SPPCR);

	/* Sets transfer bit rate */
	spbr = DIV_ROUND_UP(clk_get_rate(rspi->clk), 2 * rspi->max_speed_hz);
	rspi_write8(rspi, clamp(spbr, 0, 255), RSPI_SPBR);

	/* Disable dummy transmission, set byte access */
	rspi_write8(rspi, 0, RSPI_SPDCR);
	rspi->byte_access = 1;

	/* Sets RSPCK, SSL, next-access delay value */
	rspi_write8(rspi, 0x00, RSPI_SPCKD);
	rspi_write8(rspi, 0x00, RSPI_SSLND);
	rspi_write8(rspi, 0x00, RSPI_SPND);

	/* Data Length Setting */
	if (access_size == 8)
		rspi->spcmd |= SPCMD_SPB_8BIT;
	else if (access_size == 16)
		rspi->spcmd |= SPCMD_SPB_16BIT;
	else
		rspi->spcmd |= SPCMD_SPB_32BIT;

	rspi->spcmd |= SPCMD_SCKDEN | SPCMD_SLNDEN | SPCMD_SPNDEN;

	/* Resets transfer data length */
	rspi_write32(rspi, 0, QSPI_SPBMUL0);

	/* Resets transmit and receive buffer */
	rspi_write8(rspi, SPBFCR_TXRST | SPBFCR_RXRST, QSPI_SPBFCR);
	/* Sets buffer to allow normal operation */
	rspi_write8(rspi, 0x00, QSPI_SPBFCR);

	/* Sets SPCMD */
	rspi_write16(rspi, rspi->spcmd, RSPI_SPCMD0);

	/* Enables SPI function in master mode */
	rspi_write8(rspi, SPCR_SPE | SPCR_MSTR, RSPI_SPCR);

	return 0;
}

#define set_config_register(spi, n) spi->ops->set_config_register(spi, n)

static void rspi_enable_irq(const struct rspi_data *rspi, u8 enable)
{
	rspi_write8(rspi, rspi_read8(rspi, RSPI_SPCR) | enable, RSPI_SPCR);
}

static void rspi_disable_irq(const struct rspi_data *rspi, u8 disable)
{
	rspi_write8(rspi, rspi_read8(rspi, RSPI_SPCR) & ~disable, RSPI_SPCR);
}

static int rspi_wait_for_interrupt(struct rspi_data *rspi, u8 wait_mask,
				   u8 enable_bit)
{
	int ret;

	rspi->spsr = rspi_read8(rspi, RSPI_SPSR);
	if (rspi->spsr & wait_mask)
		return 0;

	rspi_enable_irq(rspi, enable_bit);
	ret = wait_event_timeout(rspi->wait, rspi->spsr & wait_mask, HZ);
	if (ret == 0 && !(rspi->spsr & wait_mask))
		return -ETIMEDOUT;

	return 0;
}

static inline int rspi_wait_for_tx_empty(struct rspi_data *rspi)
{
	return rspi_wait_for_interrupt(rspi, SPSR_SPTEF, SPCR_SPTIE);
}

static inline int rspi_wait_for_rx_full(struct rspi_data *rspi)
{
	return rspi_wait_for_interrupt(rspi, SPSR_SPRF, SPCR_SPRIE);
}

static int rspi_data_out(struct rspi_data *rspi, u8 data)
{
	int error = rspi_wait_for_tx_empty(rspi);
	if (error < 0) {
		dev_err(&rspi->master->dev, "transmit timeout\n");
		return error;
	}
	rspi_write_data(rspi, data);
	return 0;
}

static int rspi_data_in(struct rspi_data *rspi)
{
	int error;
	u8 data;

	error = rspi_wait_for_rx_full(rspi);
	if (error < 0) {
		dev_err(&rspi->master->dev, "receive timeout\n");
		return error;
	}
	data = rspi_read_data(rspi);
	return data;
}

static int rspi_pio_transfer(struct rspi_data *rspi, const u8 *tx, u8 *rx,
			     unsigned int n)
{
	while (n-- > 0) {
		if (tx) {
			int ret = rspi_data_out(rspi, *tx++);
			if (ret < 0)
				return ret;
		}
		if (rx) {
			int ret = rspi_data_in(rspi);
			if (ret < 0)
				return ret;
			*rx++ = ret;
		}
	}

	return 0;
}

static void rspi_dma_complete(void *arg)
{
	struct rspi_data *rspi = arg;

	rspi->dma_callbacked = 1;
	wake_up_interruptible(&rspi->wait);
}

static int rspi_dma_map_sg(struct scatterlist *sg, const void *buf,
			   unsigned len, struct dma_chan *chan,
			   enum dma_transfer_direction dir)
{
	sg_init_table(sg, 1);
	sg_set_buf(sg, buf, len);
	sg_dma_len(sg) = len;
	return dma_map_sg(chan->device->dev, sg, 1, dir);
}

static void rspi_dma_unmap_sg(struct scatterlist *sg, struct dma_chan *chan,
			      enum dma_transfer_direction dir)
{
	dma_unmap_sg(chan->device->dev, sg, 1, dir);
}

static int rspi_send_dma(struct rspi_data *rspi, struct spi_transfer *t)
{
	struct scatterlist sg;
	const void *buf = t->tx_buf;
	struct dma_async_tx_descriptor *desc;
	unsigned int len = t->len;
	int ret = 0;

	if (!rspi_dma_map_sg(&sg, buf, len, rspi->chan_tx, DMA_TO_DEVICE))
		return -EFAULT;

	desc = dmaengine_prep_slave_sg(rspi->chan_tx, &sg, 1, DMA_TO_DEVICE,
				       DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!desc) {
		ret = -EIO;
		goto end;
	}

	/*
	 * DMAC needs SPTIE, but if SPTIE is set, this IRQ routine will be
	 * called. So, this driver disables the IRQ while DMA transfer.
	 */
	disable_irq(rspi->tx_irq);

	rspi_write8(rspi, rspi_read8(rspi, RSPI_SPCR) | SPCR_TXMD, RSPI_SPCR);
	rspi_enable_irq(rspi, SPCR_SPTIE);
	rspi->dma_callbacked = 0;

	desc->callback = rspi_dma_complete;
	desc->callback_param = rspi;
	dmaengine_submit(desc);
	dma_async_issue_pending(rspi->chan_tx);

	ret = wait_event_interruptible_timeout(rspi->wait,
					       rspi->dma_callbacked, HZ);
	if (ret > 0 && rspi->dma_callbacked)
		ret = 0;
	else if (!ret)
		ret = -ETIMEDOUT;
	rspi_disable_irq(rspi, SPCR_SPTIE);

	enable_irq(rspi->tx_irq);

end:
	rspi_dma_unmap_sg(&sg, rspi->chan_tx, DMA_TO_DEVICE);
	return ret;
}

static void rspi_receive_init(const struct rspi_data *rspi)
{
	u8 spsr;

	spsr = rspi_read8(rspi, RSPI_SPSR);
	if (spsr & SPSR_SPRF)
		rspi_read_data(rspi);	/* dummy read */
	if (spsr & SPSR_OVRF)
		rspi_write8(rspi, rspi_read8(rspi, RSPI_SPSR) & ~SPSR_OVRF,
			    RSPI_SPSR);
}

static void rspi_rz_receive_init(const struct rspi_data *rspi)
{
	rspi_receive_init(rspi);
	rspi_write8(rspi, SPBFCR_TXRST | SPBFCR_RXRST, RSPI_SPBFCR);
	rspi_write8(rspi, 0, RSPI_SPBFCR);
}

static void qspi_receive_init(const struct rspi_data *rspi)
{
	u8 spsr;

	spsr = rspi_read8(rspi, RSPI_SPSR);
	if (spsr & SPSR_SPRF)
		rspi_read_data(rspi);   /* dummy read */
	rspi_write8(rspi, SPBFCR_TXRST | SPBFCR_RXRST, QSPI_SPBFCR);
	rspi_write8(rspi, 0, QSPI_SPBFCR);
}

static int rspi_send_receive_dma(struct rspi_data *rspi, struct spi_transfer *t)
{
	struct scatterlist sg_rx, sg_tx;
	const void *tx_buf = t->tx_buf;
	void *rx_buf = t->rx_buf;
	struct dma_async_tx_descriptor *desc_tx, *desc_rx;
	unsigned int len = t->len;
	int ret = 0;

	/* prepare transmit transfer */
	if (!rspi_dma_map_sg(&sg_tx, tx_buf, len, rspi->chan_tx,
			     DMA_TO_DEVICE))
		return -EFAULT;

	desc_tx = dmaengine_prep_slave_sg(rspi->chan_tx, &sg_tx, 1,
			DMA_TO_DEVICE, DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!desc_tx) {
		ret = -EIO;
		goto end_tx_mapped;
	}

	/* prepare receive transfer */
	if (!rspi_dma_map_sg(&sg_rx, rx_buf, len, rspi->chan_rx,
			     DMA_FROM_DEVICE)) {
		ret = -EFAULT;
		goto end_tx_mapped;

	}
	desc_rx = dmaengine_prep_slave_sg(rspi->chan_rx, &sg_rx, 1,
					  DMA_FROM_DEVICE,
					  DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!desc_rx) {
		ret = -EIO;
		goto end;
	}

	rspi_receive_init(rspi);

	/*
	 * DMAC needs SPTIE, but if SPTIE is set, this IRQ routine will be
	 * called. So, this driver disables the IRQ while DMA transfer.
	 */
	disable_irq(rspi->tx_irq);
	if (rspi->rx_irq != rspi->tx_irq)
		disable_irq(rspi->rx_irq);

	rspi_write8(rspi, rspi_read8(rspi, RSPI_SPCR) & ~SPCR_TXMD, RSPI_SPCR);
	rspi_enable_irq(rspi, SPCR_SPTIE | SPCR_SPRIE);
	rspi->dma_callbacked = 0;

	desc_rx->callback = rspi_dma_complete;
	desc_rx->callback_param = rspi;
	dmaengine_submit(desc_rx);
	dma_async_issue_pending(rspi->chan_rx);

	desc_tx->callback = NULL;	/* No callback */
	dmaengine_submit(desc_tx);
	dma_async_issue_pending(rspi->chan_tx);

	ret = wait_event_interruptible_timeout(rspi->wait,
					       rspi->dma_callbacked, HZ);
	if (ret > 0 && rspi->dma_callbacked)
		ret = 0;
	else if (!ret)
		ret = -ETIMEDOUT;
	rspi_disable_irq(rspi, SPCR_SPTIE | SPCR_SPRIE);

	enable_irq(rspi->tx_irq);
	if (rspi->rx_irq != rspi->tx_irq)
		enable_irq(rspi->rx_irq);

end:
	rspi_dma_unmap_sg(&sg_rx, rspi->chan_rx, DMA_FROM_DEVICE);
end_tx_mapped:
	rspi_dma_unmap_sg(&sg_tx, rspi->chan_tx, DMA_TO_DEVICE);
	return ret;
}

static int rspi_is_dma(const struct rspi_data *rspi, struct spi_transfer *t)
{
	/* If the module receives data by DMAC, it also needs TX DMAC */
	if (t->rx_buf)
		return rspi->chan_tx && rspi->chan_rx;

	if (rspi->chan_tx)
		return 1;

	return 0;
}

static int rspi_transfer_out_in(struct rspi_data *rspi,
				struct spi_transfer *xfer)
{
	u8 spcr;
	int ret;

	spcr = rspi_read8(rspi, RSPI_SPCR);
	if (xfer->rx_buf) {
		rspi_receive_init(rspi);
		spcr &= ~SPCR_TXMD;
	} else {
		spcr |= SPCR_TXMD;
	}
	rspi_write8(rspi, spcr, RSPI_SPCR);

	ret = rspi_pio_transfer(rspi, xfer->tx_buf, xfer->rx_buf, xfer->len);
	if (ret < 0)
		return ret;

	/* Wait for the last transmission */
	rspi_wait_for_tx_empty(rspi);

	return 0;
}

static int rspi_transfer_one(struct spi_master *master, struct spi_device *spi,
			     struct spi_transfer *xfer)
{
	struct rspi_data *rspi = spi_master_get_devdata(master);

	if (!rspi_is_dma(rspi, xfer))
		return rspi_transfer_out_in(rspi, xfer);

	if (xfer->rx_buf)
		return rspi_send_receive_dma(rspi, xfer);
	else
		return rspi_send_dma(rspi, xfer);
}

static int rspi_rz_transfer_out_in(struct rspi_data *rspi,
				   struct spi_transfer *xfer)
{
	int ret;

	rspi_rz_receive_init(rspi);

	ret = rspi_pio_transfer(rspi, xfer->tx_buf, xfer->rx_buf, xfer->len);
	if (ret < 0)
		return ret;

	/* Wait for the last transmission */
	rspi_wait_for_tx_empty(rspi);

	return 0;
}

static int rspi_rz_transfer_one(struct spi_master *master,
				struct spi_device *spi,
				struct spi_transfer *xfer)
{
	struct rspi_data *rspi = spi_master_get_devdata(master);

	return rspi_rz_transfer_out_in(rspi, xfer);
}

static int qspi_transfer_out_in(struct rspi_data *rspi,
				struct spi_transfer *xfer)
{
	int ret;

	qspi_receive_init(rspi);

	ret = rspi_pio_transfer(rspi, xfer->tx_buf, xfer->rx_buf, xfer->len);
	if (ret < 0)
		return ret;

	/* Wait for the last transmission */
	rspi_wait_for_tx_empty(rspi);

	return 0;
}

static int qspi_transfer_out(struct rspi_data *rspi, struct spi_transfer *xfer)
{
	int ret;

	ret = rspi_pio_transfer(rspi, xfer->tx_buf, NULL, xfer->len);
	if (ret < 0)
		return ret;

	/* Wait for the last transmission */
	rspi_wait_for_tx_empty(rspi);

	return 0;
}

static int qspi_transfer_in(struct rspi_data *rspi, struct spi_transfer *xfer)
{
	return rspi_pio_transfer(rspi, NULL, xfer->rx_buf, xfer->len);
}

static int qspi_transfer_one(struct spi_master *master, struct spi_device *spi,
			     struct spi_transfer *xfer)
{
	struct rspi_data *rspi = spi_master_get_devdata(master);

	if (spi->mode & SPI_LOOP) {
		return qspi_transfer_out_in(rspi, xfer);
	} else if (xfer->tx_nbits > SPI_NBITS_SINGLE) {
		/* Quad or Dual SPI Write */
		return qspi_transfer_out(rspi, xfer);
	} else if (xfer->rx_nbits > SPI_NBITS_SINGLE) {
		/* Quad or Dual SPI Read */
		return qspi_transfer_in(rspi, xfer);
	} else {
		/* Single SPI Transfer */
		return qspi_transfer_out_in(rspi, xfer);
	}
}

static int rspi_setup(struct spi_device *spi)
{
	struct rspi_data *rspi = spi_master_get_devdata(spi->master);

	rspi->max_speed_hz = spi->max_speed_hz;

	rspi->spcmd = SPCMD_SSLKP;
	if (spi->mode & SPI_CPOL)
		rspi->spcmd |= SPCMD_CPOL;
	if (spi->mode & SPI_CPHA)
		rspi->spcmd |= SPCMD_CPHA;

	/* CMOS output mode and MOSI signal from previous transfer */
	rspi->sppcr = 0;
	if (spi->mode & SPI_LOOP)
		rspi->sppcr |= SPPCR_SPLP;

	set_config_register(rspi, 8);

	return 0;
}

static u16 qspi_transfer_mode(const struct spi_transfer *xfer)
{
	if (xfer->tx_buf)
		switch (xfer->tx_nbits) {
		case SPI_NBITS_QUAD:
			return SPCMD_SPIMOD_QUAD;
		case SPI_NBITS_DUAL:
			return SPCMD_SPIMOD_DUAL;
		default:
			return 0;
		}
	if (xfer->rx_buf)
		switch (xfer->rx_nbits) {
		case SPI_NBITS_QUAD:
			return SPCMD_SPIMOD_QUAD | SPCMD_SPRW;
		case SPI_NBITS_DUAL:
			return SPCMD_SPIMOD_DUAL | SPCMD_SPRW;
		default:
			return 0;
		}

	return 0;
}

static int qspi_setup_sequencer(struct rspi_data *rspi,
				const struct spi_message *msg)
{
	const struct spi_transfer *xfer;
	unsigned int i = 0, len = 0;
	u16 current_mode = 0xffff, mode;

	list_for_each_entry(xfer, &msg->transfers, transfer_list) {
		mode = qspi_transfer_mode(xfer);
		if (mode == current_mode) {
			len += xfer->len;
			continue;
		}

		/* Transfer mode change */
		if (i) {
			/* Set transfer data length of previous transfer */
			rspi_write32(rspi, len, QSPI_SPBMUL(i - 1));
		}

		if (i >= QSPI_NUM_SPCMD) {
			dev_err(&msg->spi->dev,
				"Too many different transfer modes");
			return -EINVAL;
		}

		/* Program transfer mode for this transfer */
		rspi_write16(rspi, rspi->spcmd | mode, RSPI_SPCMD(i));
		current_mode = mode;
		len = xfer->len;
		i++;
	}
	if (i) {
		/* Set final transfer data length and sequence length */
		rspi_write32(rspi, len, QSPI_SPBMUL(i - 1));
		rspi_write8(rspi, i - 1, RSPI_SPSCR);
	}

	return 0;
}

static int rspi_prepare_message(struct spi_master *master,
				struct spi_message *msg)
{
	struct rspi_data *rspi = spi_master_get_devdata(master);
	int ret;

	if (msg->spi->mode &
	    (SPI_TX_DUAL | SPI_TX_QUAD | SPI_RX_DUAL | SPI_RX_QUAD)) {
		/* Setup sequencer for messages with multiple transfer modes */
		ret = qspi_setup_sequencer(rspi, msg);
		if (ret < 0)
			return ret;
	}

	/* Enable SPI function in master mode */
	rspi_write8(rspi, rspi_read8(rspi, RSPI_SPCR) | SPCR_SPE, RSPI_SPCR);
	return 0;
}

static int rspi_unprepare_message(struct spi_master *master,
				  struct spi_message *msg)
{
	struct rspi_data *rspi = spi_master_get_devdata(master);

	/* Disable SPI function */
	rspi_write8(rspi, rspi_read8(rspi, RSPI_SPCR) & ~SPCR_SPE, RSPI_SPCR);

	/* Reset sequencer for Single SPI Transfers */
	rspi_write16(rspi, rspi->spcmd, RSPI_SPCMD0);
	rspi_write8(rspi, 0, RSPI_SPSCR);
	return 0;
}

static irqreturn_t rspi_irq_mux(int irq, void *_sr)
{
	struct rspi_data *rspi = _sr;
	u8 spsr;
	irqreturn_t ret = IRQ_NONE;
	u8 disable_irq = 0;

	rspi->spsr = spsr = rspi_read8(rspi, RSPI_SPSR);
	if (spsr & SPSR_SPRF)
		disable_irq |= SPCR_SPRIE;
	if (spsr & SPSR_SPTEF)
		disable_irq |= SPCR_SPTIE;

	if (disable_irq) {
		ret = IRQ_HANDLED;
		rspi_disable_irq(rspi, disable_irq);
		wake_up(&rspi->wait);
	}

	return ret;
}

static irqreturn_t rspi_irq_rx(int irq, void *_sr)
{
	struct rspi_data *rspi = _sr;
	u8 spsr;

	rspi->spsr = spsr = rspi_read8(rspi, RSPI_SPSR);
	if (spsr & SPSR_SPRF) {
		rspi_disable_irq(rspi, SPCR_SPRIE);
		wake_up(&rspi->wait);
		return IRQ_HANDLED;
	}

	return 0;
}

static irqreturn_t rspi_irq_tx(int irq, void *_sr)
{
	struct rspi_data *rspi = _sr;
	u8 spsr;

	rspi->spsr = spsr = rspi_read8(rspi, RSPI_SPSR);
	if (spsr & SPSR_SPTEF) {
		rspi_disable_irq(rspi, SPCR_SPTIE);
		wake_up(&rspi->wait);
		return IRQ_HANDLED;
	}

	return 0;
}

static struct dma_chan *rspi_request_dma_chan(struct device *dev,
					      enum dma_transfer_direction dir,
					      unsigned int id,
					      dma_addr_t port_addr)
{
	dma_cap_mask_t mask;
	struct dma_chan *chan;
	struct dma_slave_config cfg;
	int ret;

	dma_cap_zero(mask);
	dma_cap_set(DMA_SLAVE, mask);

	chan = dma_request_channel(mask, shdma_chan_filter,
				   (void *)(unsigned long)id);
	if (!chan) {
		dev_warn(dev, "dma_request_channel failed\n");
		return NULL;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.slave_id = id;
	cfg.direction = dir;
	if (dir == DMA_MEM_TO_DEV)
		cfg.dst_addr = port_addr;
	else
		cfg.src_addr = port_addr;

	ret = dmaengine_slave_config(chan, &cfg);
	if (ret) {
		dev_warn(dev, "dmaengine_slave_config failed %d\n", ret);
		dma_release_channel(chan);
		return NULL;
	}

	return chan;
}

static int rspi_request_dma(struct rspi_data *rspi,
			    struct platform_device *pdev)
{
	const struct rspi_plat_data *rspi_pd = dev_get_platdata(&pdev->dev);
	struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	if (!res || !rspi_pd)
		return 0;	/* The driver assumes no error. */

	/* If the module receives data by DMAC, it also needs TX DMAC */
	if (rspi_pd->dma_rx_id && rspi_pd->dma_tx_id) {
		rspi->chan_rx = rspi_request_dma_chan(&pdev->dev,
						      DMA_DEV_TO_MEM,
						      rspi_pd->dma_rx_id,
						      res->start + RSPI_SPDR);
		if (!rspi->chan_rx)
			return -ENODEV;

		dev_info(&pdev->dev, "Use DMA when rx.\n");
	}
	if (rspi_pd->dma_tx_id) {
		rspi->chan_tx = rspi_request_dma_chan(&pdev->dev,
						      DMA_MEM_TO_DEV,
						      rspi_pd->dma_tx_id,
						      res->start + RSPI_SPDR);
		if (!rspi->chan_tx)
			return -ENODEV;

		dev_info(&pdev->dev, "Use DMA when tx\n");
	}

	return 0;
}

static void rspi_release_dma(struct rspi_data *rspi)
{
	if (rspi->chan_tx)
		dma_release_channel(rspi->chan_tx);
	if (rspi->chan_rx)
		dma_release_channel(rspi->chan_rx);
}

static int rspi_remove(struct platform_device *pdev)
{
	struct rspi_data *rspi = platform_get_drvdata(pdev);

	rspi_release_dma(rspi);
	pm_runtime_disable(&pdev->dev);

	return 0;
}

static const struct spi_ops rspi_ops = {
	.set_config_register =	rspi_set_config_register,
	.transfer_one =		rspi_transfer_one,
	.mode_bits =		SPI_CPHA | SPI_CPOL | SPI_LOOP,
	.flags =		SPI_MASTER_MUST_TX,
};

static const struct spi_ops rspi_rz_ops = {
	.set_config_register =	rspi_rz_set_config_register,
	.transfer_one =		rspi_rz_transfer_one,
	.mode_bits =		SPI_CPHA | SPI_CPOL | SPI_LOOP,
	.flags =		SPI_MASTER_MUST_RX | SPI_MASTER_MUST_TX,
};

static const struct spi_ops qspi_ops = {
	.set_config_register =	qspi_set_config_register,
	.transfer_one =		qspi_transfer_one,
	.mode_bits =		SPI_CPHA | SPI_CPOL | SPI_LOOP |
				SPI_TX_DUAL | SPI_TX_QUAD |
				SPI_RX_DUAL | SPI_RX_QUAD,
	.flags =		SPI_MASTER_MUST_RX | SPI_MASTER_MUST_TX,
};

#ifdef CONFIG_OF
static const struct of_device_id rspi_of_match[] = {
	/* RSPI on legacy SH */
	{ .compatible = "renesas,rspi", .data = &rspi_ops },
	/* RSPI on RZ/A1H */
	{ .compatible = "renesas,rspi-rz", .data = &rspi_rz_ops },
	/* QSPI on R-Car Gen2 */
	{ .compatible = "renesas,qspi", .data = &qspi_ops },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, rspi_of_match);

static int rspi_parse_dt(struct device *dev, struct spi_master *master)
{
	u32 num_cs;
	int error;

	/* Parse DT properties */
	error = of_property_read_u32(dev->of_node, "num-cs", &num_cs);
	if (error) {
		dev_err(dev, "of_property_read_u32 num-cs failed %d\n", error);
		return error;
	}

	master->num_chipselect = num_cs;
	return 0;
}
#else
#define rspi_of_match	NULL
static inline int rspi_parse_dt(struct device *dev, struct spi_master *master)
{
	return -EINVAL;
}
#endif /* CONFIG_OF */

static int rspi_request_irq(struct device *dev, unsigned int irq,
			    irq_handler_t handler, const char *suffix,
			    void *dev_id)
{
	const char *base = dev_name(dev);
	size_t len = strlen(base) + strlen(suffix) + 2;
	char *name = devm_kzalloc(dev, len, GFP_KERNEL);
	if (!name)
		return -ENOMEM;
	snprintf(name, len, "%s:%s", base, suffix);
	return devm_request_irq(dev, irq, handler, 0, name, dev_id);
}

static int rspi_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct spi_master *master;
	struct rspi_data *rspi;
	int ret;
	const struct of_device_id *of_id;
	const struct rspi_plat_data *rspi_pd;
	const struct spi_ops *ops;

	master = spi_alloc_master(&pdev->dev, sizeof(struct rspi_data));
	if (master == NULL) {
		dev_err(&pdev->dev, "spi_alloc_master error.\n");
		return -ENOMEM;
	}

	of_id = of_match_device(rspi_of_match, &pdev->dev);
	if (of_id) {
		ops = of_id->data;
		ret = rspi_parse_dt(&pdev->dev, master);
		if (ret)
			goto error1;
	} else {
		ops = (struct spi_ops *)pdev->id_entry->driver_data;
		rspi_pd = dev_get_platdata(&pdev->dev);
		if (rspi_pd && rspi_pd->num_chipselect)
			master->num_chipselect = rspi_pd->num_chipselect;
		else
			master->num_chipselect = 2; /* default */
	};

	/* ops parameter check */
	if (!ops->set_config_register) {
		dev_err(&pdev->dev, "there is no set_config_register\n");
		ret = -ENODEV;
		goto error1;
	}

	rspi = spi_master_get_devdata(master);
	platform_set_drvdata(pdev, rspi);
	rspi->ops = ops;
	rspi->master = master;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	rspi->addr = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(rspi->addr)) {
		ret = PTR_ERR(rspi->addr);
		goto error1;
	}

	rspi->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(rspi->clk)) {
		dev_err(&pdev->dev, "cannot get clock\n");
		ret = PTR_ERR(rspi->clk);
		goto error1;
	}

	pm_runtime_enable(&pdev->dev);

	init_waitqueue_head(&rspi->wait);

	master->bus_num = pdev->id;
	master->setup = rspi_setup;
	master->auto_runtime_pm = true;
	master->transfer_one = ops->transfer_one;
	master->prepare_message = rspi_prepare_message;
	master->unprepare_message = rspi_unprepare_message;
	master->mode_bits = ops->mode_bits;
	master->flags = ops->flags;
	master->dev.of_node = pdev->dev.of_node;

	ret = platform_get_irq_byname(pdev, "rx");
	if (ret < 0) {
		ret = platform_get_irq_byname(pdev, "mux");
		if (ret < 0)
			ret = platform_get_irq(pdev, 0);
		if (ret >= 0)
			rspi->rx_irq = rspi->tx_irq = ret;
	} else {
		rspi->rx_irq = ret;
		ret = platform_get_irq_byname(pdev, "tx");
		if (ret >= 0)
			rspi->tx_irq = ret;
	}
	if (ret < 0) {
		dev_err(&pdev->dev, "platform_get_irq error\n");
		goto error2;
	}

	if (rspi->rx_irq == rspi->tx_irq) {
		/* Single multiplexed interrupt */
		ret = rspi_request_irq(&pdev->dev, rspi->rx_irq, rspi_irq_mux,
				       "mux", rspi);
	} else {
		/* Multi-interrupt mode, only SPRI and SPTI are used */
		ret = rspi_request_irq(&pdev->dev, rspi->rx_irq, rspi_irq_rx,
				       "rx", rspi);
		if (!ret)
			ret = rspi_request_irq(&pdev->dev, rspi->tx_irq,
					       rspi_irq_tx, "tx", rspi);
	}
	if (ret < 0) {
		dev_err(&pdev->dev, "request_irq error\n");
		goto error2;
	}

	ret = rspi_request_dma(rspi, pdev);
	if (ret < 0)
		dev_warn(&pdev->dev, "DMA not available, using PIO\n");

	ret = devm_spi_register_master(&pdev->dev, master);
	if (ret < 0) {
		dev_err(&pdev->dev, "spi_register_master error.\n");
		goto error3;
	}

	dev_info(&pdev->dev, "probed\n");

	return 0;

error3:
	rspi_release_dma(rspi);
error2:
	pm_runtime_disable(&pdev->dev);
error1:
	spi_master_put(master);

	return ret;
}

static struct platform_device_id spi_driver_ids[] = {
	{ "rspi",	(kernel_ulong_t)&rspi_ops },
	{ "rspi-rz",	(kernel_ulong_t)&rspi_rz_ops },
	{ "qspi",	(kernel_ulong_t)&qspi_ops },
	{},
};

MODULE_DEVICE_TABLE(platform, spi_driver_ids);

static struct platform_driver rspi_driver = {
	.probe =	rspi_probe,
	.remove =	rspi_remove,
	.id_table =	spi_driver_ids,
	.driver		= {
		.name = "renesas_spi",
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(rspi_of_match),
	},
};
module_platform_driver(rspi_driver);

MODULE_DESCRIPTION("Renesas RSPI bus driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Yoshihiro Shimoda");
MODULE_ALIAS("platform:rspi");
