/*
 * Driver for Broadcom BCM2835 SPI Controllers
 *
 * Copyright (C) 2013 Martin Sperl
 *
 * This driver is inspired by:
 * spi-ath79.c, Copyright (C) 2009-2011 Gabor Juhos <juhosg@openwrt.org>
 * spi-atmel.c, Copyright (C) 2006 Atmel Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>

#include <linux/device.h>
#include <linux/netdevice.h>

#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>

#include <linux/spi/spi.h>

#include <linux/can.h>
#include <linux/can/dev.h>
#include <linux/can/platform/mcp251x.h>

#define DRV_NAME        "mcp2515a"

static bool use_optimize = 1;
module_param(use_optimize, bool, 0);
MODULE_PARM_DESC(use_optimize, "Run the driver with spi_message_compile support");

#define spi_message_optimize(...)
#define spi_message_unoptimize(...)

/* the MCP commands */
#define MCP2515_CMD_WRITE         0x02
#define MCP2515_CMD_READ          0x03
#define MCP2515_CMD_MODIFY        0x05
#define MCP2515_CMD_STATUS        0xA0
#define MCP2515_CMD_RXSTATUS      0xB0
#define MCP2515_CMD_RESET         0xC0
#define MCP2515_CMD_READ_RX(i)    (0x90+(i<<2))
#define MCP2515_CMD_WRITE_TX(i)   (0x40+(i<<1))
#define MCP2515_CMD_REQ2SEND(i)   (0x80+i)

/* the MCP register */
#define MCP2515_REG_CANSTAT       0x0E
#define MCP2515_REG_CANCTRL       0x0F

#define MCP2515_REG_CANCTRL_REQOP_NORMAL   (0<<5)
#define MCP2515_REG_CANCTRL_REQOP_SLEEP    (1<<5)
#define MCP2515_REG_CANCTRL_REQOP_LOOPBACK (2<<5)
#define MCP2515_REG_CANCTRL_REQOP_LISTEN   (3<<5)
#define MCP2515_REG_CANCTRL_REQOP_CONFIG   (4<<5)

#define MCP2515_REG_CANCTRL_ABORT          (1<<4)
#define MCP2515_REG_CANCTRL_OSM            (1<<3)

#define MCP2515_REG_CANCTRL_CLKEN          (1<<2)
#define MCP2515_REG_CANCTRL_CLKPRE_1       (0)
#define MCP2515_REG_CANCTRL_CLKPRE_2       (1)
#define MCP2515_REG_CANCTRL_CLKPRE_4       (2)
#define MCP2515_REG_CANCTRL_CLKPRE_8       (3)

#define MCP2515_REG_CNF3          0x28
#define MCP2515_REG_CNF2          0x29
#define MCP2515_REG_CNF1          0x2A

#define MCP2515_REG_CANINTE       0x2B
#define MCP2515_REG_CANINTF       0x2C
#define MCP2515_REG_CANINT_MERR           (1<<7)
#define MCP2515_REG_CANINT_WAKI           (1<<6)
#define MCP2515_REG_CANINT_ERRI           (1<<5)
#define MCP2515_REG_CANINT_TX2I           (1<<4)
#define MCP2515_REG_CANINT_TX1I           (1<<3)
#define MCP2515_REG_CANINT_TX0I           (1<<2)
#define MCP2515_REG_CANINT_RX1I           (1<<1)
#define MCP2515_REG_CANINT_RX0I           (1<<0)

#define MCP2515_REG_EFLG          0x2D
#define MCP2515_REG_EFLG_RX1OVR            (1<<7)
#define MCP2515_REG_EFLG_RX0OVR            (1<<6)
#define MCP2515_REG_EFLG_TXBO              (1<<5)
#define MCP2515_REG_EFLG_TXEP              (1<<4)
#define MCP2515_REG_EFLG_RXEP              (1<<3)
#define MCP2515_REG_EFLG_TXWAR             (1<<2)
#define MCP2515_REG_EFLG_RXWAR             (1<<1)
#define MCP2515_REG_EFLG_EWARN             (1<<0)

#define MCP2515_REG_RXB0CTRL      0x60
#define MCP2515_REG_RXB1CTRL      0x70

#define MCP2515_REG_RXB_RXM_ANY   (3<<5)
#define MCP2515_REG_RXB_RXM_EXT   (2<<5)
#define MCP2515_REG_RXB_RXM_STD   (1<<5)
#define MCP2515_REG_RXB_RXM_FILTER (0<<5)

#define MCP2515_REG_RXB_RXRTR     (1<<2)
#define MCP2515_REG_RXB_BUKT      (1<<2)

#define MCP2515_REG_TXB0CTRL      0x30
#define MCP2515_REG_TXB1CTRL      0x40
#define MCP2515_REG_TXB2CTRL      0x50

#define MCP2515_REG_TXB_ABTF      (1<<6)
#define MCP2515_REG_TXB_MLOA      (1<<5)
#define MCP2515_REG_TXB_TXERR     (1<<4)
#define MCP2515_REG_TXB_TXREQ     (1<<3)
#define MCP2515_REG_TXB_TXP_0     (0)
#define MCP2515_REG_TXB_TXP_1     (1)
#define MCP2515_REG_TXB_TXP_2     (2)
#define MCP2515_REG_TXB_TXP_3     (3)

#define MCP2515_REG_BFPCTRL       0x0c
#define MCP2515_REG_BFP_B1BFS     (1<<5) /* pin RX1 value */
#define MCP2515_REG_BFP_B0BFS     (1<<4) /* pin RX0 value */
#define MCP2515_REG_BFP_B1BFE     (1<<3) /* pin RX1 enabled */
#define MCP2515_REG_BFP_B0BFE     (1<<2) /* pin RX0 enabled */
#define MCP2515_REG_BFP_B1BFM     (1<<1) /* pin RX1 as irq on buffer full */
#define MCP2515_REG_BFP_B0BFM     (1<<0) /* pin RX0 as irq on buffer full */

#define MCP2515_REG_TXRTSCTRL     0x0d
#define MCP2515_REG_TXRTS_B2RTS   (1<<5) /* pin TX2 value */
#define MCP2515_REG_TXRTS_B1RTS   (1<<4) /* pin TX1 value */
#define MCP2515_REG_TXRTS_B0RTS   (1<<3) /* pin TX0 value */
#define MCP2515_REG_TXRTS_B2RTSM  (1<<2) /* pin TX2 as irq to send message */
#define MCP2515_REG_TXRTS_B1RTSM  (1<<1) /* pin TX1 as irq to send message */
#define MCP2515_REG_TXRTS_B0RTSM  (1<<0) /* pin TX0 as irq to send message */

#define MCP2515_REG_TEC           0x1c
#define MCP2515_REG_REC           0x1d

#define MCP2515_CMD_STATUS_RX0IF   (1<<0)
#define MCP2515_CMD_STATUS_RX1IF   (1<<1)
#define MCP2515_CMD_STATUS_TX0REQ  (1<<2)
#define MCP2515_CMD_STATUS_TX0IF   (1<<3)
#define MCP2515_CMD_STATUS_TX1REQ  (1<<4)
#define MCP2515_CMD_STATUS_TX1IF   (1<<5)
#define MCP2515_CMD_STATUS_TX2REQ  (1<<6)
#define MCP2515_CMD_STATUS_TX2IF   (1<<7)

#define MCP2515_MSG_SIDH           0
#define MCP2515_MSG_SIDH_SHIFT     3

#define MCP2515_MSG_SIDL           1
#define MCP2515_MSG_SIDL_MASK      0xe0
#define MCP2515_MSG_SIDL_SHIFT     5
#define MCP2515_MSG_SIDL_EMASK     0x03

#define MCP2515_MSG_SIDL_SRR       (1<<4)
#define MCP2515_MSG_SIDL_IDE       (1<<3)
#define MCP2515_MSG_EIDH           2
#define MCP2515_MSG_EIDL           3
#define MCP2515_MSG_DCL            4
#define MCP2515_MSG_DCL_RTR        (1<<6)
#define MCP2515_MSG_DCL_MASK        0x0f
#define MCP2515_MSG_DATA           5

struct mcp2515a_transfers;

/* the driver structure */
struct mcp2515a_priv {
        struct can_priv    can;
        struct net_device *net;
        struct spi_device *spi;

	/* the DMA Buffer */
	dma_addr_t transfers_dma_addr;
	/* the transfers */
	struct mcp2515a_transfers *transfers;
	/* the last transfer structure used */
	u8 structure_used;

	/* some counters for RX sent/received */
	volatile u32 status_sent_count;
	volatile u32 status_callback_count;

	/* the structures needed for TX */
	struct completion can_transmit;
	u8 tx_status; /* the one from the mcp2515 */
	u8 tx_mask; /* the mask that transmits create temporarily while
		       everything is in flight
		       - we might need to lock this...*/

	/* some states */
	u8 carrier_off;
	/* flag to say we are shutting down */
	u8 is_shutdown;
};

static const struct can_bittiming_const mcp2515a_bittiming_const = {
        .name = "mcp2515",
        .tseg1_min = 2,
        .tseg1_max = 16,
        .tseg2_min = 2,
        .tseg2_max = 8,
        .sjw_max = 4,
        .brp_min = 1,
        .brp_max = 64,
        .brp_inc = 1,
};

static int mcp2515a_reset(struct spi_device *spi)
{
        const u8 reset = 0xc0;

        return spi_write(spi, &reset, sizeof(reset));
}

/* confirm that this is the "correct" device */
int mcp2515a_confirm_device(struct spi_device *spi)
{
	/* the buffer to transmit/receive */
	u8 b[4];
	int ret;

	/* send a reset - one byte */
	ret = mcp2515a_reset(spi);
	if (ret)
		return ret;
	/* now wait for 10 ms - as per documentation */
	mdelay(10);
	/* and read the canstat and other values */
	b[0] = MCP2515_CMD_READ;
	b[1] = MCP2515_REG_CANSTAT;
	b[2] = 0;
	b[3] = 0;
	ret = spi_write_then_read(spi,b,2,b+2,2);
	if (ret)
		return ret;

	/* check canstat against magic numbers */
	/* CANSTAT */
	b[2] &= 0xEE;
	if (b[2] != 0x80)
		goto mcp2515a_confirm_device_err;
	b[3] &= 0x17;
	if (b[3] != 0x07)
		goto mcp2515a_confirm_device_err;

	/* ok, we have succeeded */
	return 0;

mcp2515a_confirm_device_err:
	dev_err(&spi->dev,
		"Found unexpected response from device:"
		" CANSTAT 0x%02x CANCTRL 0x%02x - not detected\n",
		b[2], b[3]);
	return -ENODEV;
}

#define TRANSFER_WRITE_STRUCT(name,datalen) \
	struct { \
		u8 cmd; \
		u8 reg; \
		u8 data[datalen]; \
		struct spi_transfer t_tx;\
	} name;
#define TRANSFER_INIT(base,message,callback,callbackdata)		\
	spi_message_init(&base->message.msg);				\
	base->message.msg.is_dma_mapped = 1;				\
	base->message.msg.complete = callback;				\
	base->message.msg.context = callbackdata;
#define TRANSFER_INIT_WRITE(base,message,xfer,regist)			\
	base->message.xfer.data;					\
	base->message.xfer.cmd = MCP2515_CMD_WRITE;			\
	base->message.xfer.reg = regist;				\
	base->message.xfer.t_tx.len = 2					\
		+ sizeof(base->message.xfer.data);			\
	base->message.xfer.t_tx.tx_buf = &base->message.xfer.cmd;	\
	base->message.xfer.t_tx.rx_buf = NULL;				\
	base->message.xfer.t_tx.tx_dma = base##_dma_addr		\
		+ offsetof(struct mcp2515a_transfers,message.xfer.cmd);	\
	base->message.xfer.t_tx.rx_dma = 0;				\
	base->message.xfer.t_tx.cs_change = 1;				\
	spi_message_add_tail(&base->message.xfer.t_tx,&base->message.msg);
#define TRANSFER_READ_STRUCT(name,datalen) \
	struct { \
		u8 cmd; \
		u8 reg; \
		u8 data[datalen]; \
		struct spi_transfer t_tx;\
		struct spi_transfer t_rx;\
	} name;
#define TRANSFER_INIT_READ(base,message,xfer,regist)			\
	base->message.xfer.data;					\
	base->message.xfer.cmd = MCP2515_CMD_READ;			\
	base->message.xfer.reg = regist;				\
	base->message.xfer.t_tx.len = 2;				\
	base->message.xfer.t_tx.tx_buf = &base->message.xfer.cmd;	\
	base->message.xfer.t_tx.rx_buf = NULL;				\
	base->message.xfer.t_tx.tx_dma = base##_dma_addr		\
		+ offsetof(struct mcp2515a_transfers,message.xfer.cmd);	\
	base->message.xfer.t_rx.rx_dma = 0;				\
	spi_message_add_tail(&base->message.xfer.t_tx,&base->message.msg); \
	base->message.xfer.t_rx.len = sizeof(base->message.xfer.data);	\
	base->message.xfer.t_rx.rx_buf = &base->message.xfer.data;	\
	base->message.xfer.t_rx.tx_buf = NULL;				\
	base->message.xfer.t_rx.rx_dma = base##_dma_addr		\
		+ offsetof(struct mcp2515a_transfers,message.xfer.data); \
	base->message.xfer.t_rx.tx_dma = 0;				\
	base->message.xfer.t_rx.cs_change = 1;				\
	spi_message_add_tail(&base->message.xfer.t_rx,&base->message.msg);

struct mcp2515a_transfers {
	/* the messages */
	struct {
		struct spi_message msg;
		TRANSFER_WRITE_STRUCT(setRXB0ctrl,1);
		TRANSFER_WRITE_STRUCT(setRXB1ctrl,1);
		TRANSFER_WRITE_STRUCT(seterrorcounter,2);
		TRANSFER_WRITE_STRUCT(changetoconfigmode,1);
		TRANSFER_WRITE_STRUCT(setpinctrl,2);
		TRANSFER_WRITE_STRUCT(setconfig,5);
		TRANSFER_WRITE_STRUCT(changetomode,1);
		TRANSFER_READ_STRUCT(readconfig,8);
	} config;
	/* the messages we schedule in response to the irq pin going down */
	struct {
		struct spi_message msg;
		/* read the status flags quickly */
		TRANSFER_READ_STRUCT(read_status,1);
		/* read the interrupt mask, sources and the error flags */
		TRANSFER_READ_STRUCT(read_inte_intf_eflg,3);
		/* clear the interrupts */
		TRANSFER_WRITE_STRUCT(clear_inte,1);
	} read_status;
	/* the above has a callback, this second is there to allow for
	 * HW/SW concurrency while the below transfer happens, we may have
	 * the time to handle the above information in the callback(irq)
	 * handler. as the most likley result is that the interrupt source
	 * is for a receive of messages, we read the message in preparation
	 *  already, so that we only have to acknowledge it...
	 */
	struct {
		struct spi_message msg;
		TRANSFER_READ_STRUCT(read_tec_rec,2);
		TRANSFER_READ_STRUCT(read_rx0,13);
		TRANSFER_READ_STRUCT(read_status,1);
	} read_status2;
	/* the message we send from the callback
	 * - there are 8 variants of this*/
#define CALLBACK_CLEAR       (1<<0)
#define CALLBACK_ACK_RX0     (1<<1)
#define CALLBACK_READACK_RX1 (1<<2)
	struct {
		struct spi_message msg;
		/* clear overflow flags if needed */
		TRANSFER_WRITE_STRUCT(clear_rxoverflow,2);
		/* acknowledge RX0 - if we need it */
		TRANSFER_WRITE_STRUCT(ack_rx0,0);
		/* read+acknowledge RX1 - if we need it */
		TRANSFER_READ_STRUCT(readack_rx1,13);
		/* and reenable interrupts by setting
		 * the "corresponding" irq_mask */
		TRANSFER_WRITE_STRUCT(set_irq_mask,1);
	} callback_action[8];
	/* the transfers */
	struct {
		struct spi_message msg;
		TRANSFER_WRITE_STRUCT(message,14);
		TRANSFER_WRITE_STRUCT(transmit,0);
	} transmit_tx2;
	struct {
		struct spi_message msg;
		TRANSFER_WRITE_STRUCT(message,14);
		TRANSFER_WRITE_STRUCT(transmit,0);
	} transmit_tx1;
	struct {
		struct spi_message msg;
		TRANSFER_WRITE_STRUCT(message,14);
		TRANSFER_WRITE_STRUCT(transmit,0);
	} transmit_tx0;
};

/* the complete callbacks and interrupt handlers */
static irqreturn_t mcp2515a_interrupt_handler(int, void *);
static void mcp2515a_completed_read_status (void *);
static void mcp2515a_completed_transfers (void *);
static void mcp2515a_completed_dummy (void *data) { ; }

static void mcp2515a_free_transfers(struct net_device* net)
{
	struct mcp2515a_priv *priv = netdev_priv(net);
        //struct spi_device *spi = priv->spi;
	int i;
	/* if we have been prepared, then release us */
	if (priv->transfers) {
		/* first unprepare messages */
		spi_message_unoptimize(spi,
				&priv->transfers->config.msg);
		spi_message_unoptimize(spi,
				&priv->transfers->read_status.msg);
		spi_message_unoptimize(spi,
				&priv->transfers->read_status2.msg);
		for( i=0 ; i<8 ; i++) {
			spi_message_unoptimize(
				spi,
				&priv->transfers->callback_action[i].msg);
		}
		spi_message_unoptimize(spi,
				&priv->transfers->transmit_tx2.msg);
		spi_message_unoptimize(spi,
				&priv->transfers->transmit_tx1.msg);
		spi_message_unoptimize(spi,
				&priv->transfers->transmit_tx0.msg);
		/* now release the structures */
		dma_free_coherent(&priv->spi->dev,
				sizeof(struct mcp2515a_transfers),
				priv->transfers,
				priv->transfers_dma_addr);
	}
}

static int mcp2515a_init_transfers(struct net_device* net)
{
	struct mcp2515a_priv *priv = netdev_priv(net);
        struct spi_device *spi = priv->spi;
        struct device *dev = &spi->dev;
	u8* data;
	int i;
	/* first forece the coherent mask
	   - a bit of a hack, but at least it works... */
        dev->coherent_dma_mask = 0xffffffff;

	/* allocate memory */
	priv->transfers = dma_zalloc_coherent(
		&priv->spi->dev,
		sizeof(struct mcp2515a_transfers),
		&priv->transfers_dma_addr, GFP_KERNEL);
	if (! priv->transfers) return -ENOMEM;

	/* now let us fill in the data structures */

	/* setting up receive policies for buffers */
	TRANSFER_INIT(priv->transfers,config,NULL,NULL);
	data = TRANSFER_INIT_WRITE(priv->transfers,
				config,
				setRXB0ctrl,
				MCP2515_REG_RXB0CTRL);
	data[0] = MCP2515_REG_RXB_RXM_ANY /* receive any message */
                | MCP2515_REG_RXB_BUKT; /* rollover to RXB1*/
	data = TRANSFER_INIT_WRITE(priv->transfers,
				config,
				setRXB1ctrl,
				MCP2515_REG_RXB1CTRL);
	data[0] = MCP2515_REG_RXB_RXM_ANY; /* receive any message */

	/* clear TEC/REC */
	data = TRANSFER_INIT_WRITE(priv->transfers,
				config,
				seterrorcounter,
				MCP2515_REG_TEC);
	data[0] = 0; /* tec */
	data[1] = 0; /* rec */

	/* move to config mode */
	data = TRANSFER_INIT_WRITE(priv->transfers,
				config,
				changetoconfigmode,
				MCP2515_REG_CANCTRL);
	data[0] = MCP2515_REG_CANCTRL_REQOP_CONFIG;

	/* clear bit ctrl */
	data = TRANSFER_INIT_WRITE(priv->transfers,
				config,
				setpinctrl,
				MCP2515_REG_BFPCTRL);
	data[0] = 0; /* BFPCTRL - high impedance */
	data[1] = 0; /* TXRTSCTRL - no functionality*/

	/* configure CNF and interrupt-registers */
	data = TRANSFER_INIT_WRITE(priv->transfers,
				config,
				setconfig,
				MCP2515_REG_CNF3);
	data[0] = data[1] = data[2] = 0; /* CNF3,CNF2,CNF1 */
	data[3] = 0xff; /* INTE - enable all interupt sources */
	data[4] = 0; /* INTF - clear all interupt sources */

	/* and change to the final mode we want to enter */
	data = TRANSFER_INIT_WRITE(priv->transfers,
				config,
				changetomode,
				MCP2515_REG_CANCTRL);
	data[0] = MCP2515_REG_CANCTRL_REQOP_NORMAL;

	/* initiate read of basic configs */
	data = TRANSFER_INIT_READ(priv->transfers,
				config,
				readconfig,
				MCP2515_REG_CNF3);

	if (use_optimize)
		spi_message_optimize(spi,
				&priv->transfers->config.msg,
				GFP_KERNEL);

	/* The read status transfer with the callback */
	TRANSFER_INIT(priv->transfers,
		read_status,
		mcp2515a_completed_read_status,
		net);
	/* add the STATUS read transfer
	   - we modify the length and command */
	data = TRANSFER_INIT_READ(priv->transfers,
				read_status,
				read_status,
				0);
	priv->transfers->read_status.read_status.cmd = MCP2515_CMD_STATUS;
	priv->transfers->read_status.read_status.t_tx.len = 1;
	/* add the read interrupts and error registers */
	data = TRANSFER_INIT_READ(priv->transfers,
				read_status,
				read_inte_intf_eflg,
				MCP2515_REG_CANINTE);
	/* and clear the interrupt mask, so no more IRQ occurs */
	data = TRANSFER_INIT_WRITE(priv->transfers,
				read_status,
				clear_inte,
				MCP2515_REG_CANINTE);
	data[0] = 0;
	if (use_optimize)
		spi_message_optimize(spi,
				&priv->transfers->read_status.msg,
				GFP_KERNEL);

	/* the second status message that gets scheduled
	 * - this time without callbacks for some reason 3.13 interuduced
	 * the requirement for complete to be set - so some dummy code */
	TRANSFER_INIT(priv->transfers,
		read_status2,
		mcp2515a_completed_dummy,
		NULL);
	/* we read the error-count */
	data = TRANSFER_INIT_READ(priv->transfers,
				read_status2,
				read_tec_rec,
				MCP2515_REG_TEC);
	/* and we read the RX0 buffer... */
	data = TRANSFER_INIT_READ(priv->transfers,
				read_status2,
				read_rx0,
				MCP2515_REG_RXB0CTRL+1);
	data = TRANSFER_INIT_READ(priv->transfers,
				read_status2,
				read_status,
				0);
	priv->transfers->read_status2.read_status.cmd = MCP2515_CMD_STATUS;
	priv->transfers->read_status2.read_status.t_tx.len = 1;
	if (use_optimize)
		spi_message_optimize(spi,
				&priv->transfers->read_status2.msg,
				GFP_KERNEL);

	/* and the callback action transfer in all variants - we want to
	 *  prepare the statements...
	 * note that we will need to change the order in which we run
	 * this dependent on status flags from above
	 */
	for ( i=0 ; i<8; i++) {
		TRANSFER_INIT(priv->transfers,
			callback_action[i],
			mcp2515a_completed_transfers,net);
		/* acknowledge the buffer overflows */
		if (i & CALLBACK_CLEAR) {
			data = TRANSFER_INIT_WRITE(priv->transfers,
						callback_action[i],
						clear_rxoverflow,
						MCP2515_REG_EFLG);
			priv->transfers
				->callback_action[i].clear_rxoverflow.cmd =
				MCP2515_CMD_MODIFY;
			data[0] = MCP2515_REG_EFLG_RX1OVR
				| MCP2515_REG_EFLG_RX0OVR;
			data[1] = 0;
		}
		/* acknowledge the RX0 buffer */
		if (i & CALLBACK_ACK_RX0) {
			data = TRANSFER_INIT_WRITE(priv->transfers,
						callback_action[i],
						ack_rx0,
						0);
			priv->transfers
				->callback_action[i].ack_rx0.cmd =
				MCP2515_CMD_READ_RX(0);
			priv->transfers
				->callback_action[i].ack_rx0.t_tx.len = 1;
		}
		/* read and acknowledge rx1 */
		if (i & CALLBACK_READACK_RX1) {
			data=TRANSFER_INIT_READ(priv->transfers,
						callback_action[i],
						readack_rx1,
						0);
			priv->transfers->
				callback_action[i].readack_rx1.cmd =
				MCP2515_CMD_READ_RX(1);
			priv->transfers->
				callback_action[i].readack_rx1.t_tx.len = 1;
		}
		/* and set the IRQ mask back again */
		data = TRANSFER_INIT_WRITE(priv->transfers,
					callback_action[i],
					set_irq_mask,
					MCP2515_REG_CANINTE);
		data[0] = 0x3f; /* no MERRE, WAKIE */
		/* and prepare the message */
		if (use_optimize)
			spi_message_optimize(spi,
					&priv->transfers
					->callback_action[i].msg,
					GFP_KERNEL);
	}

	/* setup TX2 */
	TRANSFER_INIT(priv->transfers,
		transmit_tx2,
		mcp2515a_completed_dummy,
		NULL);
	data = TRANSFER_INIT_WRITE(priv->transfers,
				transmit_tx2,
				message,
				MCP2515_REG_TXB2CTRL);
	data = TRANSFER_INIT_WRITE(priv->transfers,
				transmit_tx2,
				transmit,
				0);
	priv->transfers->transmit_tx2.transmit.cmd =
		MCP2515_CMD_REQ2SEND((1<<2));
	priv->transfers->transmit_tx2.transmit.t_tx.len = 1;
	if (use_optimize)
		spi_message_optimize(spi,
				&priv->transfers->transmit_tx2.msg,
				GFP_KERNEL);
	/* setup TX1 */
	TRANSFER_INIT(priv->transfers,
		transmit_tx1,
		mcp2515a_completed_dummy,
		NULL);
	data = TRANSFER_INIT_WRITE(priv->transfers,
				transmit_tx1,
				message,
				MCP2515_REG_TXB2CTRL);
	data = TRANSFER_INIT_WRITE(priv->transfers,
				transmit_tx1,
				transmit,
				0);
	priv->transfers->transmit_tx1.transmit.cmd =
		MCP2515_CMD_REQ2SEND((1<<1));
	priv->transfers->transmit_tx1.transmit.t_tx.len = 1;
	if (use_optimize)
		spi_message_optimize(spi,
				&priv->transfers->transmit_tx1.msg,
				GFP_KERNEL);
	/* setup TX0 */
	TRANSFER_INIT(priv->transfers,
		transmit_tx0,
		mcp2515a_completed_dummy,
		NULL);
	data = TRANSFER_INIT_WRITE(priv->transfers,
				transmit_tx0,
				message,
				MCP2515_REG_TXB2CTRL);
	data = TRANSFER_INIT_WRITE(priv->transfers,
				transmit_tx0,
				transmit,
				0);
	priv->transfers->transmit_tx0.transmit.cmd =
		MCP2515_CMD_REQ2SEND((1<<0));
	priv->transfers->transmit_tx0.transmit.t_tx.len = 1;
	if (use_optimize)
		spi_message_optimize(spi,
				&priv->transfers->transmit_tx0.msg,
				GFP_KERNEL);
	/* and return ok */
	return 0;
}

static int mcp2515a_config(struct net_device *net)
{
        struct mcp2515a_priv *priv = netdev_priv(net);
        struct spi_device *spi = priv->spi;
        struct can_bittiming *bt = &priv->can.bittiming;
	int ret = 0;
	u8 mode = 0;

	/* select the mode we want */
	if (priv->can.ctrlmode & CAN_CTRLMODE_LOOPBACK) {
		mode = MCP2515_REG_CANCTRL_REQOP_LOOPBACK;
        } else if (priv->can.ctrlmode & CAN_CTRLMODE_LISTENONLY) {
		mode = MCP2515_REG_CANCTRL_REQOP_LISTEN;
        } else {
		mode = MCP2515_REG_CANCTRL_REQOP_NORMAL;
	}
	/* set one-shot mode if needed */
	if (priv->can.ctrlmode & CAN_CTRLMODE_ONE_SHOT)
		mode |= MCP2515_REG_CANCTRL_OSM;
	priv->transfers->config.changetomode.data[0]=mode;

	/* the Bit speed config */
	/* CNF3 */
	priv->transfers->config.setconfig.data[0] =
		bt->phase_seg2 - 1;
	/* CNF2 */
	priv->transfers->config.setconfig.data[1] =
		(priv->can.ctrlmode & CAN_CTRLMODE_3_SAMPLES ? 0xc0 : 0x80)
		| (bt->phase_seg1 - 1) << 3
		| (bt->prop_seg - 1)
		;
	/* CNF1 */
	priv->transfers->config.setconfig.data[2] =
		(bt->sjw - 1) << 6
		| (bt->brp - 1);

	/* execute transfer */
	ret = spi_sync(spi,&priv->transfers->config.msg);
	if (ret)
		return ret;
	/* dump the CNF */
	netdev_info(net, "configured CTRL: 0x%02x"
		" CNF: 0x%02x 0x%02x 0x%02x\n",
		priv->transfers->config.readconfig.data[7],
		priv->transfers->config.readconfig.data[2],
		priv->transfers->config.readconfig.data[1],
		priv->transfers->config.readconfig.data[0]
		);

	/* and return */
	return 0;
}

static int mcp2515a_do_set_mode(struct net_device *net, enum can_mode mode)
{
	if (mode == CAN_MODE_START)
		return mcp2515a_config(net);
	/* otherwise send message that it is not supported... */
	return -EOPNOTSUPP;
}

static void mcp2515a_queue_rx_message(struct mcp2515a_priv *priv, char* data)
{
        struct sk_buff *skb;
        struct can_frame *frame;

	/* increment packet counter */
	priv->net->stats.rx_packets++;

	/* allocate message buffer */
	skb = alloc_can_skb(priv->net, &frame);
        if (!skb) {
                dev_err(&priv->net->dev, "cannot allocate RX skb\n");
                priv->net->stats.rx_dropped++;
                return;
        }

	/* parse the can_id */
	/* and handle extended - if needed */
	if (data[MCP2515_MSG_SIDL]|MCP2515_MSG_SIDL_IDE) {
		frame->can_id |=
			/* the extended flag */
			CAN_EFF_FLAG
			/* RTR */
			| ((data[MCP2515_MSG_DCL] & MCP2515_MSG_DCL_RTR) ?
				CAN_RTR_FLAG : 0)
			/* the extended Address - top 2 bits */
			| ((data[MCP2515_MSG_SIDL]&MCP2515_MSG_SIDL_EMASK)
				<< (2*8+11))
			/* the extended Address EXTH */
			| (data[MCP2515_MSG_EIDH] << (1*8+11))
			/* the extended Address EXTL */
			| (data[MCP2515_MSG_EIDL] << (0*8+11))
			;
	} else {
		frame->can_id =
			/* handle RTR */
			( (data[MCP2515_MSG_SIDL] & MCP2515_MSG_SIDL_SRR) ?
				CAN_RTR_FLAG : 0)
			;
	}
	/* and add the standard header */
	frame->can_id |=
		(data[MCP2515_MSG_SIDH] << 3)
		| (data[MCP2515_MSG_SIDL] >> 5)
		;
	/* get data length */
	frame->can_dlc = get_can_dlc(
		data[MCP2515_MSG_DCL] & MCP2515_MSG_DCL_MASK);

	/* copy data */
	memcpy(frame->data,
		data + MCP2515_MSG_DATA,
		frame->can_dlc);

	/* increment byte counters */
        priv->net->stats.rx_bytes += frame->can_dlc;

	/* call can_led_event */
	can_led_event(priv->net, CAN_LED_EVENT_RX);

	/* and schedule packet */
	netif_rx_ni(skb);
}

static void mcp2515a_completed_read_status_error (struct net_device *net)
{
	struct mcp2515a_priv *priv = netdev_priv(net);
	struct mcp2515a_transfers *trans = priv->transfers;
	/* some status registers, we may need... */
	/*
	  u8 inte=trans->read_status.read_inte_intf_eflg.data[0];
	  u8 intf=trans->read_status.read_inte_intf_eflg.data[1];
	*/
	u8 eflg = trans->read_status.read_inte_intf_eflg.data[2];
	/* the error flags */
	u32 err_id = 0;
	u8 err_detail = 0;
	/* overflow occurred */
	if (eflg&(MCP2515_REG_EFLG_RX1OVR)) {
		/* increment counters */
		net->stats.rx_over_errors++;
		net->stats.rx_errors++;
		/* add bits */
		err_id |= CAN_ERR_CRTL;
		err_detail |= CAN_ERR_CRTL_RX_OVERFLOW;
	}
	/* the error handler for CAN BUS problems */
	if ( eflg & (MCP2515_REG_EFLG_TXEP|MCP2515_REG_EFLG_RXEP) ) {
		if (!priv->carrier_off) {
			dev_err(&net->dev,
				"CAN-Bus-error detected"
				" - Error-flags: 0x%02x\n",
				eflg);
			priv->net->stats.tx_carrier_errors++;
			netif_carrier_off(net);
			priv->carrier_off = 1;
			/* maybe reset here? */
			/* maybe change "interrupt-mask" ? */
		}
	} else {
		/* otherwise clear the message */
		if (priv->carrier_off) {
			dev_err(&net->dev,"CAN-Bus-error recovered\n");
			netif_carrier_on(net);
			priv->carrier_off = 0;
		}
	}
	/* and deliver the error message if there is some data...*/
	if (err_id) {
		struct sk_buff *skb;
		struct can_frame *frame;

		/* allocate a frame to deliver */
		skb = alloc_can_err_skb(priv->net, &frame);
		if (!skb) {
			netdev_err(priv->net,
				"cannot allocate error message\n");
			return;
		}
		/* assign data to it */
		frame->can_id |= err_id;
		frame->data[1] = err_detail;
		/* and deliver it */
		netif_rx_ni(skb);
	}
}

static void mcp2515a_completed_read_status (void* context)
{
	struct net_device *net = context;
	struct mcp2515a_priv *priv = netdev_priv(net);
	struct mcp2515a_transfers *trans = priv->transfers;
	/* get the status bits */
	u8 status = trans->read_status.read_status.data[0];
	u8 eflg = trans->read_status.read_inte_intf_eflg.data[2];
	/* structure to use*/
	u8 structure = 0;
	/* return early if we are shutdown */
	if (priv->is_shutdown)
		return;
	/* increment callback counter */
	priv->status_callback_count++;
	/* decide which structure we use */
	if (eflg & (MCP2515_REG_EFLG_RX1OVR|MCP2515_REG_EFLG_RX0OVR))
		structure |= CALLBACK_CLEAR;
	if (status & MCP2515_CMD_STATUS_RX0IF )
		structure |= CALLBACK_ACK_RX0;
	if (status & MCP2515_CMD_STATUS_RX1IF )
		structure |= CALLBACK_READACK_RX1;
	/* and enable our own interrupts before actually scheduling the
	 * transfer - this is happening here avoiding a race condition... */
	printk(KERN_INFO "XXXX %02x %02x %02x %08x\n",
		status,eflg,structure,
		trans->read_status.read_status.t_rx.rx_dma);
	enable_irq(priv->spi->irq);

	/* schedule the transfer */
	spi_async(priv->spi,&trans->callback_action[structure].msg);
	/* and assign it for the callback */
	priv->structure_used = structure;

	/* copy status back to tx_status */
	priv->tx_status = status & (
		MCP2515_CMD_STATUS_TX0REQ
		| MCP2515_CMD_STATUS_TX1REQ
		| MCP2515_CMD_STATUS_TX2REQ);
	/* and wake up if there are some bits NOT set */
	if (priv->tx_status ^ (
			MCP2515_CMD_STATUS_TX0REQ
			| MCP2515_CMD_STATUS_TX1REQ
			| MCP2515_CMD_STATUS_TX2REQ)
		)
		complete(&priv->can_transmit);

	/* now handle the error situations - if such have occurred */
	mcp2515a_completed_read_status_error(net);
}

void mcp2515a_completed_transfers (void *context)
{
	struct net_device *net = context;
	struct mcp2515a_priv *priv = netdev_priv(net);
	struct mcp2515a_transfers *trans = priv->transfers;
	u8 structure = priv->structure_used;
	/* get the status bits */
	u8 status = trans->read_status.read_status.data[0];
	/* return early if we are shutdown */
	if (priv->is_shutdown)
		return;
	/* and schedule RX0 to network stack */
	if (status & MCP2515_CMD_STATUS_RX0IF ) {
		mcp2515a_queue_rx_message(
			priv,trans->read_status2.read_rx0.data);
	}
	/* and schedule RX1 to network stack */
	if (status & MCP2515_CMD_STATUS_RX1IF ) {
		priv->net->stats.rx_fifo_errors++;
		mcp2515a_queue_rx_message(
			priv,
			trans->callback_action[structure].readack_rx1.data);
	}
}

/* the interrupt-handler for this device */
static irqreturn_t mcp2515a_interrupt_handler(int irq, void *dev_id)
{
        struct net_device *net = dev_id;
        struct mcp2515a_priv *priv = netdev_priv(net);
        struct spi_device *spi = priv->spi;
	int err = 0;
	printk(KERN_INFO "mcp2515-IRQ triggered\n");
	/* we will just schedule the 2 status transfers (of which
	 * the first generates a callback, while the second is just
	 *  pending...) */
	if (!priv->is_shutdown) {
		err = spi_async(spi,&priv->transfers->read_status.msg);
		err = spi_async(spi,&priv->transfers->read_status2.msg);

		/* increment sent counter */
		priv->status_sent_count++;
	}

	/* disable the interrupt - one of the handlers will reenable it */
	disable_irq_nosync(irq);

	/* return with a andled interrupt */
        return IRQ_HANDLED;
}

static netdev_tx_t mcp2515a_start_xmit(struct sk_buff *skb,
				struct net_device *net)
{
	/* check some messages */
        if (can_dropped_invalid_skb(net, skb))
                return NETDEV_TX_OK;
	/* scheduling is as simple as this:
	 * * we make use of TX2 before TX1 and finally TX0.
	 * * we also make use of decreasing priority configs
	 * both means that we run the following sequence
	 * * TX2-P3
	 * * TX1-P3
	 * * TX0-P3
	 * * TX2-P2
	 * * TX1-P2
	 * * TX0-P2
	 * * TX2-P1
	 * * TX1-P1
	 * * TX0-P1
	 * * TX2-P0
	 * * TX1-P0
	 * * TX0-P0
	 * now we have to wait until the messages buffer TX0 has been
	 * transmitted before we may continue.
	 * this introduces a "delivery" latency every 12 packets where the
	 * queue has to fully empty.
	 * but then - as soon as TX2,TX1,TX0 are all "empty" the Priority
	 * gets reset back to highest, which should allow to delay the
	 * full idle-delay delay for a lot longer...
	 * this is the drawback of making sure that TX packets get sent
	 * out in sequence under any circumstances...
	 * we could provide an option to avoid this penalty by adding
	 * a module parameter to ignore these delays and schedule
	 * immediately...
	 *
	 * for most practical purposes we will just schedule the message
	 * directly via SPI and if we have to wait, then we will delay the
	 * transfer
	 */

	/* the other thing is that we have the oportunity for
	 * race-conditions... a message might be in "transfer" while the
	 * DMA is reading the next status now - so this can result in a
	 * buffer overwritten - how to resolve tht is a good question
	 */
#if 1
	return NETDEV_TX_BUSY;
#else
	if (!try_wait_for_completion(&priv->can_transfer))
		return NETDEV_TX_BUSY;

	/* first check on priority reset back to 3 */
	if (priv->tx_status == 0)
		priv->tx_priority = 3;

	/* now decide which buffer we should use */
	if (!(priv->tx_status&MCP2515_CMD_STATUS_TX2REQ)) {
		tx = 2;
	} else if (!(priv->tx_status&MCP2515_CMD_STATUS_TX1REQ)) {
		tx = 1;
	} else if (!(priv->tx_status&MCP2515_CMD_STATUS_TX0REQ)) {
		tx = 0;
	}
	/* so fill in that buffer*/
	/* transfer and forget it */

	/* and return message as delivered */
	return NETDEV_TX_OK;
#endif
}

static int mcp2515a_open(struct net_device *net)
{
        struct mcp2515a_priv *priv = netdev_priv(net);
        struct spi_device *spi = priv->spi;
	//struct mcp251x_platform_data *pdata = spi->dev.platform_data;
        int ret;
	int flags=0;

	/* just in case identify the device again */
        ret = mcp2515a_confirm_device(spi);
        if (ret)
                return ret;

	/* try to open the can device */
        ret = open_candev(net);
        if (ret)
                return ret;

	/* the interrupt flag calculation */
/*
	if (pdata->irq_flags)
		flags = pdata->irq_flags;
	else
*/
		/* this does not work for some reason o n the RPI:
		 * flags |= IRQF_TRIGGER_LOW;
		 * so we are having to "cope" with edge-interrupts,
		 * which may bring other problems...
		 */
	flags = IRQF_TRIGGER_FALLING;
	/* request the IRQ with the above flags */
        ret = request_irq(spi->irq,
			mcp2515a_interrupt_handler,
			flags,
			net->name,
			net);
        if (ret)
                goto err_candev;

	/* now configure the device */
        ret = mcp2515a_config(net);
        if (ret)
                goto err_irq;
	/* and wake the queue, telling it, that we are receving data */
        netif_wake_queue(net);

	/* and log a message */
	dev_info(&net->dev,
		"Started device with irq %i and irq flags=0x%x\n",
		spi->irq,flags);

	/* and return */
        return 0;

err_irq:
	/* and free the IRQ */
        free_irq(spi->irq, net);
	/* send a syncronous reset */
	mcp2515a_reset(spi);
err_candev:
	dev_err(&net->dev,
		"MCP2515 Got an error opening the device - %i\n",ret);
	/* and close the device */
	close_candev(net);
	/* and return */
        return ret;
}

static int mcp2515a_stop(struct net_device *dev)
{
        struct mcp2515a_priv *priv = netdev_priv(dev);
        struct spi_device *spi = priv->spi;
	/* mark us as shut down */
	priv->is_shutdown = 1;

	/* first free the interrupt */
        free_irq(spi->irq, dev);

	/* send a syncronous reset
	 * - anything that is in-flight should stop after this...*/
        mcp2515a_reset(spi);

	/* close the device */
        close_candev(dev);
	/* and return OK */
        return 0;
}

/* the respective device operations */
static const struct net_device_ops mcp2515a_netdev_ops = {
        .ndo_open       = mcp2515a_open,
        .ndo_stop       = mcp2515a_stop,
        .ndo_start_xmit = mcp2515a_start_xmit,
};

/* the initialization and probing for this device */
static int mcp2515a_probe(struct spi_device *spi)
{
        struct net_device *net;
        struct mcp251x_platform_data *pdata = spi->dev.platform_data;
	struct mcp2515a_priv *priv;
        int ret;

	/* Platform data is required for osc freq */
	if (!pdata)
		return -ENODEV;

	/* check for device to be present */
	ret=mcp2515a_confirm_device(spi);
	if (ret)
		return ret;

	/* allocate the CAN structure */
	net = alloc_candev(sizeof(struct mcp2515a_priv),1);
        if (!net)
		return -ENOMEM;

	/* copy the driver data */
	dev_set_drvdata(&spi->dev, net);

	/* setting up as netdev */
	SET_NETDEV_DEV(net, &spi->dev);
	/*  link in the device ops */
	net->netdev_ops = &mcp2515a_netdev_ops;
	net->flags |= IFF_ECHO;

	/* get the private data and fill in details */
	priv = netdev_priv(net);

	/* fill in private data */
	priv->spi = spi;
	priv->net = net;
        priv->can.clock.freq = pdata->oscillator_frequency / 2;
	priv->can.bittiming_const = & mcp2515a_bittiming_const;
        priv->can.ctrlmode_supported =
		CAN_CTRLMODE_3_SAMPLES
                | CAN_CTRLMODE_LOOPBACK
		| CAN_CTRLMODE_LISTENONLY
		| CAN_CTRLMODE_ONE_SHOT ;
        priv->can.do_set_mode = mcp2515a_do_set_mode;

	/* initialize the "cancontroller" */
	init_completion(&priv->can_transmit);

	/* allocate some buffers from DMA space */
	ret=mcp2515a_init_transfers(net);
	if (ret)
		goto error_out;

	/* and register the device */
        ret = register_candev(net);
        if (ret)
		goto error_transfers;
	/* return without errors */
	return 0;

error_transfers:
	mcp2515a_free_transfers(net);
error_out:
	free_candev(net);
	return ret;
}

static int mcp2515a_remove(struct spi_device *spi)
{
        struct net_device *net = dev_get_drvdata(&spi->dev);
	/* unregister the device */
        unregister_candev(net);
	/* free the memory for generated SPI messages*/
	mcp2515a_free_transfers(net);
	/* clear driver data */
        dev_set_drvdata(&spi->dev, NULL);
	/* free the device finally */
        free_candev(net);
	/* and return OK */
        return 0;
}

/* for Device tree matching */
static const struct of_device_id mcp2515a_match[] = {
        { .compatible = "mcp2515", },
        {}
};
MODULE_DEVICE_TABLE(of, mcp2515a_match);

/* and "normal" aliases */
static const struct spi_device_id mcp2515a_id_table[] = {
        { "mcp2515",   2515 },
        { "mcp2515a",  2515 },
        { },
};

/* power management */
static int mcp2515a_can_suspend(struct device *dev)
{
	return 0;
}

static int mcp2515a_can_resume(struct device *dev)
{
	return 0;
}


static SIMPLE_DEV_PM_OPS(mcp2515a_powermanagement_ops,
			mcp2515a_can_suspend,
			mcp2515a_can_resume);

static struct spi_driver mcp2515a_spi_driver = {
        .driver = {
                .name = DRV_NAME,
                .owner = THIS_MODULE,
		.of_match_table = mcp2515a_match,
#if CONFIG_PM_SLEEP
		.pm = &mcp2515a_powermanagement_ops,
#endif
        },
        .probe    = mcp2515a_probe,
        .remove   = mcp2515a_remove,
	.id_table = mcp2515a_id_table,
};

static int mcp2515a_init(void)
{
        return spi_register_driver(&mcp2515a_spi_driver);
}
module_init(mcp2515a_init);

static void mcp2515a_exit(void)
{
        spi_unregister_driver(&mcp2515a_spi_driver);
}
module_exit(mcp2515a_exit);

MODULE_DESCRIPTION("Driver for Microchip MCP2515 SPI CAN controller"
		" using asyncronous SPI-calls");
MODULE_AUTHOR("Martin Sperl");
MODULE_LICENSE("GPL");
