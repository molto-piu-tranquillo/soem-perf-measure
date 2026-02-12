/* r8169_dma.c — Hugepage-backed DMA memory for userspace NIC driver
 *
 * With iommu=pt (passthrough), physical address == DMA address.
 * Allocates a 2MB hugepage and looks up its physical address
 * via /proc/self/pagemap.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>

#include "r8169_uio.h"

#define HUGEPAGE_SIZE  (2 * 1024 * 1024)  /* 2MB */
#define PAGE_SIZE_4K   4096

static int virt_to_phys(void *virt, uint64_t *phys_out)
{
	int fd;
	uint64_t entry;
	off_t offset;

	fd = open("/proc/self/pagemap", O_RDONLY);
	if (fd < 0) {
		perror("open pagemap");
		return -1;
	}

	offset = ((uintptr_t)virt / PAGE_SIZE_4K) * sizeof(uint64_t);
	if (pread(fd, &entry, sizeof(entry), offset) != sizeof(entry)) {
		perror("read pagemap");
		close(fd);
		return -1;
	}
	close(fd);

	if (!(entry & (1ULL << 63))) {
		fprintf(stderr, "pagemap: page not present\n");
		return -1;
	}

	uint64_t pfn = entry & ((1ULL << 55) - 1);
	*phys_out = pfn * PAGE_SIZE_4K;
	return 0;
}

int dma_alloc_hugepage(struct r8169_dma_mem *mem, size_t size)
{
	void *ptr;

	if (size < HUGEPAGE_SIZE)
		size = HUGEPAGE_SIZE;

	ptr = mmap(NULL, size,
		   PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE,
		   -1, 0);
	if (ptr == MAP_FAILED) {
		fprintf(stderr, "mmap hugepage failed: %s\n"
			"  Run: echo 4 > /sys/kernel/mm/hugepages/"
			"hugepages-2048kB/nr_hugepages\n",
			strerror(errno));
		return -1;
	}

	if (mlock(ptr, size) < 0) {
		perror("mlock hugepage");
		munmap(ptr, size);
		return -1;
	}

	memset(ptr, 0, size);

	uint64_t phys;
	if (virt_to_phys(ptr, &phys) < 0) {
		munmap(ptr, size);
		return -1;
	}

	mem->virt = ptr;
	mem->phys = phys;
	mem->size = size;

	printf("DMA hugepage: virt=%p phys=0x%lx size=%zu\n",
	       ptr, (unsigned long)phys, size);
	return 0;
}

void dma_free(struct r8169_dma_mem *mem)
{
	if (mem->virt) {
		munmap(mem->virt, mem->size);
		mem->virt = NULL;
		mem->phys = 0;
		mem->size = 0;
	}
}
