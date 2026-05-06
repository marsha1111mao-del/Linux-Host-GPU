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

#include "pmthor_drv.h"
#include "pmthor_regs.h"
#include <asm/sysreg.h>

int pmthor_pm_init(struct pmthor_device *ptdev)
{
	return dev_pm_domain_attach(ptdev->dev, true);
}

int pmthor_clk_init(struct pmthor_device *ptdev)
{
	int ret = 0;
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

	ret = clk_prepare_enable(ptdev->clks.core);
	if (ret)
		return ret;
	ret = clk_prepare_enable(ptdev->clks.stacks);
	if (ret)
		return ret;
	ret = clk_prepare_enable(ptdev->clks.coregroup);
	if (ret)
		return ret;
	return 0;
}

/* ====================== IRQ 基础操作 ====================== */

static void pmthor_mask(struct pmthor_irq *irq_ctx)
{
	unsigned long flags;
	spin_lock_irqsave(&irq_ctx->lock, flags);
	if (!irq_ctx->masked) {
		disable_irq_nosync(irq_ctx->irq);
		irq_ctx->masked = true;
	}
	spin_unlock_irqrestore(&irq_ctx->lock, flags);
}

static void pmthor_unmask(struct pmthor_irq *irq_ctx)
{
	unsigned long flags;
	spin_lock_irqsave(&irq_ctx->lock, flags);
	if (irq_ctx->masked) {
		enable_irq(irq_ctx->irq);
		irq_ctx->masked = false;
	}
	spin_unlock_irqrestore(&irq_ctx->lock, flags);
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
	unsigned long flags;
	bool signaled = false;

	spin_lock_irqsave(&irq_ctx->lock, flags);
	if (!irq_ctx->masked) {
		disable_irq_nosync(irq_ctx->irq);
		irq_ctx->masked = true;
		signaled = true;
	}
	spin_unlock_irqrestore(&irq_ctx->lock, flags);

	if (signaled && irq_ctx->trigger)
		eventfd_signal(irq_ctx->trigger);

	return IRQ_HANDLED;
}

/* ====================== VFIO 风格设置接口 ====================== */

static int pmthor_set_trigger(struct pmthor_irq *irq, int fd)
{
	if (irq->trigger) {
		disable_irq(irq->irq);
		eventfd_ctx_put(irq->trigger);
		irq->trigger = NULL;
	}

	if (fd < 0)
		return 0;

	irq->trigger = eventfd_ctx_fdget(fd);
	if (IS_ERR(irq->trigger))
		return PTR_ERR(irq->trigger);

	enable_irq(irq->irq);
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
	if (hdr.index > 2 || hdr.count != 1)
		return -EINVAL;

	irq = (hdr.index == 0) ? &ptdev->job_irq :
	      (hdr.index == 1) ? &ptdev->mmu_irq :
				 &ptdev->gpu_irq;

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
			return vfio_virqfd_enable(irq, pmthor_mask_handler,
						  NULL, NULL, &irq->mask, fd);
		pmthor_mask(irq);
		return 0;
	case VFIO_IRQ_SET_ACTION_UNMASK:
		if (hdr.flags & VFIO_IRQ_SET_DATA_EVENTFD)
			return vfio_virqfd_enable(irq, pmthor_unmask_handler,
						  NULL, NULL, &irq->unmask, fd);
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

	/* 将 ptdev 存入 private_data 供以后的 ioctl 使用 */
	file->private_data = ptdev;
	return 0;
}

static const struct file_operations pmthor_misc_fops = {
	.owner = THIS_MODULE,
	.open = pmthor_misc_open,
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

		irq->name = kasprintf(GFP_KERNEL, "pmthor-%s[%d]", names[i],
				      irq->irq);
		if (!irq->name)
			irq->name = "pmthor-fallback";

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
static void pmthor_vfio_irq_cleanup(struct pmthor_device *ptdev)
{
	struct pmthor_irq *irqs[3] = { &ptdev->job_irq, &ptdev->mmu_irq,
				       &ptdev->gpu_irq };
	for (int i = 0; i < 3; i++) {
		vfio_virqfd_disable(&irqs[i]->mask);
		vfio_virqfd_disable(&irqs[i]->unmask);
		if (irqs[i]->trigger)
			eventfd_ctx_put(irqs[i]->trigger);
		if (irqs[i]->irq >= 0)
			free_irq(irqs[i]->irq, irqs[i]);
		kfree(irqs[i]->name);
	}
}

/* ====================== Probe / Remove ====================== */
static int pmthor_probe(struct platform_device *pdev)
{
	struct pmthor_device *ptdev;
	struct resource *res;
	int ret;

	pr_info("pmthor: probe starting...\n");

	ptdev = devm_kzalloc(&pdev->dev, sizeof(*ptdev), GFP_KERNEL);
	if (!ptdev)
		return -ENOMEM;
	ptdev->dev = &pdev->dev;

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
	dev_info(&pdev->dev, "pmthor successfully initialized at 0x%pa\n",
		 &ptdev->phys_addr);
	return 0;
}

static void pmthor_remove(struct platform_device *pdev)
{
	struct pmthor_device *ptdev = platform_get_drvdata(pdev);
	if (!ptdev)
		return;

	misc_deregister(&ptdev->miscdev);
	pmthor_vfio_irq_cleanup(ptdev);
	clk_disable_unprepare(ptdev->clks.core);
	dev_pm_domain_detach(ptdev->dev, true);
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