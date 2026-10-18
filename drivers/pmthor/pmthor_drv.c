// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/interrupt.h>
#include <linux/clk.h>
#include <linux/pm_domain.h>
#include <linux/vfio.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/eventfd.h>
#include <linux/irq.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/pagemap.h>
#include <linux/pmthor_gpu.h>
#include <linux/sched/mm.h>
#include <linux/xarray.h>
#include <linux/dma-mapping.h>
#include <linux/devfreq.h>
#include <linux/devfreq_cooling.h>
#include <linux/pm_opp.h>
#include <linux/regulator/consumer.h>

#include "pmthor_drv.h"
#include "pmthor_regs.h"
#include <asm/sysreg.h>

#define VFIO_IRQ_CLEAN (1 << 6)
#define PMTHOR_RESET_TIMEOUT_US 100000

static DEFINE_MUTEX(pmthor_registry_lock);
static struct pmthor_device *pmthor_registered_gpu;

struct pmthor_gpu_session {
	struct xarray pfns;
	unsigned long pfn_count;
	unsigned int vm_refs;
	struct device *dev;
};

static int pmthor_hw_quiesce(struct pmthor_device *ptdev, const char *reason);

/* Caller has reset the GPU after both the device and all VMs stopped. */
static void pmthor_gpu_session_release(struct pmthor_device *ptdev)
{
	struct pmthor_gpu_session *session = ptdev->gpu_session;
	unsigned long pfn, released = 0;
	void *entry;

	lockdep_assert_held(&ptdev->owner_lock);
	if (!session)
		return;
	if (WARN_ON_ONCE(ptdev->opened || session->vm_refs))
		return;

	xa_for_each(&session->pfns, pfn, entry) {
		struct page *page = pfn_to_page(pfn);

		/* All accepted mappings have DMA address == raw HPA. The GPU
		 * has been reset and both device/VM owners are gone here.
		 */
		dma_unmap_page(session->dev, PFN_PHYS(pfn), PAGE_SIZE,
			       DMA_BIDIRECTIONAL);
		set_page_dirty_lock(page);
		put_page(page);
		if (!(++released % 1024))
			cond_resched();
	}
	xa_destroy(&session->pfns);
	dev_info(session->dev, "pmthor: released GPU page references=%lu\n",
		 released);
	ptdev->gpu_session = NULL;
	put_device(session->dev);
	kfree(session);
}

void pmthor_gpu_put_vm_session(struct pmthor_gpu_session *session)
{
	struct pmthor_device *ptdev;

	if (!session)
		return;
	mutex_lock(&pmthor_registry_lock);
	ptdev = pmthor_registered_gpu;
	/* Active platform unbind is unsupported: retain pages fail-closed. */
	if (!ptdev || ptdev->dev != session->dev)
		goto out_registry;
	mutex_lock(&ptdev->owner_lock);
	if (WARN_ON_ONCE(ptdev->gpu_session != session || !session->vm_refs))
		goto out_owner;
	session->vm_refs--;
	if (!session->vm_refs && !ptdev->opened) {
		/*
		 * A still-running VM could write GPU MMIO after device close.
		 * Reset again after the last VM is gone, before releasing HPAs.
		 * A failure leaves the session for a later open/reset to retry.
		 */
		if (!pmthor_hw_quiesce(ptdev, "last VM teardown"))
			pmthor_gpu_session_release(ptdev);
	}
out_owner:
	mutex_unlock(&ptdev->owner_lock);
out_registry:
	mutex_unlock(&pmthor_registry_lock);
}
EXPORT_SYMBOL_GPL(pmthor_gpu_put_vm_session);

int pmthor_gpu_track_pfn(struct pmthor_gpu_session **vm_session,
			 unsigned long pfn)
{
	struct pmthor_device *ptdev;
	struct pmthor_gpu_session *session;
	dma_addr_t dma_addr;
	int ret = -ENODEV;

	if (!pfn_valid(pfn) || PageReserved(pfn_to_page(pfn)))
		return -EINVAL;

	/* Registry lock keeps remove from freeing the device during lookup. */
	mutex_lock(&pmthor_registry_lock);
	ptdev = pmthor_registered_gpu;
	if (!ptdev)
		goto out_registry;

	mutex_lock(&ptdev->owner_lock);
	if (!ptdev->opened || ptdev->owner_mm != current->mm) {
		ret = -EPERM;
		goto out_owner;
	}
	session = ptdev->gpu_session;
	if (*vm_session && *vm_session != session) {
		ret = -EPERM;
		goto out_owner;
	}
	if (!*vm_session) {
		session->vm_refs++;
		*vm_session = session;
	}

	/* Never repeat cache ownership transitions for a live GPU page. */
	if (xa_load(&session->pfns, pfn)) {
		ret = -EBUSY;
		goto out_owner;
	}

	/* Pinning a memfd page does not prepare it for noncoherent DMA.
	 * Map it before exposing its HPA and retain that mapping until reset.
	 * The raw-HPA ABI cannot represent an IOMMU or bounce-buffer address.
	 */
	dma_addr = dma_map_page(session->dev, pfn_to_page(pfn), 0, PAGE_SIZE,
				DMA_BIDIRECTIONAL);
	if (dma_mapping_error(session->dev, dma_addr)) {
		ret = -EIO;
		goto out_owner;
	}
	if (dma_addr != PFN_PHYS(pfn)) {
		dma_unmap_page(session->dev, dma_addr, PAGE_SIZE, DMA_BIDIRECTIONAL);
		ret = -EOPNOTSUPP;
		goto out_owner;
	}

	ret = xa_insert(&session->pfns, pfn, xa_mk_value(1),
			GFP_KERNEL_ACCOUNT);
	if (!ret)
		session->pfn_count++;
	else
		dma_unmap_page(session->dev, dma_addr, PAGE_SIZE, DMA_BIDIRECTIONAL);

out_owner:
	mutex_unlock(&ptdev->owner_lock);
out_registry:
	mutex_unlock(&pmthor_registry_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(pmthor_gpu_track_pfn);

static bool pmthor_irq_stats;
module_param_named(irq_stats, pmthor_irq_stats, bool, 0644);
MODULE_PARM_DESC(irq_stats,
		 "Collect low-overhead pmthor IRQ forwarding timing stats");

static void pmthor_irq_stats_reset_locked(struct pmthor_irq *irq)
{
	if (!pmthor_irq_stats)
		return;

	memset(&irq->stats, 0, sizeof(irq->stats));
}

static void pmthor_irq_stats_dump_snapshot(struct pmthor_irq *irq,
					   const struct pmthor_irq_stats *stats)
{
	u64 avg_ns = 0;

	if (!pmthor_irq_stats)
		return;

	if (stats->masked_samples)
		avg_ns = div64_u64(stats->masked_total_ns,
				   stats->masked_samples);

	pr_info("pmthor: [MZH][PMTHOR_IRQ_STATS] label=%s signals=%llu unmask_events=%llu masked_samples=%llu unmask_without_signal=%llu ignored=%llu enable_calls=%llu disable_calls=%llu masked_avg_ns=%llu masked_max_ns=%llu pending=%u\n",
		irq->label, stats->signals, stats->unmask_events,
		stats->masked_samples, stats->unmask_without_signal,
		stats->ignored, stats->enable_calls, stats->disable_calls,
		avg_ns, stats->masked_max_ns, stats->signal_pending);
}

static void pmthor_hw_mask_and_clear_irqs(struct pmthor_device *ptdev)
{
	gpu_write(ptdev, JOB_INT_MASK, 0);
	gpu_write(ptdev, MMU_INT_MASK, 0);
	gpu_write(ptdev, GPU_INT_MASK, 0);

	gpu_write(ptdev, JOB_INT_CLEAR, gpu_read(ptdev, JOB_INT_RAWSTAT));
	gpu_write(ptdev, MMU_INT_CLEAR, gpu_read(ptdev, MMU_INT_RAWSTAT));
	gpu_write(ptdev, GPU_INT_CLEAR, gpu_read(ptdev, GPU_INT_RAWSTAT));
}

static int pmthor_hw_quiesce(struct pmthor_device *ptdev, const char *reason)
{
	u32 status, raw_gpu, raw_job, raw_mmu;
	int ret;

	if (!ptdev->iomem)
		return -ENODEV;

	pmthor_hw_mask_and_clear_irqs(ptdev);

	gpu_write(ptdev, MCU_CONTROL, MCU_CONTROL_DISABLE);
	ret = readl_poll_timeout(ptdev->iomem + MCU_STATUS, status,
				 status == MCU_STATUS_DISABLED, 10,
				 PMTHOR_RESET_TIMEOUT_US);
	if (ret) {
		dev_warn(ptdev->dev,
			 "pmthor: MCU did not stop during %s (status=%u), resetting GPU anyway\n",
			 reason, status);
	}

	gpu_write(ptdev, GPU_INT_CLEAR, GPU_IRQ_RESET_COMPLETED);
	gpu_write(ptdev, GPU_CMD, GPU_SOFT_RESET);
	ret = readl_poll_timeout(ptdev->iomem + GPU_INT_RAWSTAT, status,
				 status & GPU_IRQ_RESET_COMPLETED, 10,
				 PMTHOR_RESET_TIMEOUT_US);
	if (ret) {
		dev_warn(ptdev->dev,
			 "pmthor: soft reset timed out during %s, issuing hard reset\n",
			 reason);
		gpu_write(ptdev, GPU_CMD, GPU_HARD_RESET);
		ret = readl_poll_timeout(ptdev->iomem + GPU_INT_RAWSTAT,
					 status,
					 status & GPU_IRQ_RESET_COMPLETED, 10,
					 PMTHOR_RESET_TIMEOUT_US);
		if (ret)
			dev_warn(ptdev->dev,
				 "pmthor: hard reset timed out during %s\n",
				 reason);
	}

	pmthor_hw_mask_and_clear_irqs(ptdev);

	raw_gpu = gpu_read(ptdev, GPU_INT_RAWSTAT);
	raw_job = gpu_read(ptdev, JOB_INT_RAWSTAT);
	raw_mmu = gpu_read(ptdev, MMU_INT_RAWSTAT);
	if (ret) {
		dev_err(ptdev->dev,
			"pmthor: GPU reset failed for %s; retaining page references\n",
			reason);
		return ret;
	}
	dev_info(ptdev->dev,
		 "pmthor: hardware quiesced for %s (mcu=%u gpu_raw=%#x job_raw=%#x mmu_raw=%#x)\n",
		 reason, gpu_read(ptdev, MCU_STATUS), raw_gpu, raw_job,
		 raw_mmu);
	return 0;
}

static void pmthor_irq_enable_locked(struct pmthor_irq *irq)
{
	if (!irq->enabled) {
		enable_irq(irq->irq);
		irq->enabled = true;
		if (pmthor_irq_stats)
			irq->stats.enable_calls++;
	}
}

static void pmthor_irq_disable_locked(struct pmthor_irq *irq)
{
	if (irq->enabled) {
		disable_irq_nosync(irq->irq);
		irq->enabled = false;
		if (pmthor_irq_stats)
			irq->stats.disable_calls++;
	}
}

static void pmthor_irq_release_trigger(struct pmthor_irq *irq)
{
	struct eventfd_ctx *trigger;
	struct pmthor_irq_stats stats;
	bool dump_stats = false;
	unsigned long flags;

	spin_lock_irqsave(&irq->lock, flags);
	trigger = irq->trigger;
	irq->trigger = NULL;
	irq->masked = false;
	if (trigger && pmthor_irq_stats) {
		stats = irq->stats;
		dump_stats = true;
	}
	pmthor_irq_disable_locked(irq);
	spin_unlock_irqrestore(&irq->lock, flags);

	if (!trigger) {
		synchronize_irq(irq->irq);
		return;
	}

	synchronize_irq(irq->irq);

	if (dump_stats)
		pmthor_irq_stats_dump_snapshot(irq, &stats);
	pr_debug("pmthor: irq %s trigger fd cleared\n", irq->label);
	eventfd_ctx_put(trigger);
}

static void pmthor_irq_release_session(struct pmthor_irq *irq)
{
	vfio_virqfd_disable(&irq->mask);
	vfio_virqfd_disable(&irq->unmask);
	pmthor_irq_release_trigger(irq);
}

static int pmthor_vfio_irq_release_session(struct pmthor_device *ptdev)
{
	struct pmthor_irq *irqs[3] = { &ptdev->job_irq, &ptdev->mmu_irq,
				       &ptdev->gpu_irq };

	for (int i = 0; i < 3; i++) {
		if (irqs[i]->irq >= 0)
			pmthor_irq_release_session(irqs[i]);
	}

	return pmthor_hw_quiesce(ptdev, "session release");
}

static int pmthor_vfio_irq_cleanup(struct pmthor_device *ptdev)
{
	struct pmthor_irq *irqs[3] = { &ptdev->job_irq, &ptdev->mmu_irq,
				       &ptdev->gpu_irq };

	int ret = pmthor_vfio_irq_release_session(ptdev);

	for (int i = 0; i < 3; i++) {
		if (irqs[i]->irq >= 0) {
			free_irq(irqs[i]->irq, irqs[i]);
			irqs[i]->irq = -1;
		}
		kfree(irqs[i]->name);
		irqs[i]->name = NULL;
	}
	return ret;
}

static void pmthor_pm_detach(void *data)
{
	dev_pm_domain_detach(data, true);
}

int pmthor_pm_init(struct pmthor_device *ptdev)
{
	int ret = dev_pm_domain_attach(ptdev->dev, true);

	if (ret)
		return ret;
	return devm_add_action_or_reset(ptdev->dev, pmthor_pm_detach, ptdev->dev);
}

int pmthor_clk_init(struct pmthor_device *ptdev)
{
	ptdev->clks.core = devm_clk_get(ptdev->dev, NULL);
	if (IS_ERR(ptdev->clks.core))
		return dev_err_probe(ptdev->dev, PTR_ERR(ptdev->clks.core),
				     "get 'core' clock failed");

	ptdev->clks.stacks = devm_clk_get_optional(ptdev->dev, "stacks");
	if (IS_ERR(ptdev->clks.stacks))
		return dev_err_probe(ptdev->dev, PTR_ERR(ptdev->clks.stacks),
				     "get 'stacks' clock failed");

	ptdev->clks.coregroup = devm_clk_get_optional(ptdev->dev, "coregroup");
	if (IS_ERR(ptdev->clks.coregroup))
		return dev_err_probe(ptdev->dev, PTR_ERR(ptdev->clks.coregroup),
				     "get 'coregroup' clock failed");

	pr_info("clock rate = %lu\n", clk_get_rate(ptdev->clks.core));

	return 0;
}

static int pmthor_devfreq_target(struct device *dev, unsigned long *freq,
				 u32 flags)
{
	struct pmthor_device *ptdev = dev_get_drvdata(dev);
	struct dev_pm_opp *opp;
	int ret;

	opp = devfreq_recommended_opp(dev, freq, flags);
	if (IS_ERR(opp))
		return PTR_ERR(opp);
	dev_pm_opp_put(opp);

	/* Serialize rate changes with owner teardown and hardware reset. */
	mutex_lock(&ptdev->owner_lock);
	ret = dev_pm_opp_set_rate(dev, *freq);
	mutex_unlock(&ptdev->owner_lock);
	return ret;
}

static void pmthor_cooling_unregister(void *data)
{
	devfreq_cooling_unregister(data);
}

static void pmthor_clks_disable(void *data)
{
	struct pmthor_device *ptdev = data;

	clk_disable_unprepare(ptdev->clks.coregroup);
	clk_disable_unprepare(ptdev->clks.stacks);
	clk_disable_unprepare(ptdev->clks.core);
}

static int pmthor_clks_enable(struct pmthor_device *ptdev)
{
	int ret;

	ret = clk_prepare_enable(ptdev->clks.core);
	if (ret)
		return ret;
	ret = clk_prepare_enable(ptdev->clks.stacks);
	if (ret)
		goto disable_core;
	ret = clk_prepare_enable(ptdev->clks.coregroup);
	if (ret)
		goto disable_stacks;
	return devm_add_action_or_reset(ptdev->dev, pmthor_clks_disable, ptdev);

disable_stacks:
	clk_disable_unprepare(ptdev->clks.stacks);
disable_core:
	clk_disable_unprepare(ptdev->clks.core);
	return ret;
}

static int pmthor_devfreq_init(struct pmthor_device *ptdev)
{
	/* SRAM is coupled to mali for voltage changes, but needs its own enable
	 * reference, as in the native Panthor driver. Retain both until remove.
	 */
	static const char * const supplies[] = { "mali", NULL };
	struct device *dev = ptdev->dev;
	struct devfreq_dev_profile *profile;
	struct thermal_cooling_device *cooling;
	struct dev_pm_opp *opp;
	unsigned long freq = clk_get_rate(ptdev->clks.core);
	int ret;

	ret = devm_pm_opp_set_regulators(dev, supplies);
	if (ret)
		return ret;
	ret = devm_pm_opp_of_add_table(dev);
	if (ret)
		return ret;
	ret = devm_regulator_get_enable_optional(dev, "sram");
	if (ret && ret != -ENODEV)
		return ret;

	opp = devfreq_recommended_opp(dev, &freq, 0);
	if (IS_ERR(opp))
		return PTR_ERR(opp);
	ret = dev_pm_opp_set_opp(dev, opp);
	dev_pm_opp_put(opp);
	if (ret)
		return ret;
	/* Enable clocks after supplies, before registering rate-changing
	 * callbacks. Reverse devres cleanup then has the same safe order.
	 */
	ret = pmthor_clks_enable(ptdev);
	if (ret)
		return ret;

	profile = devm_kzalloc(dev, sizeof(*profile), GFP_KERNEL);
	if (!profile)
		return -ENOMEM;
	profile->initial_freq = freq;
	profile->target = pmthor_devfreq_target;
	/* Actual scheduler utilization lives in the Proxy. Provide explicit OPP
	 * control, not fabricated busy counters for an ondemand governor.
	 */
	ptdev->devfreq = devm_devfreq_add_device(dev, profile,
					       DEVFREQ_GOV_USERSPACE, NULL);
	if (IS_ERR(ptdev->devfreq))
		return PTR_ERR(ptdev->devfreq);
	ret = devm_devfreq_register_opp_notifier(dev, ptdev->devfreq);
	if (ret)
		return ret;

	if (IS_ENABLED(CONFIG_DEVFREQ_THERMAL)) {
		cooling = of_devfreq_cooling_register(dev->of_node, ptdev->devfreq);
		if (IS_ERR(cooling))
			return PTR_ERR(cooling);
		ret = devm_add_action_or_reset(dev, pmthor_cooling_unregister, cooling);
		if (ret)
			return ret;
	}

	dev_info(dev, "pmthor: userspace OPP control ready, initial_hz=%lu actual_hz=%lu\n",
		 freq, clk_get_rate(ptdev->clks.core));
	return 0;
}

/* ====================== IRQ 基础操作 ====================== */

static void pmthor_mask(struct pmthor_irq *irq)
{
	unsigned long flags;

	spin_lock_irqsave(&irq->lock, flags);
	irq->masked = true;
	pmthor_irq_disable_locked(irq);
	spin_unlock_irqrestore(&irq->lock, flags);
}

static void pmthor_unmask(struct pmthor_irq *irq)
{
	unsigned long flags;
	u64 now = 0, delta;

	spin_lock_irqsave(&irq->lock, flags);
	if (pmthor_irq_stats) {
		now = ktime_get_ns();
		irq->stats.unmask_events++;
		if (irq->stats.signal_pending) {
			delta = now - irq->stats.last_signal_ns;
			irq->stats.masked_samples++;
			irq->stats.masked_total_ns += delta;
			if (delta > irq->stats.masked_max_ns)
				irq->stats.masked_max_ns = delta;
			irq->stats.signal_pending = false;
		} else {
			irq->stats.unmask_without_signal++;
		}
	}
	if (irq->trigger) {
		irq->masked = false;
		pmthor_irq_enable_locked(irq);
	}
	spin_unlock_irqrestore(&irq->lock, flags);
}

static int pmthor_mask_handler(void *opaque, void *unused)
{
	pmthor_mask(opaque);
	return 0;
}
static int pmthor_unmask_handler(void *opaque, void *unused)
{
	pmthor_unmask(opaque);
	return 0;
}

static irqreturn_t pmthor_automasked_irq_handler(int irq, void *dev_id)
{
	struct pmthor_irq *irq_ctx = dev_id;
	struct eventfd_ctx *trigger = NULL;
	unsigned long flags;

	spin_lock_irqsave(&irq_ctx->lock, flags);
	if (irq_ctx->trigger && !irq_ctx->masked) {
		pmthor_irq_disable_locked(irq_ctx);
		irq_ctx->masked = true;
		trigger = irq_ctx->trigger;
		if (pmthor_irq_stats) {
			irq_ctx->stats.signals++;
			irq_ctx->stats.last_signal_ns = ktime_get_ns();
			irq_ctx->stats.signal_pending = true;
		}
	} else if (pmthor_irq_stats) {
		irq_ctx->stats.ignored++;
	}
	spin_unlock_irqrestore(&irq_ctx->lock, flags);

	if (trigger)
		eventfd_signal(trigger);

	return IRQ_HANDLED;
}

/* ====================== VFIO 风格设置接口 ====================== */

static int pmthor_set_trigger(struct pmthor_irq *irq, int fd)
{
	struct eventfd_ctx *trigger;
	unsigned long flags;

	pmthor_irq_release_trigger(irq);

	if (fd < 0)
		return 0;

	trigger = eventfd_ctx_fdget(fd);
	if (IS_ERR(trigger))
		return PTR_ERR(trigger);

	pr_debug("pmthor: irq %s trigger fd installed\n", irq->label);

	spin_lock_irqsave(&irq->lock, flags);
	irq->trigger = trigger;
	irq->masked = false;
	pmthor_irq_stats_reset_locked(irq);
	pmthor_irq_enable_locked(irq);
	spin_unlock_irqrestore(&irq->lock, flags);

	return 0;
}

static int pmthor_set_irq_mask(struct pmthor_irq *irq, int fd)
{
	if (fd >= 0) {
		pr_debug("pmthor: irq %s mask fd installed\n", irq->label);
		return vfio_virqfd_enable(irq, pmthor_mask_handler, NULL, NULL,
					  &irq->mask, fd);
	}

	if (fd < 0) {
		pr_debug("pmthor: irq %s mask fd cleared\n", irq->label);
		vfio_virqfd_disable(&irq->mask);
	}

	return 0;
}

static int pmthor_set_irq_unmask(struct pmthor_irq *irq, int fd)
{
	if (fd >= 0) {
		pr_debug("pmthor: irq %s unmask fd installed\n", irq->label);
		return vfio_virqfd_enable(irq, pmthor_unmask_handler, NULL,
					  NULL, &irq->unmask, fd);
	}

	if (fd < 0) {
		pr_debug("pmthor: irq %s unmask fd cleared\n", irq->label);
		vfio_virqfd_disable(&irq->unmask);
	}

	return 0;
}

/* ====================== IOCTL 处理 ====================== */

static long pmthor_misc_ioctl(struct file *file, unsigned int cmd,
			      unsigned long arg)
{
	struct pmthor_device *ptdev = file->private_data;
	struct vfio_irq_set hdr;
	struct pmthor_irq *irq;
	int32_t fd = -1;

	if (cmd != PMTHOR_IOCTL_SET_IRQS)
		return -ENOTTY;
	if (copy_from_user(&hdr, (void __user *)arg, sizeof(hdr)))
		return -EFAULT;

	if (hdr.flags & VFIO_IRQ_CLEAN) {
		dev_dbg(ptdev->dev, "pmthor: clean irq session\n");
		return pmthor_vfio_irq_release_session(ptdev);
	}

	if (hdr.index > 2)
		return -EINVAL;

	irq = (hdr.index == 0) ? &ptdev->job_irq :
	      (hdr.index == 1) ? &ptdev->mmu_irq :
				 &ptdev->gpu_irq;

	if (hdr.count != 1) {
		if (!hdr.count &&
		    (hdr.flags & VFIO_IRQ_SET_ACTION_TYPE_MASK) ==
			    VFIO_IRQ_SET_ACTION_TRIGGER)
			return pmthor_set_trigger(irq, -1);
		return -EINVAL;
	}

	if (hdr.flags & VFIO_IRQ_SET_DATA_EVENTFD) {
		if (copy_from_user(&fd, (void __user *)(arg + sizeof(hdr)),
				   sizeof(fd)))
			return -EFAULT;
	}

	switch (hdr.flags & VFIO_IRQ_SET_ACTION_TYPE_MASK) {
	case VFIO_IRQ_SET_ACTION_TRIGGER:
		return pmthor_set_trigger(irq, fd);
	case VFIO_IRQ_SET_ACTION_MASK:
		if (hdr.flags & VFIO_IRQ_SET_DATA_EVENTFD)
			return pmthor_set_irq_mask(irq, fd);
		pmthor_mask(irq);
		return 0;
	case VFIO_IRQ_SET_ACTION_UNMASK:
		if (hdr.flags & VFIO_IRQ_SET_DATA_EVENTFD)
			return pmthor_set_irq_unmask(irq, fd);
		pmthor_unmask(irq);
		return 0;
	}
	return -EINVAL;
}

static int pmthor_misc_open(struct inode *inode, struct file *file)
{
	/* file->private_data 在 open 被调用时指向的是 misc_register 注册的 struct miscdevice */
	struct miscdevice *misc = file->private_data;
	struct pmthor_device *ptdev =
		container_of(misc, struct pmthor_device, miscdev);
	struct pmthor_gpu_session *session;
	int ret = 0;

	if (!current->mm)
		return -EINVAL;

	mutex_lock(&ptdev->owner_lock);
	if (ptdev->opened ||
	    (ptdev->gpu_session && ptdev->gpu_session->vm_refs)) {
		ret = -EBUSY;
		goto out_unlock;
	}

	ret = pmthor_hw_quiesce(ptdev, "open");
	if (ret)
		goto out_unlock;

	/* A failed reset at a previous close kept its references fail-closed. */
	pmthor_gpu_session_release(ptdev);
	session = kzalloc(sizeof(*session), GFP_KERNEL_ACCOUNT);
	if (!session) {
		ret = -ENOMEM;
		goto out_unlock;
	}
	xa_init(&session->pfns);
	session->dev = get_device(ptdev->dev);
	ptdev->gpu_session = session;
	ptdev->owner_mm = current->mm;
	mmgrab(ptdev->owner_mm);
	WRITE_ONCE(ptdev->opened, true);

	dev_dbg(ptdev->dev, "pmthor: /dev/pmthor opened\n");
	file->private_data = ptdev;
out_unlock:
	mutex_unlock(&ptdev->owner_lock);
	return ret;
}

static int pmthor_misc_release(struct inode *inode, struct file *file)
{
	struct pmthor_device *ptdev = file->private_data;
	struct pmthor_gpu_session *session;
	int ret;

	mutex_lock(&ptdev->owner_lock);
	session = ptdev->gpu_session;
	ret = pmthor_vfio_irq_release_session(ptdev);
	dev_info(ptdev->dev,
		 "pmthor: GPU session closed pages=%lu vm_refs=%u\n",
		 session->pfn_count, session->vm_refs);
	mmdrop(ptdev->owner_mm);
	ptdev->owner_mm = NULL;
	WRITE_ONCE(ptdev->opened, false);
	/* A live VM keeps the session exclusive and requires another reset. */
	if (!ret && !session->vm_refs)
		pmthor_gpu_session_release(ptdev);
	mutex_unlock(&ptdev->owner_lock);

	dev_dbg(ptdev->dev, "pmthor: /dev/pmthor released\n");
	return ret;
}

static const struct file_operations pmthor_misc_fops = {
	.owner = THIS_MODULE,
	.open = pmthor_misc_open,
	.release = pmthor_misc_release,
	.unlocked_ioctl = pmthor_misc_ioctl,
	.compat_ioctl = pmthor_misc_ioctl,
};

/* ====================== 初始化与清理 ====================== */

static int pmthor_vfio_irq_init(struct pmthor_device *ptdev)
{
	struct pmthor_irq *irqs[3] = { &ptdev->job_irq, &ptdev->mmu_irq,
				       &ptdev->gpu_irq };
	const char *names[3] = { "job", "mmu", "gpu" };
	int i, ret;

	for (i = 0; i < 3; i++) {
		struct pmthor_irq *irq = irqs[i];
		spin_lock_init(&irq->lock);

		irq->irq = platform_get_irq_byname(
			to_platform_device(ptdev->dev), names[i]);
		if (irq->irq < 0) {
			pr_err("pmthor: Failed to get IRQ by name '%s', error: %d\n",
			       names[i], irq->irq);
			return irq->irq;
		}
		irq->label = names[i];
		irq->masked = false;
		irq->enabled = false;

		irq->name = kasprintf(GFP_KERNEL, "pmthor-%s[%d]", names[i],
				      irq->irq);
		if (!irq->name) {
			ret = -ENOMEM;
			goto err;
		}

		// 添加 IRQF_TRIGGER_HIGH 标志
		ret = request_irq(irq->irq, pmthor_automasked_irq_handler,
				  IRQF_NO_AUTOEN, irq->name, irq);
		if (ret) {
			pr_err("pmthor: request_irq failed for %s (irq %d), error: %d\n",
			       names[i], irq->irq, ret);
			kfree(irq->name);
			goto err;
		}

		struct irq_data *d = irq_get_irq_data(irq->irq);
		irq->hwintid = d ? d->hwirq : 0;
		pr_info("pmthor: registered IRQ %s at %d (hwirq: %lu)\n",
			names[i], irq->irq, irq->hwintid);
	}
	return 0;
err:
	while (--i >= 0) {
		free_irq(irqs[i]->irq, irqs[i]);
		kfree(irqs[i]->name);
	}
	return ret;
}


/* ====================== Probe / Remove ====================== */
static int pmthor_probe(struct platform_device *pdev)
{
	struct pmthor_device *ptdev;
	struct resource *res;
	u32 pa_bits;
	int ret;

	pr_info("pmthor: probe starting...\n");

	ptdev = devm_kzalloc(&pdev->dev, sizeof(*ptdev), GFP_KERNEL);
	if (!ptdev)
		return -ENOMEM;
	ptdev->dev = &pdev->dev;
	mutex_init(&ptdev->owner_lock);
	platform_set_drvdata(pdev, ptdev);

	// 1. 硬件资源初始化
	ret = pmthor_clk_init(ptdev);
	if (ret) {
		pr_err("pmthor: pmthor_clk_init failed: %d\n", ret);
		return ret;
	}

	ret = pmthor_pm_init(ptdev);
	if (ret) {
		pr_err("pmthor: pmthor_pm_init failed: %d\n", ret);
		return ret;
	}

	ptdev->iomem = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(ptdev->iomem)) {
		pr_err("pmthor: ioremap failed: %ld\n", PTR_ERR(ptdev->iomem));
		return PTR_ERR(ptdev->iomem);
	}
	ptdev->phys_addr = res->start;

	/* Complete managed power setup before IRQ registration, so even an
	 * immediate devres-action failure can gate clocks without IRQ cleanup
	 * subsequently accessing an unclocked device.
	 */
	ret = pmthor_devfreq_init(ptdev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "initialize OPP controls failed\n");
	pa_bits = GPU_MMU_FEATURES_PA_BITS(gpu_read(ptdev, GPU_MMU_FEATURES));
	if (pa_bits < 32 || pa_bits > 64)
		return -EINVAL;
	ret = dma_set_mask_and_coherent(ptdev->dev, DMA_BIT_MASK(pa_bits));
	if (ret)
		return ret;

	// 2. 中断初始化
	ret = pmthor_vfio_irq_init(ptdev);
	if (ret) {
		pr_err("pmthor: pmthor_vfio_irq_init failed: %d\n", ret);
		return ret;
	}

	// 3. 注册 MISC 设备
	ptdev->miscdev.minor = MISC_DYNAMIC_MINOR;
	ptdev->miscdev.name = DRV_NAME;
	ptdev->miscdev.fops = &pmthor_misc_fops;
	ptdev->miscdev.mode = 0666;

	ret = misc_register(&ptdev->miscdev);
	if (ret) {
		pr_err("pmthor: misc_register failed: %d\n", ret);
		pmthor_vfio_irq_cleanup(ptdev);
		return ret;
	}

	platform_set_drvdata(pdev, ptdev);
	mutex_lock(&pmthor_registry_lock);
	pmthor_registered_gpu = ptdev;
	mutex_unlock(&pmthor_registry_lock);
	dev_info(&pdev->dev, "pmthor successfully initialized at 0x%pa\n",
		 &ptdev->phys_addr);
	return 0;
}

static void pmthor_remove(struct platform_device *pdev)
{
	struct pmthor_device *ptdev = platform_get_drvdata(pdev);
	int ret;

	if (!ptdev)
		return;

	mutex_lock(&pmthor_registry_lock);
	pmthor_registered_gpu = NULL;
	mutex_unlock(&pmthor_registry_lock);
	misc_deregister(&ptdev->miscdev);
	mutex_lock(&ptdev->owner_lock);
	ret = pmthor_vfio_irq_cleanup(ptdev);
	if (!ret && !ptdev->opened && ptdev->gpu_session &&
	    !ptdev->gpu_session->vm_refs)
		pmthor_gpu_session_release(ptdev);
	if (ptdev->gpu_session || ret)
		dev_warn(ptdev->dev,
			 "pmthor: removing active/unquiesced GPU; retaining page references\n");
	mutex_unlock(&ptdev->owner_lock);
	/* Devres removes cooling/devfreq callbacks, gates clocks, then releases
	 * OPP/supply references and the power domain, including probe unwind.
	 */
}

static const struct of_device_id pmthor_of_match[] = {
	{ .compatible = "rockchip,rk3588-mali" },
	{ .compatible = "arm,mali-valhall-csf" },
	{}
};
MODULE_DEVICE_TABLE(of, pmthor_of_match);

static struct platform_driver pmthor_driver = {
	.probe = pmthor_probe,
	.remove = pmthor_remove,
	.driver = { .name = DRV_NAME, .of_match_table = pmthor_of_match },
};

module_platform_driver(pmthor_driver);
MODULE_LICENSE("GPL");
