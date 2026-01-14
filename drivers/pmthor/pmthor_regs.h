#define GPU_ID 0x0
#define gpu_write(dev, reg, data) writel(data, (dev)->iomem + (reg))

#define gpu_read(dev, reg) readl((dev)->iomem + (reg))