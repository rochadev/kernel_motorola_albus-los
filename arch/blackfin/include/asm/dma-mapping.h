/*
 * Copyright 2004-2009 Analog Devices Inc.
 *
 * Licensed under the GPL-2 or later.
 */

#ifndef _BLACKFIN_DMA_MAPPING_H
#define _BLACKFIN_DMA_MAPPING_H

#include <asm/cacheflush.h>
struct scatterlist;

void *dma_alloc_coherent(struct device *dev, size_t size,
			 dma_addr_t *dma_handle, gfp_t gfp);
void dma_free_coherent(struct device *dev, size_t size, void *vaddr,
		       dma_addr_t dma_handle);

/*
 * Now for the API extensions over the pci_ one
 */
#define dma_alloc_noncoherent(d, s, h, f) dma_alloc_coherent(d, s, h, f)
#define dma_free_noncoherent(d, s, v, h) dma_free_coherent(d, s, v, h)
#define dma_supported(d, m)         (1)
#define dma_get_cache_alignment()   (32)
#define dma_is_consistent(d, h)     (1)

static inline int
dma_set_mask(struct device *dev, u64 dma_mask)
{
	if (!dev->dma_mask || !dma_supported(dev, dma_mask))
		return -EIO;

	*dev->dma_mask = dma_mask;

	return 0;
}

static inline int
dma_mapping_error(struct device *dev, dma_addr_t dma_addr)
{
	return 0;
}

extern void
__dma_sync(dma_addr_t addr, size_t size, enum dma_data_direction dir);
static inline void
_dma_sync(dma_addr_t addr, size_t size, enum dma_data_direction dir)
{
	if (!__builtin_constant_p(dir)) {
		__dma_sync(addr, size, dir);
		return;
	}

	switch (dir) {
	case DMA_NONE:
		BUG();
	case DMA_TO_DEVICE:		/* writeback only */
		flush_dcache_range(addr, addr + size);
		break;
	case DMA_FROM_DEVICE: /* invalidate only */
	case DMA_BIDIRECTIONAL: /* flush and invalidate */
		/* Blackfin has no dedicated invalidate (it includes a flush) */
		invalidate_dcache_range(addr, addr + size);
		break;
	}
}

/*
 * Map a single buffer of the indicated size for DMA in streaming mode.
 * The 32-bit bus address to use is returned.
 *
 * Once the device is given the dma address, the device owns this memory
 * until either pci_unmap_single or pci_dma_sync_single is performed.
 */
static inline dma_addr_t
dma_map_single(struct device *dev, void *ptr, size_t size,
	       enum dma_data_direction dir)
{
	_dma_sync((dma_addr_t)ptr, size, dir);
	return (dma_addr_t) ptr;
}

static inline dma_addr_t
dma_map_page(struct device *dev, struct page *page,
	     unsigned long offset, size_t size,
	     enum dma_data_direction dir)
{
	return dma_map_single(dev, page_address(page) + offset, size, dir);
}

/*
 * Unmap a single streaming mode DMA translation.  The dma_addr and size
 * must match what was provided for in a previous pci_map_single call.  All
 * other usages are undefined.
 *
 * After this call, reads by the cpu to the buffer are guarenteed to see
 * whatever the device wrote there.
 */
static inline void
dma_unmap_single(struct device *dev, dma_addr_t dma_addr, size_t size,
		 enum dma_data_direction dir)
{
	BUG_ON(!valid_dma_direction(dir));
}

static inline void
dma_unmap_page(struct device *dev, dma_addr_t dma_addr, size_t size,
	       enum dma_data_direction dir)
{
	dma_unmap_single(dev, dma_addr, size, dir);
}

/*
 * Map a set of buffers described by scatterlist in streaming
 * mode for DMA.  This is the scather-gather version of the
 * above pci_map_single interface.  Here the scatter gather list
 * elements are each tagged with the appropriate dma address
 * and length.  They are obtained via sg_dma_{address,length}(SG).
 *
 * NOTE: An implementation may be able to use a smaller number of
 *       DMA address/length pairs than there are SG table elements.
 *       (for example via virtual mapping capabilities)
 *       The routine returns the number of addr/length pairs actually
 *       used, at most nents.
 *
 * Device ownership issues as mentioned above for pci_map_single are
 * the same here.
 */
extern int dma_map_sg(struct device *dev, struct scatterlist *sg, int nents,
		      enum dma_data_direction dir);

/*
 * Unmap a set of streaming mode DMA translations.
 * Again, cpu read rules concerning calls here are the same as for
 * pci_unmap_single() above.
 */
static inline void
dma_unmap_sg(struct device *dev, struct scatterlist *sg,
	     int nhwentries, enum dma_data_direction dir)
{
	BUG_ON(!valid_dma_direction(dir));
}

static inline void
dma_sync_single_range_for_cpu(struct device *dev, dma_addr_t handle,
			      unsigned long offset, size_t size,
			      enum dma_data_direction dir)
{
	BUG_ON(!valid_dma_direction(dir));
}

static inline void
dma_sync_single_range_for_device(struct device *dev, dma_addr_t handle,
				 unsigned long offset, size_t size,
				 enum dma_data_direction dir)
{
	_dma_sync(handle + offset, size, dir);
}

static inline void
dma_sync_single_for_cpu(struct device *dev, dma_addr_t handle, size_t size,
			enum dma_data_direction dir)
{
	dma_sync_single_range_for_cpu(dev, handle, 0, size, dir);
}

static inline void
dma_sync_single_for_device(struct device *dev, dma_addr_t handle, size_t size,
			   enum dma_data_direction dir)
{
	dma_sync_single_range_for_device(dev, handle, 0, size, dir);
}

static inline void
dma_sync_sg_for_cpu(struct device *dev, struct scatterlist *sg, int nents,
		    enum dma_data_direction dir)
{
	BUG_ON(!valid_dma_direction(dir));
}

extern void
dma_sync_sg_for_device(struct device *dev, struct scatterlist *sg,
		       int nents, enum dma_data_direction dir);

static inline void
dma_cache_sync(struct device *dev, void *vaddr, size_t size,
	       enum dma_data_direction dir)
{
	_dma_sync((dma_addr_t)vaddr, size, dir);
}

#endif				/* _BLACKFIN_DMA_MAPPING_H */
