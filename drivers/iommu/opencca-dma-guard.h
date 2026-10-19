/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __OPENCCA_DMA_GUARD_H
#define __OPENCCA_DMA_GUARD_H

#ifdef CONFIG_OPENCCA_DMA_GUARD
#include <linux/arm-smccc.h>
#include <linux/sizes.h>

#define OPENCCA_DMA_GUARD_SMC 0xc400016aUL
#define OPENCCA_MANAGED_IOMMU_SMC 0xc400016cUL

#define OPENCCA_IOMMU_VERSION 0UL
#define OPENCCA_IOMMU_CREATE 1UL
#define OPENCCA_IOMMU_MAP 2UL
#define OPENCCA_IOMMU_UNMAP 3UL
#define OPENCCA_IOMMU_QUERY 4UL
#define OPENCCA_IOMMU_BIND 5UL
#define OPENCCA_IOMMU_UNBIND 6UL
#define OPENCCA_IOMMU_DESTROY 8UL
#define OPENCCA_IOMMU_FAULT 11UL
#define OPENCCA_IOMMU_FAULT_BANK_BITS 16U
#define OPENCCA_IOMMU_FAULT_WRITE BIT(8)
#define OPENCCA_IOMMU_FAULT_GUEST BIT_ULL(32)

#ifdef CONFIG_OPENCCA_MANAGED_IOMMU
static inline int opencca_iommu_smc(unsigned long operation,
				    unsigned long handle, unsigned long arg1,
				    unsigned long arg2, unsigned long arg3,
				    struct arm_smccc_res *res)
{
	arm_smccc_1_1_smc(OPENCCA_MANAGED_IOMMU_SMC, operation, handle,
			  arg1, arg2, arg3, 0UL, 0UL, res);
	if (res->a0 == SMCCC_RET_NOT_SUPPORTED)
		return -ENODEV;
	return res->a0 ? -EPERM : 0;
}

static inline int opencca_iommu_create(unsigned long *handle)
{
	struct arm_smccc_res res;
	int ret = opencca_iommu_smc(OPENCCA_IOMMU_CREATE, 0UL, 0UL, 0UL, 0UL, &res);

	if (!ret)
		*handle = res.a1;
	return ret;
}

static inline int opencca_iommu_map(unsigned long handle, unsigned long iova,
				    phys_addr_t paddr, size_t size, int prot,
				    size_t *mapped)
{
	struct arm_smccc_res res;
	int ret = opencca_iommu_smc(OPENCCA_IOMMU_MAP, handle, iova,
				    (unsigned long)paddr,
				    size | (unsigned int)(prot & 3), &res);

	if (!ret && mapped) {
		*mapped = res.a1;
		if (*mapped != size)
			ret = -EIO;
	}
	return ret;
}

static inline size_t opencca_iommu_unmap(unsigned long handle,
					 unsigned long iova, size_t size)
{
	struct arm_smccc_res res;
	int ret = opencca_iommu_smc(OPENCCA_IOMMU_UNMAP, handle, iova, 0UL,
				    size, &res);

	return ret ? 0UL : (size_t)res.a1;
}

static inline phys_addr_t opencca_iommu_iova_to_phys(unsigned long handle,
						     unsigned long iova)
{
	struct arm_smccc_res res;
	int ret = opencca_iommu_smc(OPENCCA_IOMMU_QUERY, handle, iova, 0UL,
				    0UL, &res);

	return ret ? 0UL : (phys_addr_t)res.a1;
}

static inline int opencca_iommu_bind(unsigned long handle,
				     phys_addr_t device)
{
	struct arm_smccc_res res;

	return opencca_iommu_smc(OPENCCA_IOMMU_BIND, handle,
				(unsigned long)device, 0UL, 0UL, &res);
}

static inline int opencca_iommu_unbind(unsigned long handle,
				       phys_addr_t device)
{
	struct arm_smccc_res res;

	return opencca_iommu_smc(OPENCCA_IOMMU_UNBIND, handle,
				(unsigned long)device, 0UL, 0UL, &res);
}

static inline int opencca_iommu_destroy(unsigned long handle)
{
	struct arm_smccc_res res;

	return opencca_iommu_smc(OPENCCA_IOMMU_DESTROY, handle, 0UL, 0UL,
				 0UL, &res);
}
#endif

static inline void opencca_dma_retire(struct iommu_domain *domain,
		unsigned long iova, size_t size);

static inline int opencca_dma_reserve(struct iommu_domain *domain,
		unsigned long iova, phys_addr_t pa, size_t size)
{
#ifdef CONFIG_OPENCCA_MANAGED_IOMMU
	return 0;
#else
	struct arm_smccc_res res;
	size_t reserved = 0;

	if (!size || size > ULONG_MAX - iova || size > ULONG_MAX - pa)
		return -EINVAL;
	while (reserved < size) {
		size_t chunk = min_t(size_t, SZ_2M, size - reserved);

		arm_smccc_1_1_smc(OPENCCA_DMA_GUARD_SMC, 1UL,
			(unsigned long)domain, iova + reserved, pa + reserved,
			chunk, 0UL, 0UL, &res);
		if (res.a0) {
			/* No device PTEs have been published yet. */
			opencca_dma_retire(domain, iova, reserved);
			return res.a0 == SMCCC_RET_NOT_SUPPORTED ? -ENODEV : -EPERM;
		}
		reserved += chunk;
	}
	return 0;
#endif
}

static inline void opencca_dma_retire(struct iommu_domain *domain,
		unsigned long iova, size_t size)
{
#ifdef CONFIG_OPENCCA_MANAGED_IOMMU
	return;
#else
	struct arm_smccc_res res;

	if (!size)
		return;
	arm_smccc_1_1_smc(OPENCCA_DMA_GUARD_SMC, 2UL,
		(unsigned long)domain, iova, 0UL, size, 0UL, 0UL, &res);
	if (res.a0 != 0UL)
		pr_err_ratelimited("DMA guard reservation retained: domain=%p iova=%#lx size=%#zx status=%#lx\n",
				   domain, iova, size, res.a0);
#endif
}
#else
static inline int opencca_dma_reserve(struct iommu_domain *domain,
		unsigned long iova, phys_addr_t pa, size_t size)
{
	return 0;
}

static inline void opencca_dma_retire(struct iommu_domain *domain,
		unsigned long iova, size_t size)
{
}
#endif
#endif
