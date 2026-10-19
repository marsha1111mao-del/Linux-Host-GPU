#ifndef __ARM64_KVM_NVHE_OPENCCA_MMIO_H__
#define __ARM64_KVM_NVHE_OPENCCA_MMIO_H__

#include <linux/types.h>

#define OPENCCA_GPU_MMIO_START 0xfb000000ULL
#define OPENCCA_GPU_MMIO_END 0xfb200000ULL

static inline bool opencca_host_mmio_range_allowed(u64 start, u64 end)
{
	return start < end &&
	       (end <= OPENCCA_GPU_MMIO_START || start >= OPENCCA_GPU_MMIO_END);
}

static inline bool opencca_host_mmio_clip_range(u64 address, u64 *start,
					       u64 *end)
{
	if (address < *start || address >= *end)
		return false;
	if (address < OPENCCA_GPU_MMIO_START) {
		if (*end > OPENCCA_GPU_MMIO_START)
			*end = OPENCCA_GPU_MMIO_START;
	} else if (address >= OPENCCA_GPU_MMIO_END) {
		if (*start < OPENCCA_GPU_MMIO_END)
			*start = OPENCCA_GPU_MMIO_END;
	} else {
		return false;
	}
	return true;
}

#endif
