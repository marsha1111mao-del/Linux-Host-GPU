#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/ioport.h>
#include <linux/interrupt.h>
#include <linux/clk.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include "pmthor_drv.h"
#include "pmthor_regs.h"

static void pmthor_gpu_id_check(struct pmthor_device *ptdev)
{
	u32 id1, id2;

	id1 = gpu_read(ptdev, GPU_ID);
	id2 = gpu_read(ptdev, GPU_ID);

	dev_info(ptdev->dev, "GPU_ID = 0x%08x (stable=%s)\n", id1,
		 (id1 == id2) ? "yes" : "NO");
}
int pmthor_pm_init(struct pmthor_device *ptdev)
{
	int ret = 0;
	if (ret)
		return ret;
	ret = dev_pm_domain_attach(ptdev->dev, true);
	if (ret)
		return ret;
	// ret = devm_pm_runtime_enable(ptdev->dev);
	// if (ret)
	// 	return ret;

	// ret = pm_runtime_resume_and_get(ptdev->dev);
	return ret;
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
int pmthor_device_init(struct pmthor_device *ptdev)
{
	struct resource *res;
	int ret = 0;
	ret = pmthor_clk_init(ptdev);
	if (ret)
		return ret;
	ret = pmthor_pm_init(ptdev);
	if (ret)
		return ret;
	ptdev->coherent = device_get_dma_attr(ptdev->dev) == DEV_DMA_COHERENT;
	ptdev->iomem = devm_platform_get_and_ioremap_resource(
		to_platform_device(ptdev->dev), 0, &res);
	if (IS_ERR(ptdev->iomem))
		return PTR_ERR(ptdev->iomem);
	ptdev->phys_addr = res->start;
	pmthor_gpu_id_check(ptdev);
	return 0;
}
static int pmthor_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct pmthor_device *ptdev;
	dev_info(&pdev->dev, "[MZH]pmthor probe\n");
	ptdev = devm_kzalloc(&pdev->dev, sizeof(*ptdev), GFP_KERNEL);
	if (!ptdev)
		return -ENOMEM;
	ptdev->dev = &pdev->dev;
	ret = pmthor_device_init(ptdev);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, ptdev);

	return 0;
}

static void pmthor_remove(struct platform_device *pdev)
{
	dev_info(&pdev->dev, "[MZH]pmthor remove\n");
	struct pmthor_device *ptdev = platform_get_drvdata(pdev);

	clk_disable_unprepare(ptdev->clks.core);
	clk_disable_unprepare(ptdev->clks.stacks);
	clk_disable_unprepare(ptdev->clks.coregroup);

	dev_pm_domain_detach(ptdev->dev, true);

	dev_info(ptdev->dev, "GPU PM released\n");
}

static const struct of_device_id pmthor_of_match[] = {
	{ .compatible = "rockchip,rk3588-mali" },
	{ .compatible = "arm,mali-valhall-csf" },
	{}
};
MODULE_DEVICE_TABLE(of, pmthor_of_match);

static struct platform_driver pmthor_driver = {
	.probe  = pmthor_probe,
	.remove = pmthor_remove,
	.driver = {
		.name           = DRV_NAME,
		.of_match_table = pmthor_of_match,
	},
};
static int __init pmthor_init(void)
{
	pr_info("[MZH] pmthor init\n");
	return platform_driver_register(&pmthor_driver);
}

module_init(pmthor_init);

static void __exit pmthor_exit(void)
{
	pr_info("[MZH] pmthor exit\n");
	platform_driver_unregister(&pmthor_driver);
}
module_exit(pmthor_exit);

MODULE_DESCRIPTION("Minimal Mali Valhall DT stub driver");
MODULE_AUTHOR("You");
MODULE_LICENSE("GPL");