#define DRV_NAME "pmthor"
struct pmthor_device {
	struct device *dev;

	/** @phys_addr: Physical address of the iomem region. */
	phys_addr_t phys_addr;

	/** @iomem: CPU mapping of the IOMEM region. */
	void __iomem *iomem;
	/** @coherent: True if the CPU/GPU are memory coherent. */
	bool coherent;

	/** @clks: GPU clocks. */
	struct {
		/** @core: Core clock. */
		struct clk *core;

		/** @stacks: Stacks clock. This clock is optional. */
		struct clk *stacks;

		/** @coregroup: Core group clock. This clock is optional. */
		struct clk *coregroup;
	} clks;
	/** @pm: Power management related data. */
	struct {
		/** @state: Power state. */
		atomic_t state;

		/**
		 * @mmio_lock: Lock protecting MMIO userspace CPU mappings.
		 *
		 * This is needed to ensure we map the dummy IO pages when
		 * the device is being suspended, and the real IO pages when
		 * the device is being resumed. We can't just do with the
		 * state atomicity to deal with this race.
		 */
		struct mutex mmio_lock;

		/**
		 * @dummy_latest_flush: Dummy LATEST_FLUSH page.
		 *
		 * Used to replace the real LATEST_FLUSH page when the GPU
		 * is suspended.
		 */
		struct page *dummy_latest_flush;
	} pm;
};
int pmthor_clk_init(struct pmthor_device *ptdev);
int pmthor_device_init(struct pmthor_device *ptdev);
int pmthor_pm_init(struct pmthor_device *ptdev);