/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * r8169_uio.h — Minimal userspace driver for RTL8168H (r8169)
 *
 * Register definitions extracted from linux/drivers/net/ethernet/realtek/r8169.
 * Only the subset needed for EtherCAT send/recv is included.
 */

#ifndef R8169_UIO_H
#define R8169_UIO_H

#include <stdint.h>

/* ---------- PCI / UIO ---------- */

#define RTL8168_PCI_BDF     "0000:01:00.0"
#define RTL8168_VENDOR_ID   "10ec"
#define RTL8168_DEVICE_ID   "8168"

/* ---------- Register offsets (MMIO BAR, 256 bytes) ---------- */

#define REG_MAC0                0x00  /* MAC address bytes 0-5 */
#define REG_TxDescStartAddrLow  0x20
#define REG_TxDescStartAddrHigh 0x24
#define REG_ChipCmd             0x37
#define REG_TxPoll              0x38
#define REG_IntrMask            0x3c  /* 16-bit */
#define REG_IntrStatus          0x3e  /* 16-bit */
#define REG_TxConfig            0x40
#define REG_RxConfig            0x44
#define REG_Cfg9346             0x50
#define REG_Config1             0x52
#define REG_Config2             0x53
#define REG_Config5             0x56
#define REG_PHYAR               0x60  /* PHY Access Register (MDIO) */
#define REG_PMCH                0x6f  /* PLL power control */
#define REG_MISC                0xf0
#define RXDV_GATED_EN           (1 << 19)
#define REG_RxMaxSize           0xda
#define REG_CPlusCmd            0xe0
#define REG_IntrMitigate        0xe2
#define REG_RxDescAddrLow       0xe4
#define REG_RxDescAddrHigh      0xe8
#define REG_MaxTxPacketSize     0xec

/* ChipCmd bits */
#define CmdReset    0x10
#define CmdRxEnb    0x08
#define CmdTxEnb    0x04

/* TxPoll bits */
#define NPQ         0x40  /* Normal Priority Queue poll */

/* Cfg9346 bits */
#define Cfg9346_Lock   0x00
#define Cfg9346_Unlock 0xC0

/* TxConfig bits (8168H) */
#define TX_DMA_BURST     (7 << 8)   /* max PCI burst */
#define TX_IFG           (3 << 24)  /* shortest InterFrameGap */
#define TXCFG_AUTO_FIFO  (1 << 7)   /* 8168evl auto FIFO */

/* RxConfig bits (8168H) */
#define AcceptBroadcast  0x08
#define AcceptMulticast  0x04
#define AcceptMyPhys     0x02
#define AcceptAllPhys    0x01
#define RX128_INT_EN     (1 << 15)
#define RX_MULTI_EN      (1 << 14)
#define RX_EARLY_OFF     (1 << 11)
#define RX_DMA_BURST     (7 << 8)

/* MaxTxPacketSize for 8168evl (units of 128 bytes) */
#define EarlySize        0x27

/* IntrMask / IntrStatus bits */
#define RxOK        0x0001
#define RxErr       0x0002
#define TxOK        0x0004
#define TxErr       0x0008
#define LinkChg     0x0020

/* ---------- Descriptor ---------- */

struct r8169_desc {
	uint32_t opts1;
	uint32_t opts2;
	uint64_t addr;    /* DMA (physical) address */
} __attribute__((packed, aligned(16)));

#define DescOwn    (1u << 31)
#define RingEnd    (1u << 30)
#define FirstFrag  (1u << 29)
#define LastFrag   (1u << 28)

/* opts1 bits 13:0 = packet size */
#define DESC_SIZE_MASK  0x3FFFu

/* ---------- Ring configuration ---------- */

#define NUM_TX_DESC  256
#define NUM_RX_DESC  256
#define BUF_SIZE     2048   /* per-descriptor buffer size */

/* ---------- MMIO access (volatile, no kernel helpers) ---------- */

static inline void rtl_w8(volatile void *base, unsigned reg, uint8_t val)
{
	*(volatile uint8_t *)((uintptr_t)base + reg) = val;
}
static inline void rtl_w16(volatile void *base, unsigned reg, uint16_t val)
{
	*(volatile uint16_t *)((uintptr_t)base + reg) = val;
}
static inline void rtl_w32(volatile void *base, unsigned reg, uint32_t val)
{
	*(volatile uint32_t *)((uintptr_t)base + reg) = val;
}
static inline uint8_t rtl_r8(volatile void *base, unsigned reg)
{
	return *(volatile uint8_t *)((uintptr_t)base + reg);
}
static inline uint16_t rtl_r16(volatile void *base, unsigned reg)
{
	return *(volatile uint16_t *)((uintptr_t)base + reg);
}
static inline uint32_t rtl_r32(volatile void *base, unsigned reg)
{
	return *(volatile uint32_t *)((uintptr_t)base + reg);
}

/* ---------- DMA memory ---------- */

struct r8169_dma_mem {
	void     *virt;       /* userspace virtual address */
	uint64_t  phys;       /* physical address (= DMA addr with iommu=pt) */
	size_t    size;        /* allocation size */
};

int  dma_alloc_hugepage(struct r8169_dma_mem *mem, size_t size);
void dma_free(struct r8169_dma_mem *mem);

/* ---------- Device context ---------- */

struct r8169_uio {
	volatile void       *mmio;          /* BAR mmap */
	int                  uio_fd;        /* /dev/uioN fd */

	struct r8169_dma_mem dma;           /* single hugepage for everything */

	/* descriptor rings (inside dma) */
	struct r8169_desc   *tx_ring;       /* dma.virt + 0 */
	struct r8169_desc   *rx_ring;       /* dma.virt + 4K */

	/* packet buffers (inside dma) */
	uint8_t             *tx_buf;        /* dma.virt + 8K */
	uint8_t             *rx_buf;        /* dma.virt + 8K + 256*BUF_SIZE */

	/* DMA physical addresses */
	uint64_t             tx_ring_phys;
	uint64_t             rx_ring_phys;
	uint64_t             tx_buf_phys;
	uint64_t             rx_buf_phys;

	/* ring indices */
	uint32_t             cur_tx;
	uint32_t             dirty_tx;
	uint32_t             cur_rx;

	/* MMIO unmap size */
	size_t               mmio_size;

	/* MAC address */
	uint8_t              mac[6];
};

/* ---------- API ---------- */

int  r8169_uio_init(struct r8169_uio *dev, const char *pci_bdf);
void r8169_uio_close(struct r8169_uio *dev);

int  r8169_tx(struct r8169_uio *dev, const void *pkt, uint16_t len);
int  r8169_rx(struct r8169_uio *dev, void *buf, uint16_t max_len);
void r8169_tx_complete(struct r8169_uio *dev);

#endif /* R8169_UIO_H */
