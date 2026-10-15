/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PMTHOR_GPU_H
#define _LINUX_PMTHOR_GPU_H

#include <linux/errno.h>
#include <linux/kconfig.h>

/*
 * Transfer a normal RAM PFN reference to the current process's GPU session,
 * binding the VM to that session on its first translation.
 * Zero consumes the reference; -EBUSY means it is already tracked, and all
 * negative returns leave the caller responsible for its extra reference.
 * References remain until both the VM and the successfully reset device
 * session have released their ownership, in either teardown order. Device
 * close cannot admit a new owner while a bound VM lives; the last VM teardown
 * resets the device again before releasing pages if the file closed first.
 */
struct pmthor_gpu_session;

#if IS_REACHABLE(CONFIG_PMTHOR)
int pmthor_gpu_track_pfn(struct pmthor_gpu_session **vm_session,
			 unsigned long pfn);
void pmthor_gpu_put_vm_session(struct pmthor_gpu_session *session);
#else
static inline int pmthor_gpu_track_pfn(struct pmthor_gpu_session **vm_session,
				      unsigned long pfn)
{
	return -ENODEV;
}

static inline void
pmthor_gpu_put_vm_session(struct pmthor_gpu_session *session)
{
}
#endif

#endif
