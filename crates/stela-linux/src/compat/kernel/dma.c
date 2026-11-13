// SPDX-License-Identifier: GPL-2.0-only
/* Parts are adapted from the Linux files named below, whose notices are:
 * Copyright (c) 2006  SUSE Linux Products GmbH
 * Copyright (c) 2006  Tejun Heo <teheo@suse.de>
 */
/* Linux's DMA mapping API, following Linux
 * cc9de5d4b5ab6459f5774946aa3195ad51236fa8 kernel/dma/mapping.c. Every device
 * maps through the dma_map_ops its owner attached (set_dma_ops); there is no
 * direct path, since only the owner knows its requester's DMA domain. */
#include <linux/bug.h>
#include <linux/dma-map-ops.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <stela/catalogue.h>

dma_addr_t dma_map_phys(struct device *dev, phys_addr_t phys, size_t size,
			enum dma_data_direction dir, unsigned long attrs)
{
	if (stela_linux_kernel_hosted())
		return DMA_MAPPING_ERROR;
	BUG_ON(!valid_dma_direction(dir));
	return get_dma_ops(dev)->map_phys(dev, phys, size, dir, attrs);
}

void dma_unmap_phys(struct device *dev, dma_addr_t addr, size_t size,
		    enum dma_data_direction dir, unsigned long attrs)
{
	BUG_ON(!valid_dma_direction(dir));
	get_dma_ops(dev)->unmap_phys(dev, addr, size, dir, attrs);
}

dma_addr_t dma_map_page_attrs(struct device *dev, struct page *page, size_t offset,
			      size_t size, enum dma_data_direction dir, unsigned long attrs)
{
	if (stela_linux_kernel_hosted())
		return DMA_MAPPING_ERROR;
	return dma_map_phys(dev, page_to_phys(page) + offset, size, dir, attrs);
}

void dma_unmap_page_attrs(struct device *dev, dma_addr_t addr, size_t size,
			  enum dma_data_direction dir, unsigned long attrs)
{
	dma_unmap_phys(dev, addr, size, dir, attrs);
}

static int map_sg(struct device *dev, struct scatterlist *sg, int nents,
		  enum dma_data_direction dir, unsigned long attrs)
{
	if (stela_linux_kernel_hosted())
		return -EOPNOTSUPP;
	BUG_ON(!valid_dma_direction(dir));
	int ents = get_dma_ops(dev)->map_sg(dev, sg, nents, dir, attrs);
	return ents ? ents : -EIO;
}

unsigned int dma_map_sg_attrs(struct device *dev, struct scatterlist *sg, int nents,
			      enum dma_data_direction dir, unsigned long attrs)
{
	int ents = map_sg(dev, sg, nents, dir, attrs);
	return ents < 0 ? 0 : (unsigned int)ents;
}

int dma_map_sgtable(struct device *dev, struct sg_table *sgt,
		    enum dma_data_direction dir, unsigned long attrs)
{
	int nents = map_sg(dev, sgt->sgl, (int)sgt->orig_nents, dir, attrs);
	if (nents < 0)
		return nents;
	sgt->nents = (unsigned int)nents;
	return 0;
}

void dma_unmap_sg_attrs(struct device *dev, struct scatterlist *sg, int nents,
			enum dma_data_direction dir, unsigned long attrs)
{
	BUG_ON(!valid_dma_direction(dir));
	get_dma_ops(dev)->unmap_sg(dev, sg, nents, dir, attrs);
}

void *dma_alloc_attrs(struct device *dev, size_t size, dma_addr_t *dma_handle, gfp_t flag,
		      unsigned long attrs)
{
	if (stela_linux_kernel_hosted())
		return NULL;
	/* The zone modifiers mean nothing here: the mask decides placement. */
	flag &= ~(__GFP_DMA | __GFP_DMA32 | __GFP_HIGHMEM);
	return get_dma_ops(dev)->alloc(dev, size, dma_handle, flag, attrs);
}

void dma_free_attrs(struct device *dev, size_t size, void *cpu_addr, dma_addr_t dma_handle,
		    unsigned long attrs)
{
	WARN_ON(irqs_disabled());
	if (cpu_addr)
		get_dma_ops(dev)->free(dev, size, cpu_addr, dma_handle, attrs);
}

/* Linux __dma_alloc_pages: a bus whose operations allocate no pages gives
 * none, and the caller falls back or fails as it would there. */
struct page *dma_alloc_pages(struct device *dev, size_t size, dma_addr_t *dma_handle,
			     enum dma_data_direction dir, gfp_t gfp)
{
	const struct dma_map_ops *ops = get_dma_ops(dev);

	if (stela_linux_kernel_hosted() || !ops->alloc_pages_op)
		return NULL;
	gfp &= ~(__GFP_DMA | __GFP_DMA32 | __GFP_HIGHMEM);
	return ops->alloc_pages_op(dev, PAGE_ALIGN(size), dma_handle, dir, gfp);
}

/* Only pages dma_alloc_pages returned come back here, and it returns none
 * without the operation. */
void dma_free_pages(struct device *dev, size_t size, struct page *page, dma_addr_t dma_handle,
		    enum dma_data_direction dir)
{
	const struct dma_map_ops *ops = get_dma_ops(dev);

	if (page && ops->free_pages)
		ops->free_pages(dev, PAGE_ALIGN(size), page, dma_handle, dir);
}

static bool supported(struct device *dev, u64 mask)
{
	const struct dma_map_ops *ops = get_dma_ops(dev);
	return !ops->dma_supported || ops->dma_supported(dev, mask);
}

size_t dma_max_mapping_size(struct device *dev)
{
	const struct dma_map_ops *ops = get_dma_ops(dev);

	return ops && ops->max_mapping_size ? ops->max_mapping_size(dev) : SIZE_MAX;
}

int dma_set_mask(struct device *dev, u64 mask)
{
	if (!dev->dma_mask || !supported(dev, mask))
		return -EIO;
	*dev->dma_mask = mask;
	return 0;
}

int dma_set_coherent_mask(struct device *dev, u64 mask)
{
	if (!supported(dev, mask))
		return -EIO;
	dev->coherent_dma_mask = mask;
	return 0;
}

/* The mask a device needs to reach all memory, following kernel/dma/mapping.c
 * for a device that maps through its owner's operations, never directly and
 * never through dma-iommu: the operations' answer or, as Linux defaults for
 * operations without one, 32 bits. */
u64 dma_get_required_mask(struct device *dev)
{
	const struct dma_map_ops *ops = get_dma_ops(dev);

	if (ops->get_required_mask)
		return ops->get_required_mask(dev);
	return DMA_BIT_MASK(32);
}

/* Whether the device cannot reach all memory: with operations attached, only
 * a mask or bus limit below the required mask limits it. */
bool dma_addressing_limited(struct device *dev)
{
	if (min_not_zero(dma_get_mask(dev), dev->bus_dma_limit) >= dma_get_required_mask(dev))
		return false;
	dev_dbg(dev, "device is DMA addressing limited\n");
	return true;
}

/* A device's memory-mapped resource, which maps as MMIO. */
dma_addr_t dma_map_resource(struct device *dev, phys_addr_t phys_addr, size_t size,
			    enum dma_data_direction dir, unsigned long attrs)
{
	return dma_map_phys(dev, phys_addr, size, dir, attrs | DMA_ATTR_MMIO);
}

void dma_unmap_resource(struct device *dev, dma_addr_t addr, size_t size,
			enum dma_data_direction dir, unsigned long attrs)
{
	dma_unmap_phys(dev, addr, size, dir, attrs | DMA_ATTR_MMIO);
}

#ifdef STELA_LINUX_DMA_TEST
#include <stela/linux.h>
#include <stela/test.h>

#define BUS 0x4000000000ULL

static unsigned int mapped, freed;
static gfp_t allocated_gfp;
static unsigned long last_attrs;

static dma_addr_t map_phys(struct device *dev, phys_addr_t phys, size_t size,
			   enum dma_data_direction dir, unsigned long attrs)
{
	(void)dev;
	assert(size == 0x20 && dir == DMA_TO_DEVICE);
	last_attrs = attrs;
	mapped++;
	return BUS + phys;
}

static void unmap_phys(struct device *dev, dma_addr_t handle, size_t size,
		       enum dma_data_direction dir, unsigned long attrs)
{
	(void)dev;
	(void)size;
	(void)dir;
	assert(handle >= BUS);
	last_attrs = attrs;
	mapped--;
}

static int map_scatter(struct device *dev, struct scatterlist *sg, int nents,
		       enum dma_data_direction dir, unsigned long attrs)
{
	(void)dev;
	(void)sg;
	(void)dir;
	(void)attrs;
	return nents == 3 ? 2 : 0;
}

static void *alloc(struct device *dev, size_t size, dma_addr_t *handle, gfp_t gfp,
		   unsigned long attrs)
{
	static u8 memory[16];

	(void)dev;
	(void)attrs;
	assert(size == sizeof(memory));
	allocated_gfp = gfp;
	*handle = BUS;
	return memory;
}

static void free_memory(struct device *dev, size_t size, void *cpu, dma_addr_t handle,
			unsigned long attrs)
{
	(void)dev;
	(void)size;
	(void)cpu;
	(void)attrs;
	assert(handle == BUS);
	freed++;
}

static int mask_supported(struct device *dev, u64 mask)
{
	(void)dev;
	return mask >= DMA_BIT_MASK(32);
}

static u64 required_mask(struct device *dev)
{
	(void)dev;
	return DMA_BIT_MASK(48);
}

static const struct dma_map_ops ops = {
	.map_phys = map_phys,
	.unmap_phys = unmap_phys,
	.map_sg = map_scatter,
	.alloc = alloc,
	.free = free_memory,
	.dma_supported = mask_supported,
};

int main(void)
{
	stela_linux_enter(&stela_linux_bare);
	u64 mask = 0;
	struct device dev = {.dma_mask = &mask};
	set_dma_ops(&dev, &ops);

	dma_addr_t handle = dma_map_phys(&dev, 0x1000, 0x20, DMA_TO_DEVICE, 0);
	assert(handle == BUS + 0x1000 && mapped == 1 && !dma_mapping_error(&dev, handle));
	dma_unmap_phys(&dev, handle, 0x20, DMA_TO_DEVICE, 0);
	assert(!mapped);

	struct sg_table table = {.orig_nents = 3};
	assert(!dma_map_sgtable(&dev, &table, DMA_BIDIRECTIONAL, 0) && table.nents == 2);
	table.orig_nents = 1;
	assert(dma_map_sgtable(&dev, &table, DMA_BIDIRECTIONAL, 0) == -EIO);
	assert(!dma_map_sg_attrs(&dev, table.sgl, 1, DMA_BIDIRECTIONAL, 0));

	void *cpu = dma_alloc_coherent(&dev, 16, &handle, GFP_KERNEL | __GFP_DMA32);
	assert(cpu && handle == BUS && !(allocated_gfp & __GFP_DMA32));
	dma_free_coherent(&dev, 16, cpu, handle);
	dma_free_coherent(&dev, 16, NULL, handle);
	assert(freed == 1);

	assert(dma_set_mask(&dev, DMA_BIT_MASK(31)) == -EIO && !mask);
	assert(!dma_set_mask_and_coherent(&dev, DMA_BIT_MASK(47)));
	assert(mask == DMA_BIT_MASK(47) && dev.coherent_dma_mask == mask);

	/* Operations without a required mask need 32 bits; a mask or bus limit
	 * below the required mask limits addressing. */
	assert(dma_get_required_mask(&dev) == DMA_BIT_MASK(32) && !dma_addressing_limited(&dev));
	dev.bus_dma_limit = DMA_BIT_MASK(31);
	assert(dma_addressing_limited(&dev));
	dev.bus_dma_limit = 0;
	struct dma_map_ops wide = ops;

	wide.get_required_mask = required_mask;
	set_dma_ops(&dev, &wide);
	assert(dma_get_required_mask(&dev) == DMA_BIT_MASK(48) && dma_addressing_limited(&dev));
	set_dma_ops(&dev, &ops);

	/* A resource maps and unmaps as MMIO. */
	handle = dma_map_resource(&dev, 0x3000, 0x20, DMA_TO_DEVICE, 0);
	assert(handle == BUS + 0x3000 && mapped == 1 && (last_attrs & DMA_ATTR_MMIO));
	last_attrs = 0;
	dma_unmap_resource(&dev, handle, 0x20, DMA_TO_DEVICE, 0);
	assert(!mapped && (last_attrs & DMA_ATTR_MMIO));
	stela_linux_exit();
	printf("Linux DMA mapping, addressing limits and resources through the owner's dma_map_ops passed\n");
	return 0;
}
#endif
