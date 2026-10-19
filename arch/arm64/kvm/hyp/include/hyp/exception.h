#ifndef __ARM64_KVM_HYP_EXCEPTION_H__
#define __ARM64_KVM_HYP_EXCEPTION_H__

#include <asm/kvm_emulate.h>

static inline unsigned long exception64_vector(unsigned long pstate,
					       unsigned long target_mode)
{
	unsigned long mode = pstate & (PSR_MODE_MASK | PSR_MODE32_BIT);

	if (mode == target_mode)
		return CURRENT_EL_SP_ELx_VECTOR;
	if ((mode | PSR_MODE_THREAD_BIT) == target_mode)
		return CURRENT_EL_SP_EL0_VECTOR;
	if (!(mode & PSR_MODE32_BIT))
		return LOWER_EL_AArch64_VECTOR;
	return LOWER_EL_AArch32_VECTOR;
}

static inline unsigned long exception64_pstate(unsigned long old,
		unsigned long sctlr, unsigned long target_mode, bool has_mte)
{
	unsigned long pstate = old & (PSR_N_BIT | PSR_Z_BIT | PSR_C_BIT |
				     PSR_V_BIT | PSR_DIT_BIT | PSR_PAN_BIT);

	if (has_mte)
		pstate |= PSR_TCO_BIT;
	if (!(sctlr & SCTLR_EL1_SPAN))
		pstate |= PSR_PAN_BIT;
	if (sctlr & SCTLR_ELx_DSSBS)
		pstate |= PSR_SSBS_BIT;
	return pstate | PSR_D_BIT | PSR_A_BIT | PSR_I_BIT | PSR_F_BIT |
	       target_mode;
}

#endif
