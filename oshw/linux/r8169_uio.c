/* r8169_uio.c — Minimal userspace driver for RTL8168H
 *
 * Drives the NIC directly via UIO (mmap BAR + DMA descriptors).
 * No kernel network stack, no NAPI, no syscalls for TX/RX.
 * PHY is assumed pre-initialized by kernel driver before UIO bind.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <glob.h>

#include "r8169_uio.h"

/* Memory layout inside the single hugepage:
 *   0x000000  TX descriptor ring  (256 * 16 = 4KB)
 *   0x001000  RX descriptor ring  (256 * 16 = 4KB)
 *   0x002000  TX buffers          (256 * 2KB = 512KB)
 *   0x082000  RX buffers          (256 * 2KB = 512KB)
 *   Total: ~1MB, fits in one 2MB hugepage.
 */
#define TX_RING_OFFSET   0x000000
#define RX_RING_OFFSET   0x001000
#define TX_BUF_OFFSET    0x002000
#define RX_BUF_OFFSET    (TX_BUF_OFFSET + NUM_TX_DESC * BUF_SIZE)

/* ---------- UIO device discovery ---------- */

/* Find /dev/uioN for a given PCI BDF (e.g. "0000:03:00.0") */
static int find_uio_device(const char *pci_bdf, char *uio_path, size_t pathlen)
{
	char pattern[256];
	glob_t gl;

	snprintf(pattern, sizeof(pattern),
		 "/sys/bus/pci/devices/%s/uio/uio*", pci_bdf);

	if (glob(pattern, 0, NULL, &gl) != 0 || gl.gl_pathc == 0) {
		fprintf(stderr, "No UIO device found for %s\n"
			"  Run: scripts/bind_uio.sh\n", pci_bdf);
		globfree(&gl);
		return -1;
	}

	/* Extract uio number from path */
	const char *name = strrchr(gl.gl_pathv[0], '/');
	if (!name) {
		globfree(&gl);
		return -1;
	}
	snprintf(uio_path, pathlen, "/dev%s", name);
	globfree(&gl);
	return 0;
}

/* Find the MMIO BAR index by checking PCI resource sizes.
 * r8169 uses a 4KB or 256-byte MMIO BAR (not I/O ports). */
static int find_mmio_bar(const char *pci_bdf, int *bar_idx, size_t *bar_size)
{
	char path[256];
	FILE *fp;

	snprintf(path, sizeof(path),
		 "/sys/bus/pci/devices/%s/resource", pci_bdf);

	fp = fopen(path, "r");
	if (!fp) {
		perror("open PCI resource");
		return -1;
	}

	for (int i = 0; i < 6; i++) {
		unsigned long start, end, flags;
		if (fscanf(fp, "0x%lx 0x%lx 0x%lx\n",
			   &start, &end, &flags) != 3)
			break;

		/* Check for MMIO (not I/O port) and non-zero size */
		if (start && end && !(flags & 0x1)) {
			*bar_idx = i;
			*bar_size = end - start + 1;
			fclose(fp);
			printf("PCI BAR%d: phys=0x%lx size=%zu\n",
			       i, start, *bar_size);
			return 0;
		}
	}

	fclose(fp);
	fprintf(stderr, "No MMIO BAR found for %s\n", pci_bdf);
	return -1;
}

/* ---------- MDIO (PHY register access) ---------- */

static void phy_write(volatile void *mmio, int reg, uint16_t val)
{
	rtl_w32(mmio, REG_PHYAR,
		0x80000000 | ((reg & 0x1f) << 16) | (val & 0xffff));
	/* Wait for write to complete (bit 31 clears) */
	for (int i = 0; i < 2000; i++) {
		usleep(1);
		if (!(rtl_r32(mmio, REG_PHYAR) & 0x80000000))
			return;
	}
}

static uint16_t phy_read(volatile void *mmio, int reg)
{
	rtl_w32(mmio, REG_PHYAR, (reg & 0x1f) << 16);
	/* Wait for read to complete (bit 31 sets) */
	for (int i = 0; i < 2000; i++) {
		usleep(1);
		uint32_t v = rtl_r32(mmio, REG_PHYAR);
		if (v & 0x80000000)
			return (uint16_t)(v & 0xffff);
	}
	return 0xffff;
}

/* ---------- NIC initialization ---------- */

static void nic_reset(struct r8169_uio *dev)
{
	/* Soft reset — resets MAC, preserves PHY state */
	rtl_w8(dev->mmio, REG_ChipCmd, CmdReset);

	/* Wait up to 100ms for reset to complete */
	for (int i = 0; i < 1000; i++) {
		if (!(rtl_r8(dev->mmio, REG_ChipCmd) & CmdReset))
			return;
		usleep(100);
	}
	fprintf(stderr, "WARNING: chip reset timeout\n");
}

static void nic_phy_power_up(struct r8169_uio *dev)
{
	volatile void *mmio = dev->mmio;

	/* Power up PHY PLL (PMCH register, bits 7:6) */
	rtl_w8(mmio, REG_PMCH, rtl_r8(mmio, REG_PMCH) | 0xC0);

	/* Select PHY page 0 and enable auto-negotiation */
	phy_write(mmio, 0x1f, 0x0000);  /* page 0 */
	phy_write(mmio, 0x00, 0x1000);  /* BMCR: auto-neg enable */

	/* Wait for link (up to 5 seconds) */
	printf("Waiting for PHY link...");
	fflush(stdout);
	for (int i = 0; i < 50; i++) {
		usleep(100000);  /* 100ms */
		uint16_t bmsr = phy_read(mmio, 0x01);  /* BMSR */
		if (bmsr & 0x0004) {  /* link status bit */
			printf(" link up\n");
			return;
		}
	}
	printf(" timeout (no link)\n");
}

static void nic_init_rings(struct r8169_uio *dev)
{
	/* Init TX descriptors — all owned by host */
	for (int i = 0; i < NUM_TX_DESC; i++) {
		dev->tx_ring[i].opts1 = 0;
		dev->tx_ring[i].opts2 = 0;
		dev->tx_ring[i].addr = htole64(
			dev->tx_buf_phys + (uint64_t)i * BUF_SIZE);
	}
	/* Mark last TX descriptor */
	dev->tx_ring[NUM_TX_DESC - 1].opts1 = htole32(RingEnd);

	/* Init RX descriptors — all owned by NIC */
	for (int i = 0; i < NUM_RX_DESC; i++) {
		uint32_t flags = DescOwn | BUF_SIZE;
		if (i == NUM_RX_DESC - 1)
			flags |= RingEnd;
		dev->rx_ring[i].opts1 = htole32(flags);
		dev->rx_ring[i].opts2 = 0;
		dev->rx_ring[i].addr = htole64(
			dev->rx_buf_phys + (uint64_t)i * BUF_SIZE);
	}

	dev->cur_tx = 0;
	dev->dirty_tx = 0;
	dev->cur_rx = 0;
}

static void nic_hw_start(struct r8169_uio *dev)
{
	volatile void *mmio = dev->mmio;

	/* === Phase 1: config-locked registers === */
	rtl_w8(mmio, REG_Cfg9346, Cfg9346_Unlock);

	/* Disable all interrupts */
	rtl_w16(mmio, REG_IntrMask, 0);
	rtl_w16(mmio, REG_IntrStatus, 0xFFFF);  /* ACK any pending */

	/* Disable interrupt coalescing */
	rtl_w16(mmio, REG_IntrMitigate, 0);

	/* Max TX packet size for 8168evl (EarlySize) */
	rtl_w8(mmio, REG_MaxTxPacketSize, EarlySize);

	/* Disable RXDV gate (8168H blocks RX after reset until cleared) */
	rtl_w32(mmio, REG_MISC,
		rtl_r32(mmio, REG_MISC) & ~RXDV_GATED_EN);

	/* Set max RX packet size */
	rtl_w16(mmio, REG_RxMaxSize, BUF_SIZE);

	/* Set descriptor ring addresses (write High BEFORE Low) */
	rtl_w32(mmio, REG_TxDescStartAddrHigh,
		(uint32_t)(dev->tx_ring_phys >> 32));
	rtl_w32(mmio, REG_TxDescStartAddrLow,
		(uint32_t)(dev->tx_ring_phys & 0xFFFFFFFF));
	rtl_w32(mmio, REG_RxDescAddrHigh,
		(uint32_t)(dev->rx_ring_phys >> 32));
	rtl_w32(mmio, REG_RxDescAddrLow,
		(uint32_t)(dev->rx_ring_phys & 0xFFFFFFFF));

	rtl_w8(mmio, REG_Cfg9346, Cfg9346_Lock);

	/* PCI commit — ensure all config writes reach HW */
	rtl_r8(mmio, REG_Cfg9346);

	/* === Phase 2: enable TX/RX, then set DMA config === */
	rtl_w8(mmio, REG_ChipCmd, CmdTxEnb | CmdRxEnb);

	/* RxConfig: 8168H base (no RX_FIFO_THRESH — that's for 8169) */
	rtl_w32(mmio, REG_RxConfig,
		RX128_INT_EN | RX_MULTI_EN | RX_DMA_BURST | RX_EARLY_OFF |
		AcceptBroadcast | AcceptMyPhys);

	/* TxConfig: DMA burst + IFG + auto FIFO (8168evl) */
	rtl_w32(mmio, REG_TxConfig,
		TX_DMA_BURST | TX_IFG | TXCFG_AUTO_FIFO);
}

static void nic_read_mac(struct r8169_uio *dev)
{
	for (int i = 0; i < 6; i++)
		dev->mac[i] = rtl_r8(dev->mmio, REG_MAC0 + i);

	printf("MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
	       dev->mac[0], dev->mac[1], dev->mac[2],
	       dev->mac[3], dev->mac[4], dev->mac[5]);
}

/* Enable PCI bus mastering (required for DMA).
 * Kernel driver does pci_set_master(); uio_pci_generic does not. */
static int pci_enable_bus_master(const char *pci_bdf)
{
	char path[256];
	int fd;
	uint16_t cmd;

	snprintf(path, sizeof(path),
		 "/sys/bus/pci/devices/%s/config", pci_bdf);
	fd = open(path, O_RDWR);
	if (fd < 0) {
		perror("open PCI config");
		return -1;
	}

	/* PCI Command Register at offset 0x04 */
	if (pread(fd, &cmd, 2, 0x04) != 2) {
		perror("read PCI command");
		close(fd);
		return -1;
	}

	printf("PCI Command: 0x%04x (BusMaster=%d)\n",
	       cmd, !!(cmd & 0x04));

	if (!(cmd & 0x04)) {
		cmd |= 0x04;  /* Set Bus Master Enable (bit 2) */
		if (pwrite(fd, &cmd, 2, 0x04) != 2) {
			perror("write PCI command");
			close(fd);
			return -1;
		}
		printf("PCI Bus Master enabled\n");
	}

	close(fd);
	return 0;
}

/* ---------- Public API ---------- */

int r8169_uio_init(struct r8169_uio *dev, const char *pci_bdf)
{
	char uio_path[64];
	int bar_idx;
	size_t bar_size;

	memset(dev, 0, sizeof(*dev));
	dev->uio_fd = -1;

	/* 1. Find UIO device */
	if (find_uio_device(pci_bdf, uio_path, sizeof(uio_path)) < 0)
		return -1;
	printf("UIO device: %s\n", uio_path);

	/* 2. Find MMIO BAR */
	if (find_mmio_bar(pci_bdf, &bar_idx, &bar_size) < 0)
		return -1;

	/* 3. Open UIO device (for interrupt control, optional) */
	dev->uio_fd = open(uio_path, O_RDWR);
	if (dev->uio_fd < 0) {
		perror("open UIO device");
		return -1;
	}

	/* 3b. mmap BAR via PCI sysfs resource file.
	 * uio_pci_generic doesn't expose BAR maps — use resourceN directly. */
	{
		char res_path[256];
		int res_fd;

		snprintf(res_path, sizeof(res_path),
			 "/sys/bus/pci/devices/%s/resource%d", pci_bdf, bar_idx);
		res_fd = open(res_path, O_RDWR | O_SYNC);
		if (res_fd < 0) {
			perror("open PCI resource");
			close(dev->uio_fd);
			return -1;
		}
		dev->mmio = mmap(NULL, bar_size,
				 PROT_READ | PROT_WRITE,
				 MAP_SHARED, res_fd, 0);
		close(res_fd);  /* fd can be closed after mmap */
		if (dev->mmio == MAP_FAILED) {
			perror("mmap BAR");
			close(dev->uio_fd);
			return -1;
		}
	}
	dev->mmio_size = bar_size;
	printf("MMIO mapped: BAR%d size=%zu\n", bar_idx, bar_size);

	/* 4. Allocate DMA memory (single hugepage) */
	if (dma_alloc_hugepage(&dev->dma, 0) < 0) {
		munmap((void *)dev->mmio, bar_size);
		close(dev->uio_fd);
		return -1;
	}

	/* 5. Set up pointers into hugepage */
	uint8_t *base = dev->dma.virt;
	uint64_t base_phys = dev->dma.phys;

	dev->tx_ring = (struct r8169_desc *)(base + TX_RING_OFFSET);
	dev->rx_ring = (struct r8169_desc *)(base + RX_RING_OFFSET);
	dev->tx_buf  = base + TX_BUF_OFFSET;
	dev->rx_buf  = base + RX_BUF_OFFSET;

	dev->tx_ring_phys = base_phys + TX_RING_OFFSET;
	dev->rx_ring_phys = base_phys + RX_RING_OFFSET;
	dev->tx_buf_phys  = base_phys + TX_BUF_OFFSET;
	dev->rx_buf_phys  = base_phys + RX_BUF_OFFSET;

	/* 6. Enable PCI bus mastering for DMA */
	if (pci_enable_bus_master(pci_bdf) < 0) {
		dma_free(&dev->dma);
		munmap((void *)dev->mmio, bar_size);
		close(dev->uio_fd);
		return -1;
	}

	/* 7. Reset and initialize NIC */
	nic_reset(dev);
	nic_phy_power_up(dev);
	nic_read_mac(dev);
	nic_init_rings(dev);
	nic_hw_start(dev);

	printf("r8169_uio: initialized on %s\n", pci_bdf);
	return 0;
}

void r8169_uio_close(struct r8169_uio *dev)
{
	if (dev->mmio) {
		/* Disable TX/RX */
		rtl_w8(dev->mmio, REG_ChipCmd, 0);
		rtl_w16(dev->mmio, REG_IntrMask, 0);
		munmap((void *)dev->mmio, dev->mmio_size);
		dev->mmio = NULL;
	}

	dma_free(&dev->dma);

	if (dev->uio_fd >= 0) {
		close(dev->uio_fd);
		dev->uio_fd = -1;
	}
}

int r8169_tx(struct r8169_uio *dev, const void *pkt, uint16_t len)
{
	uint32_t entry = dev->cur_tx % NUM_TX_DESC;
	struct r8169_desc *txd = &dev->tx_ring[entry];

	/* Check if descriptor is free */
	if (le32toh(txd->opts1) & DescOwn)
		return -1;  /* ring full */

	/* Copy packet to TX buffer */
	uint8_t *txbuf = dev->tx_buf + (size_t)entry * BUF_SIZE;
	memcpy(txbuf, pkt, len);

	/* Pad to minimum ethernet frame size (zero padding) */
	if (len < 60) {
		memset(txbuf + len, 0, 60 - len);
		len = 60;
	}

	/* Set descriptor — must set opts1 (with DescOwn) LAST */
	txd->opts2 = 0;
	txd->addr = htole64(dev->tx_buf_phys + (uint64_t)entry * BUF_SIZE);

	uint32_t opts1 = DescOwn | FirstFrag | LastFrag | len;
	if (entry == NUM_TX_DESC - 1)
		opts1 |= RingEnd;

	__atomic_thread_fence(__ATOMIC_RELEASE);
	txd->opts1 = htole32(opts1);

	/* Kick TX */
	rtl_w8(dev->mmio, REG_TxPoll, NPQ);

	dev->cur_tx++;
	return len;
}

void r8169_tx_complete(struct r8169_uio *dev)
{
	while (dev->dirty_tx != dev->cur_tx) {
		uint32_t entry = dev->dirty_tx % NUM_TX_DESC;
		uint32_t status = le32toh(dev->tx_ring[entry].opts1);

		if (status & DescOwn)
			break;  /* NIC still owns */

		dev->dirty_tx++;
	}
}

int r8169_rx(struct r8169_uio *dev, void *buf, uint16_t max_len)
{
	uint32_t entry = dev->cur_rx % NUM_RX_DESC;
	struct r8169_desc *rxd = &dev->rx_ring[entry];

	uint32_t status = le32toh(
		__atomic_load_n(&rxd->opts1, __ATOMIC_ACQUIRE));

	if (status & DescOwn)
		return 0;  /* NIC still owns — no packet */

	uint16_t pkt_len = status & DESC_SIZE_MASK;
	/* Subtract CRC (4 bytes) if present */
	if (pkt_len > 4)
		pkt_len -= 4;

	if (pkt_len > max_len)
		pkt_len = max_len;

	memcpy(buf, dev->rx_buf + (size_t)entry * BUF_SIZE, pkt_len);

	/* Recycle descriptor — give back to NIC */
	uint32_t new_opts1 = DescOwn | BUF_SIZE;
	if (entry == NUM_RX_DESC - 1)
		new_opts1 |= RingEnd;

	rxd->opts2 = 0;
	__atomic_thread_fence(__ATOMIC_RELEASE);
	rxd->opts1 = htole32(new_opts1);

	dev->cur_rx++;
	return pkt_len;
}
