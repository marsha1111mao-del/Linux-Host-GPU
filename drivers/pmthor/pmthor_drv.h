#ifndef __PMTHOR_DRV_H__
#define __PMTHOR_DRV_H__

#include <linux/types.h>
#include <linux/device.h>
#include <linux/eventfd.h>
#include <linux/vfio.h>
#include <linux/miscdevice.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>

#define DRV_NAME "pmthor"

/* 前向声明内核结构体指针 */
struct virqfd;

struct pmthor_irq_stats {
	u64 signals;
	u64 unmask_events;
	u64 masked_samples;
	u64 unmask_without_signal;
	u64 ignored;
	u64 enable_calls;
	u64 disable_calls;
	u64 masked_total_ns;
	u64 masked_max_ns;
	u64 last_signal_ns;
	bool signal_pending;
};

/* 中断上下文结构 */
struct pmthor_irq {
	u32 flags;
	int irq;
	unsigned long hwintid;
	char *name;
	const char *label;
	spinlock_t lock;
	bool masked;
	bool enabled;
	struct pmthor_irq_stats stats;

	struct eventfd_ctx *trigger;
	struct virqfd *mask;
	struct virqfd *unmask;
};

/* 设备结构 */
struct pmthor_device {
	struct device *dev;
	struct miscdevice miscdev; /* 必须包含这个，用于 container_of */
	struct mutex owner_lock;
	bool opened;

	phys_addr_t phys_addr;
	void __iomem *iomem;
	bool coherent;

	struct {
		struct clk *core;
		struct clk *stacks;
		struct clk *coregroup;
	} clks;

	struct {
		atomic_t state;
		struct mutex mmio_lock;
		struct page *dummy_latest_flush;
	} pm;

	struct pmthor_irq job_irq;
	struct pmthor_irq mmu_irq;
	struct pmthor_irq gpu_irq;
};

/* 函数声明 */
int pmthor_clk_init(struct pmthor_device *ptdev);
int pmthor_pm_init(struct pmthor_device *ptdev);
int pmthor_device_init(struct pmthor_device *ptdev);

/* IOCTL 定义 */
#define PMTHOR_IOCTL_SET_IRQS _IOW('P', 0x01, struct vfio_irq_set)

#endif /* __PMTHOR_DRV_H__ */
