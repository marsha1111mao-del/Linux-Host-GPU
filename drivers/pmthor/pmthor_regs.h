#ifndef __PMTHOR_REGS_H__
#define __PMTHOR_REGS_H__

#include <linux/bits.h>

#define GPU_ID						0x0

#define GPU_INT_RAWSTAT				0x20
#define GPU_INT_CLEAR					0x24
#define GPU_INT_MASK					0x28
#define GPU_CMD						0x30
#define GPU_CMD_DEF(type, payload)			((type) | ((payload) << 8))
#define GPU_SOFT_RESET					GPU_CMD_DEF(1, 1)
#define GPU_HARD_RESET					GPU_CMD_DEF(1, 2)
#define GPU_IRQ_RESET_COMPLETED				BIT(8)

#define MCU_CONTROL					0x700
#define MCU_CONTROL_DISABLE				0
#define MCU_STATUS					0x704
#define MCU_STATUS_DISABLED				0

#define JOB_INT_RAWSTAT				0x1000
#define JOB_INT_CLEAR					0x1004
#define JOB_INT_MASK					0x1008

#define MMU_INT_RAWSTAT				0x2000
#define MMU_INT_CLEAR					0x2004
#define MMU_INT_MASK					0x2008

#define PMTHOR_ALL_INTERRUPTS				0xffffffff

#define gpu_write(dev, reg, data) writel((data), (dev)->iomem + (reg))
#define gpu_read(dev, reg) readl((dev)->iomem + (reg))

#endif /* __PMTHOR_REGS_H__ */
