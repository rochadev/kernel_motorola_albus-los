/* Copyright (c) 2002,2007-2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include "adreno.h"
#include "kgsl_sharedmem.h"
#include "a3xx_reg.h"
#include "adreno_pm4types.h"

/**
 * _adreno_iommu_add_idle_cmds - Add pm4 packets for GPU idle
 * @adreno_dev - Pointer to device structure
 * @cmds - Pointer to memory where idle commands need to be added
 */
static inline int _adreno_iommu_add_idle_cmds(struct adreno_device *adreno_dev,
							unsigned int *cmds)
{
	unsigned int *start = cmds;

	cmds += cp_wait_for_idle(adreno_dev, cmds);

	if (adreno_is_a3xx(adreno_dev))
		cmds += cp_wait_for_me(adreno_dev, cmds);

	return cmds - start;
}

/*
 * _invalidate_uche_cpu() - Invalidate UCHE using CPU
 * @adreno_dev: the device
 */
static void _invalidate_uche_cpu(struct adreno_device *adreno_dev)
{
	/* Invalidate UCHE using CPU */
	if (adreno_is_a5xx(adreno_dev))
		adreno_writereg(adreno_dev,
			ADRENO_REG_UCHE_INVALIDATE0, 0x12);
	else if (adreno_is_a4xx(adreno_dev)) {
		adreno_writereg(adreno_dev,
			ADRENO_REG_UCHE_INVALIDATE0, 0);
		adreno_writereg(adreno_dev,
			ADRENO_REG_UCHE_INVALIDATE1, 0x12);
	} else if (adreno_is_a3xx(adreno_dev)) {
		adreno_writereg(adreno_dev,
			ADRENO_REG_UCHE_INVALIDATE0, 0);
		adreno_writereg(adreno_dev,
			ADRENO_REG_UCHE_INVALIDATE1,
			0x90000000);
	} else {
		BUG();
	}
}

/*
 * _ctx_switch_use_cpu_path() - Decide whether to use cpu path
 * @adreno_dev: the device
 * @new_pt: pagetable to switch
 * @rb: ringbuffer for ctx switch
 *
 * If we are idle and switching to default pagetable it is
 * preferable to poke the iommu directly rather than using the
 * GPU command stream.
 */
static bool _ctx_switch_use_cpu_path(
				struct adreno_device *adreno_dev,
				struct kgsl_pagetable *new_pt,
				struct adreno_ringbuffer *rb)
{
	/*
	 * If rb is current, we can use cpu path when GPU is
	 * idle and we are switching to default pt.
	 * If rb is not current, we can use cpu path when rb has no
	 * pending commands (rptr = wptr) and we are switching to default pt.
	 */
	if (adreno_dev->cur_rb == rb)
		return adreno_isidle(&adreno_dev->dev) &&
			(new_pt == adreno_dev->dev.mmu.defaultpagetable);
	else if ((rb->wptr == rb->rptr) &&
			(new_pt == adreno_dev->dev.mmu.defaultpagetable))
		return true;

	return false;
}

/**
 * adreno_iommu_set_apriv() - Generate commands to set/reset the APRIV
 * @adreno_dev: Device on which the commands will execute
 * @cmds: The memory pointer where commands are generated
 * @set: If set then APRIV is set else reset
 *
 * Returns the number of commands generated
 */
unsigned int adreno_iommu_set_apriv(struct adreno_device *adreno_dev,
				unsigned int *cmds, int set)
{
	unsigned int *cmds_orig = cmds;

	cmds += cp_wait_for_idle(adreno_dev, cmds);
	cmds += cp_wait_for_me(adreno_dev, cmds);
	*cmds++ = cp_register(adreno_dev, adreno_getreg(adreno_dev,
				ADRENO_REG_CP_CNTL), 1);
	if (set)
		*cmds++ = 1;
	else
		*cmds++ = 0;

	return cmds - cmds_orig;
}

/**
 * _adreno_iommu_wait_reg_mem() - Add a CP_WAIT_REG_MEM command
 * @cmds:	Pointer to memory where commands are to be added
 * @addr:	Register address to poll for
 * @val:	Value to poll for
 * @mask:	The value against which register value is masked
 * @interval:	wait interval
 */
static inline int _adreno_iommu_wait_reg_mem(struct adreno_device *adreno_dev,
				unsigned int *cmds, unsigned int addr,
				unsigned int val, unsigned int mask,
				unsigned int interval)
{
	unsigned int *start = cmds;
	*cmds++ = cp_mem_packet(adreno_dev, CP_WAIT_REG_MEM, 5, 1);
	*cmds++ = 0x3; /* Function = Equals */
	cmds += cp_gpuaddr(adreno_dev, cmds, addr); /* Poll address */
	*cmds++ = val; /* ref val */
	*cmds++ = mask;
	*cmds++ = interval;

	/* WAIT_REG_MEM turns back on protected mode - push it off */
	*cmds++ = cp_packet(adreno_dev, CP_SET_PROTECTED_MODE, 1);
	*cmds++ = 0;

	return cmds - start;
}

/**
 * _adreno_iommu_wait_reg_eq() - Add a CP_WAIT_REG_EQ command
 * @cmds:	Pointer to memory where commands are to be added
 * @addr:	Regiater address to poll for
 * @val:	Value to poll for
 * @mask:	The value against which register value is masked
 * @interval:	wait interval
 */
static inline int _adreno_iommu_wait_reg_eq(struct adreno_device *adreno_dev,
			unsigned int *cmds,
			unsigned int reg_addr, unsigned int val,
			unsigned int mask, unsigned int interval)
{
	unsigned int *start = cmds;
	*cmds++ = cp_packet(adreno_dev, CP_WAIT_REG_EQ, 4);
	*cmds++ = reg_addr;
	*cmds++ = val;
	*cmds++ = mask;
	*cmds++ = interval;
	return cmds - start;
}

static inline int _adreno_iommu_add_idle_indirect_cmds(
			struct adreno_device *adreno_dev,
			unsigned int *cmds, uint64_t nop_gpuaddr)
{
	unsigned int *start = cmds;
	/*
	 * Adding an indirect buffer ensures that the prefetch stalls until
	 * the commands in indirect buffer have completed. We need to stall
	 * prefetch with a nop indirect buffer when updating pagetables
	 * because it provides stabler synchronization */
	cmds += cp_wait_for_me(adreno_dev, cmds);
	*cmds++ = cp_mem_packet(adreno_dev, CP_INDIRECT_BUFFER_PFE, 2, 1);
	cmds += cp_gpuaddr(adreno_dev, cmds, nop_gpuaddr);
	*cmds++ = 2;
	cmds += cp_wait_for_idle(adreno_dev, cmds);
	return cmds - start;
}

/**
 * _adreno_mmu_set_pt_update_condition() - Generate commands to setup a
 * flag to indicate whether pt switch is required or not by comparing
 * current pt id and incoming pt id
 * @rb: The RB on which the commands will execute
 * @cmds: The pointer to memory where the commands are placed.
 * @ptname: Incoming pt id to set to
 *
 * Returns number of commands added.
 */
static unsigned int _adreno_mmu_set_pt_update_condition(
			struct adreno_ringbuffer *rb,
			unsigned int *cmds, unsigned int ptname)
{
	struct kgsl_device *device = rb->device;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	unsigned int *cmds_orig = cmds;
	/*
	 * write 1 to switch pt flag indicating that we need to execute the
	 * pt switch commands
	 */
	*cmds++ = cp_mem_packet(adreno_dev, CP_MEM_WRITE, 2, 1);
	cmds += cp_gpuaddr(adreno_dev, cmds, (rb->pagetable_desc.gpuaddr +
		offsetof(struct adreno_ringbuffer_pagetable_info,
		switch_pt_enable)));
	*cmds++ = 1;
	*cmds++ = cp_packet(adreno_dev, CP_WAIT_MEM_WRITES, 1);
	*cmds++ = 0;
	cmds += cp_wait_for_me(adreno_dev, cmds);
	if (ADRENO_FEATURE(adreno_dev, ADRENO_HAS_REG_TO_REG_CMDS)) {
		/* copy current ptid value to register SCRATCH_REG7 */
		*cmds++ = cp_mem_packet(adreno_dev, CP_MEM_TO_REG, 2, 1);
		*cmds++ = adreno_getreg(adreno_dev,
					ADRENO_REG_CP_SCRATCH_REG7);
		cmds += cp_gpuaddr(adreno_dev, cmds,
			 (adreno_dev->ringbuffers[0].pagetable_desc.gpuaddr +
			 offsetof(struct adreno_ringbuffer_pagetable_info,
			 current_global_ptname)));
		/* copy the incoming ptid to SCRATCH_REG6 */
		*cmds++ = cp_mem_packet(adreno_dev, CP_MEM_TO_REG, 2, 1);
		*cmds++ = adreno_getreg(adreno_dev,
					ADRENO_REG_CP_SCRATCH_REG6);
		cmds += cp_gpuaddr(adreno_dev, cmds,
			   (rb->pagetable_desc.gpuaddr +
			   offsetof(struct adreno_ringbuffer_pagetable_info,
			   incoming_ptname)));
		cmds += cp_wait_for_me(adreno_dev, cmds);
		/*
		 * compare the incoming ptid to current ptid and make the
		 * the pt switch commands optional based on condition
		 * that current_global_ptname(SCRATCH_REG7) ==
		 * incoming_ptid(SCRATCH_REG6)
		 */
		*cmds++ = cp_packet(adreno_dev, CP_COND_REG_EXEC, 3);
		*cmds++ = (2 << 28) | adreno_getreg(adreno_dev,
					ADRENO_REG_CP_SCRATCH_REG6);
		*cmds++ = adreno_getreg(adreno_dev,
					ADRENO_REG_CP_SCRATCH_REG7);
		*cmds++ = 7;
		/*
		 * if the incoming and current pt are equal then set the pt
		 * switch flag to 0 so that the pt switch commands will be
		 * skipped
		 */
		*cmds++ = cp_mem_packet(adreno_dev, CP_MEM_WRITE, 2, 1);
		cmds += cp_gpuaddr(adreno_dev, cmds, (rb->pagetable_desc.gpuaddr
			     + offsetof(struct adreno_ringbuffer_pagetable_info,
			     switch_pt_enable)));
		*cmds++ = 0;
	} else {
		/*
		 * same as if operation above except the current ptname is
		 * directly compared to the incoming pt id
		 */
		*cmds++ = cp_mem_packet(adreno_dev, CP_COND_WRITE, 6, 2);
		/* write to mem space, when a mem space is equal to ref val */
		*cmds++ = (1 << 8) | (1 << 4) | 3;
		cmds += cp_gpuaddr(adreno_dev, cmds,
			   (adreno_dev->ringbuffers[0].pagetable_desc.gpuaddr +
			   offsetof(struct adreno_ringbuffer_pagetable_info,
			   current_global_ptname)));
		*cmds++ = ptname;
		*cmds++ = 0xFFFFFFFF;
		cmds += cp_gpuaddr(adreno_dev, cmds,
			   (rb->pagetable_desc.gpuaddr +
			   offsetof(struct adreno_ringbuffer_pagetable_info,
			   switch_pt_enable)));
		*cmds++ = 0;
	}
	*cmds++ = cp_packet(adreno_dev, CP_WAIT_MEM_WRITES, 1);
	*cmds++ = 0;
	cmds += cp_wait_for_me(adreno_dev, cmds);

	return cmds - cmds_orig;
}

/**
 * _adreno_iommu_pt_update_pid_to_mem() - Add commands to write to memory the
 * pagetable id.
 * @rb: The ringbuffer on which these commands will execute
 * @cmds: Pointer to memory where the commands are copied
 * @ptname: The pagetable id
 */
static unsigned int _adreno_iommu_pt_update_pid_to_mem(
				struct adreno_ringbuffer *rb,
				unsigned int *cmds, int ptname)
{
	struct kgsl_device *device = rb->device;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	unsigned int *cmds_orig = cmds;

	if (ADRENO_FEATURE(adreno_dev, ADRENO_HAS_REG_TO_REG_CMDS)) {
		/* copy the incoming pt in memory to  SCRATCH_REG6 */
		*cmds++ = cp_mem_packet(adreno_dev, CP_MEM_TO_REG, 2, 1);
		*cmds++ = adreno_getreg(adreno_dev,
			ADRENO_REG_CP_SCRATCH_REG6);
		cmds += cp_gpuaddr(adreno_dev, cmds,
			  (rb->pagetable_desc.gpuaddr +
			  offsetof(struct adreno_ringbuffer_pagetable_info,
			  incoming_ptname)));
		/* copy the value in SCRATCH_REG6 to the current pt field */
		*cmds++ = cp_mem_packet(adreno_dev, CP_REG_TO_MEM, 2, 1);
		*cmds++ = adreno_getreg(adreno_dev,
			ADRENO_REG_CP_SCRATCH_REG6);
		cmds += cp_gpuaddr(adreno_dev, cmds,
			  (rb->pagetable_desc.gpuaddr +
			   offsetof(struct adreno_ringbuffer_pagetable_info,
			  current_rb_ptname)));
		*cmds++ = cp_mem_packet(adreno_dev, CP_REG_TO_MEM, 2, 1);
		*cmds++ = adreno_getreg(adreno_dev,
			ADRENO_REG_CP_SCRATCH_REG6);
		cmds += cp_gpuaddr(adreno_dev, cmds,
			  (adreno_dev->ringbuffers[0].pagetable_desc.gpuaddr +
			  offsetof(struct adreno_ringbuffer_pagetable_info,
			  current_global_ptname)));
	} else {
		*cmds++ = cp_mem_packet(adreno_dev, CP_MEM_WRITE, 2, 1);
		cmds += cp_gpuaddr(adreno_dev, cmds,
			  (rb->pagetable_desc.gpuaddr +
			  offsetof(struct adreno_ringbuffer_pagetable_info,
			  current_rb_ptname)));
		*cmds++ = ptname;

		*cmds++ = cp_mem_packet(adreno_dev, CP_MEM_WRITE, 2, 1);
		cmds += cp_gpuaddr(adreno_dev, cmds,
			  (adreno_dev->ringbuffers[0].pagetable_desc.gpuaddr +
			  offsetof(struct adreno_ringbuffer_pagetable_info,
			  current_global_ptname)));
		*cmds++ = ptname;
	}
	/* pagetable switch done, Housekeeping: set the switch_pt_enable to 0 */
	*cmds++ = cp_mem_packet(adreno_dev, CP_MEM_WRITE, 2, 1);
	cmds += cp_gpuaddr(adreno_dev, cmds,
			(rb->pagetable_desc.gpuaddr +
			offsetof(struct adreno_ringbuffer_pagetable_info,
			switch_pt_enable)));
	*cmds++ = 0;
	*cmds++ = cp_packet(adreno_dev, CP_WAIT_MEM_WRITES, 1);
	*cmds++ = 0;
	cmds += cp_wait_for_me(adreno_dev, cmds);

	return cmds - cmds_orig;
}

static unsigned int _adreno_iommu_set_pt_v1(struct adreno_ringbuffer *rb,
					unsigned int *cmds_orig,
					phys_addr_t pt_val,
					unsigned int ptname)
{
	struct kgsl_device *device = rb->device;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	uint64_t ttbr0_val = 0;
	unsigned int reg_pt_val;
	unsigned int *cmds = cmds_orig;
	unsigned int *cond_exec_ptr;
	unsigned int ttbr0, tlbiall, tlbstatus, tlbsync, mmu_ctrl;

	cmds += _adreno_iommu_add_idle_cmds(adreno_dev, cmds);

	/* set flag that indicates whether pt switch is required*/
	cmds += _adreno_mmu_set_pt_update_condition(rb, cmds, ptname);
	*cmds++ = cp_mem_packet(adreno_dev, CP_COND_EXEC, 4, 2);
	cmds += cp_gpuaddr(adreno_dev, cmds,
			(rb->pagetable_desc.gpuaddr +
			offsetof(struct adreno_ringbuffer_pagetable_info,
			switch_pt_enable)));
	cmds += cp_gpuaddr(adreno_dev, cmds,
			(rb->pagetable_desc.gpuaddr +
			offsetof(struct adreno_ringbuffer_pagetable_info,
			switch_pt_enable)));
	*cmds++ = 1;
	/* Exec count to be filled later */
	cond_exec_ptr = cmds;
	cmds++;

	if (ADRENO_FEATURE(adreno_dev, ADRENO_HAS_REG_TO_REG_CMDS)) {
		int count = 1;

		if (KGSL_IOMMU_CTX_TTBR0_ADDR_MASK &
			0xFFFFFFFF00000000ULL)
			count = 2;
		/* transfer the ttbr0 value to ME_SCRATCH */
		*cmds++ = cp_mem_packet(adreno_dev, CP_MEM_TO_REG, 2, 1);
		*cmds++ = count << 16 | adreno_getreg(adreno_dev,
				ADRENO_REG_CP_SCRATCH_REG6);
		cmds += cp_gpuaddr(adreno_dev, cmds,
			   (rb->pagetable_desc.gpuaddr +
			   offsetof(struct adreno_ringbuffer_pagetable_info,
			   ttbr0) + sizeof(uint64_t)));
		cmds += cp_wait_for_idle(adreno_dev, cmds);
		*cmds++ = cp_packet(adreno_dev, CP_REG_TO_SCRATCH, 1);
		*cmds++ = (count << 24) | (6 << 16) |
			adreno_getreg(adreno_dev,
					ADRENO_REG_CP_SCRATCH_REG6);
	} else {
		ttbr0_val = kgsl_mmu_get_default_ttbr0(&device->mmu,
			KGSL_IOMMU_CONTEXT_USER);
		ttbr0_val &= ~KGSL_IOMMU_CTX_TTBR0_ADDR_MASK;
		ttbr0_val |= (pt_val & KGSL_IOMMU_CTX_TTBR0_ADDR_MASK);
	}
	mmu_ctrl = kgsl_mmu_get_reg_ahbaddr(
		&device->mmu, KGSL_IOMMU_CONTEXT_USER,
		KGSL_IOMMU_IMPLDEF_MICRO_MMU_CTRL) >> 2;

	ttbr0 = kgsl_mmu_get_reg_ahbaddr(&device->mmu, KGSL_IOMMU_CONTEXT_USER,
					KGSL_IOMMU_CTX_TTBR0) >> 2;

	if (kgsl_mmu_hw_halt_supported(&device->mmu)) {
		cmds += cp_wait_for_idle(adreno_dev, cmds);
		/*
		 * glue commands together until next
		 * WAIT_FOR_ME
		 */
		if (adreno_is_a4xx(adreno_dev))
			cmds += _adreno_iommu_wait_reg_mem(adreno_dev, cmds,
			adreno_getreg(adreno_dev, ADRENO_REG_CP_WFI_PEND_CTR),
				1, 0xFFFFFFFF, 0xF);
		else
			cmds += _adreno_iommu_wait_reg_eq(adreno_dev, cmds,
			adreno_getreg(adreno_dev, ADRENO_REG_CP_WFI_PEND_CTR),
				1, 0xFFFFFFFF, 0xF);

		/* set the iommu lock bit */
		*cmds++ = cp_packet(adreno_dev, CP_REG_RMW, 3);
		*cmds++ = mmu_ctrl;
		/* AND to unmask the lock bit */
		*cmds++ = ~(KGSL_IOMMU_IMPLDEF_MICRO_MMU_CTRL_HALT);
		/* OR to set the IOMMU lock bit */
		*cmds++ = KGSL_IOMMU_IMPLDEF_MICRO_MMU_CTRL_HALT;
		/* wait for smmu to lock */
		if (adreno_is_a4xx(adreno_dev))
			cmds += _adreno_iommu_wait_reg_mem(adreno_dev,
				cmds, mmu_ctrl,
				KGSL_IOMMU_IMPLDEF_MICRO_MMU_CTRL_IDLE,
				KGSL_IOMMU_IMPLDEF_MICRO_MMU_CTRL_IDLE, 0xF);
		else
			cmds += _adreno_iommu_wait_reg_eq(adreno_dev, cmds,
				mmu_ctrl,
				KGSL_IOMMU_IMPLDEF_MICRO_MMU_CTRL_IDLE,
				KGSL_IOMMU_IMPLDEF_MICRO_MMU_CTRL_IDLE, 0xF);
	}
	if (ADRENO_FEATURE(adreno_dev, ADRENO_HAS_REG_TO_REG_CMDS)) {
		/* ME_SCRATCH_REG to REG copy */
		*cmds++ = cp_packet(adreno_dev, CP_SCRATCH_TO_REG, 1);
		if (KGSL_IOMMU_CTX_TTBR0_ADDR_MASK & 0xFFFFFFFF00000000ULL)
			*cmds++ = (2 << 24) | (6 << 16) | ttbr0;
		else
			*cmds++ = (1 << 24) | (6 << 16) | ttbr0;
	} else {
		/*
		 * set ttbr0, only need to set the higer bits if the
		 * address bits lie in the higher bits
		 */
		if (KGSL_IOMMU_CTX_TTBR0_ADDR_MASK & 0xFFFFFFFF00000000ULL) {
			reg_pt_val = (unsigned int)ttbr0_val & 0xFFFFFFFF;
			*cmds++ = cp_register(adreno_dev, ttbr0, 1);
			*cmds++ = reg_pt_val;
			reg_pt_val = (unsigned int)
				((ttbr0_val & 0xFFFFFFFF00000000ULL) >> 32);
			*cmds++ = cp_register(adreno_dev, ttbr0 + 1, 1);
			*cmds++ = reg_pt_val;
		} else {
			reg_pt_val = ttbr0_val;
			*cmds++ = cp_register(adreno_dev, ttbr0, 1);
			*cmds++ = reg_pt_val;
		}
	}
	if (kgsl_mmu_hw_halt_supported(&device->mmu) &&
		adreno_is_a3xx(adreno_dev)) {
		/* unlock the IOMMU lock */
		*cmds++ = cp_packet(adreno_dev, CP_REG_RMW, 3);
		*cmds++ = mmu_ctrl;
		/* AND to unmask the lock bit */
		*cmds++ = ~(KGSL_IOMMU_IMPLDEF_MICRO_MMU_CTRL_HALT);
		/* OR with 0 so lock bit is unset */
		*cmds++ = 0;
		/* release all commands with wait_for_me */
		cmds += cp_wait_for_me(adreno_dev, cmds);
	}
	tlbiall = kgsl_mmu_get_reg_ahbaddr(&device->mmu,
				KGSL_IOMMU_CONTEXT_USER,
				KGSL_IOMMU_CTX_TLBIALL) >> 2;
	*cmds++ = cp_register(adreno_dev, tlbiall, 1);
	*cmds++ = 1;

	tlbsync = kgsl_mmu_get_reg_ahbaddr(&device->mmu,
				KGSL_IOMMU_CONTEXT_USER,
				KGSL_IOMMU_CTX_TLBSYNC) >> 2;
	*cmds++ = cp_register(adreno_dev, tlbsync, 1);
	*cmds++ = 0;

	tlbstatus = kgsl_mmu_get_reg_ahbaddr(&device->mmu,
			KGSL_IOMMU_CONTEXT_USER,
			KGSL_IOMMU_CTX_TLBSTATUS) >> 2;
	if (adreno_is_a4xx(adreno_dev))
		cmds += _adreno_iommu_wait_reg_mem(adreno_dev,
			cmds, tlbstatus, 0,
			KGSL_IOMMU_CTX_TLBSTATUS_SACTIVE, 0xF);
	else
		cmds += _adreno_iommu_wait_reg_eq(adreno_dev,
			cmds, tlbstatus, 0,
			KGSL_IOMMU_CTX_TLBSTATUS_SACTIVE, 0xF);

	if (kgsl_mmu_hw_halt_supported(&device->mmu) &&
		!adreno_is_a3xx(adreno_dev)) {
		/* unlock the IOMMU lock */
		*cmds++ = cp_packet(adreno_dev, CP_REG_RMW, 3);
		*cmds++ = mmu_ctrl;
		/* AND to unmask the lock bit */
		*cmds++ = ~(KGSL_IOMMU_IMPLDEF_MICRO_MMU_CTRL_HALT);
		/* OR with 0 so lock bit is unset */
		*cmds++ = 0;
	}
	/* release all commands with wait_for_me */
	cmds += cp_wait_for_me(adreno_dev, cmds);

	/* Exec count ordinal of CP_COND_EXEC packet */
	*cond_exec_ptr = (cmds - cond_exec_ptr - 1);
	cmds += _adreno_iommu_add_idle_cmds(adreno_dev, cmds);
	cmds += _adreno_iommu_pt_update_pid_to_mem(rb, cmds, ptname);

	return cmds - cmds_orig;
}


static unsigned int _adreno_iommu_set_pt_v2_a3xx(struct kgsl_device *device,
					unsigned int *cmds_orig,
					phys_addr_t pt_val)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	uint64_t ttbr0_val;
	unsigned int reg_pt_val;
	unsigned int *cmds = cmds_orig;
	unsigned int ttbr0, tlbiall, tlbstatus, tlbsync;

	cmds += _adreno_iommu_add_idle_cmds(adreno_dev, cmds);

	ttbr0_val = kgsl_mmu_get_default_ttbr0(&device->mmu,
			KGSL_IOMMU_CONTEXT_USER);
	ttbr0_val &= ~KGSL_IOMMU_CTX_TTBR0_ADDR_MASK;
	ttbr0_val |= (pt_val & KGSL_IOMMU_CTX_TTBR0_ADDR_MASK);
	ttbr0 = kgsl_mmu_get_reg_ahbaddr(&device->mmu, KGSL_IOMMU_CONTEXT_USER,
				KGSL_IOMMU_CTX_TTBR0) >> 2;

	/*
	 * glue commands together until next
	 * WAIT_FOR_ME
	 */
	cmds += _adreno_iommu_wait_reg_eq(adreno_dev, cmds,
			adreno_getreg(adreno_dev, ADRENO_REG_CP_WFI_PEND_CTR),
			1, 0xFFFFFFFF, 0xF);

	/* MMU-500 VBIF stall */
	*cmds++ = cp_packet(adreno_dev, CP_REG_RMW, 3);
	*cmds++ = A3XX_VBIF_DDR_OUTPUT_RECOVERABLE_HALT_CTRL0;
	/* AND to unmask the HALT bit */
	*cmds++ = ~(VBIF_RECOVERABLE_HALT_CTRL);
	/* OR to set the HALT bit */
	*cmds++ = 0x1;

	/* Wait for acknowledgement */
	cmds += _adreno_iommu_wait_reg_eq(adreno_dev, cmds,
			A3XX_VBIF_DDR_OUTPUT_RECOVERABLE_HALT_CTRL1,
			1, 0xFFFFFFFF, 0xF);

	/* set ttbr0 */
	if (KGSL_IOMMU_CTX_TTBR0_ADDR_MASK & 0xFFFFFFFF00000000ULL) {
		reg_pt_val = (unsigned int)ttbr0_val & 0xFFFFFFFF;
		*cmds++ = cp_packet(adreno_dev, CP_REG_WR_NO_CTXT, 2);
		*cmds++ = ttbr0;
		*cmds++ = reg_pt_val;
		reg_pt_val = (unsigned int)
			((ttbr0_val & 0xFFFFFFFF00000000ULL) >> 32);
		*cmds++ = cp_packet(adreno_dev, CP_REG_WR_NO_CTXT, 2);
		*cmds++ = ttbr0 + 1;
		*cmds++ = reg_pt_val;
	} else {
		reg_pt_val = ttbr0_val;
		*cmds++ = cp_packet(adreno_dev, CP_REG_WR_NO_CTXT, 2);
		*cmds++ = ttbr0;
		*cmds++ = reg_pt_val;
	}

	/* MMU-500 VBIF unstall */
	*cmds++ = cp_packet(adreno_dev, CP_REG_RMW, 3);
	*cmds++ = A3XX_VBIF_DDR_OUTPUT_RECOVERABLE_HALT_CTRL0;
	/* AND to unmask the HALT bit */
	*cmds++ = ~(VBIF_RECOVERABLE_HALT_CTRL);
	/* OR to reset the HALT bit */
	*cmds++ = 0;

	/* release all commands with wait_for_me */
	cmds += cp_wait_for_me(adreno_dev, cmds);

	tlbiall = kgsl_mmu_get_reg_ahbaddr(&device->mmu,
				KGSL_IOMMU_CONTEXT_USER,
				KGSL_IOMMU_CTX_TLBIALL) >> 2;
	*cmds++ = cp_packet(adreno_dev, CP_REG_WR_NO_CTXT, 2);
	*cmds++ = tlbiall;
	*cmds++ = 1;

	tlbsync = kgsl_mmu_get_reg_ahbaddr(&device->mmu,
				KGSL_IOMMU_CONTEXT_USER,
				KGSL_IOMMU_CTX_TLBSYNC) >> 2;
	*cmds++ = cp_packet(adreno_dev, CP_REG_WR_NO_CTXT, 2);
	*cmds++ = tlbsync;
	*cmds++ = 0;

	tlbstatus = kgsl_mmu_get_reg_ahbaddr(&device->mmu,
			KGSL_IOMMU_CONTEXT_USER, KGSL_IOMMU_CTX_TLBSTATUS) >> 2;
	cmds += _adreno_iommu_wait_reg_eq(adreno_dev, cmds, tlbstatus, 0,
			KGSL_IOMMU_CTX_TLBSTATUS_SACTIVE, 0xF);
		/* release all commands with wait_for_me */
	cmds += cp_wait_for_me(adreno_dev, cmds);

	cmds += _adreno_iommu_add_idle_cmds(adreno_dev, cmds);

	return cmds - cmds_orig;
}

static unsigned int _adreno_iommu_set_pt_v2_a4xx(struct kgsl_device *device,
					unsigned int *cmds_orig,
					phys_addr_t pt_val)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	uint64_t ttbr0_val;
	unsigned int reg_pt_val;
	unsigned int *cmds = cmds_orig;
	unsigned int ttbr0, tlbiall, tlbstatus, tlbsync;

	cmds += _adreno_iommu_add_idle_cmds(adreno_dev, cmds);

	ttbr0_val = kgsl_mmu_get_default_ttbr0(&device->mmu,
			KGSL_IOMMU_CONTEXT_USER);
	ttbr0_val &= ~KGSL_IOMMU_CTX_TTBR0_ADDR_MASK;
	ttbr0_val |= (pt_val & KGSL_IOMMU_CTX_TTBR0_ADDR_MASK);
	ttbr0 = kgsl_mmu_get_reg_ahbaddr(&device->mmu, KGSL_IOMMU_CONTEXT_USER,
				KGSL_IOMMU_CTX_TTBR0) >> 2;

	/*
	 * glue commands together until next
	 * WAIT_FOR_ME
	 */
	cmds += _adreno_iommu_wait_reg_mem(adreno_dev, cmds,
			adreno_getreg(adreno_dev, ADRENO_REG_CP_WFI_PEND_CTR),
			1, 0xFFFFFFFF, 0xF);

	/* MMU-500 VBIF stall */
	*cmds++ = cp_packet(adreno_dev, CP_REG_RMW, 3);
	*cmds++ = A3XX_VBIF_DDR_OUTPUT_RECOVERABLE_HALT_CTRL0;
	/* AND to unmask the HALT bit */
	*cmds++ = ~(VBIF_RECOVERABLE_HALT_CTRL);
	/* OR to set the HALT bit */
	*cmds++ = 0x1;

	/* Wait for acknowledgement */
	cmds += _adreno_iommu_wait_reg_mem(adreno_dev, cmds,
		A3XX_VBIF_DDR_OUTPUT_RECOVERABLE_HALT_CTRL1,
			1, 0xFFFFFFFF, 0xF);

	/* set ttbr0 */
	if (sizeof(phys_addr_t) > sizeof(unsigned int)) {

		reg_pt_val = ttbr0_val & 0xFFFFFFFF;
		*cmds++ = cp_packet(adreno_dev, CP_WIDE_REG_WRITE, 2);
		*cmds++ = ttbr0;
		*cmds++ = reg_pt_val;

		reg_pt_val = (unsigned int)((ttbr0_val &
			0xFFFFFFFF00000000ULL) >> 32);
		*cmds++ = cp_packet(adreno_dev, CP_WIDE_REG_WRITE, 2);
		*cmds++ = ttbr0+1;
		*cmds++ = reg_pt_val;
	} else {
		reg_pt_val = ttbr0_val;
		*cmds++ = cp_packet(adreno_dev, CP_WIDE_REG_WRITE, 2);
		*cmds++ = ttbr0;
		*cmds++ = reg_pt_val;
	}

	/* MMU-500 VBIF unstall */
	*cmds++ = cp_packet(adreno_dev, CP_REG_RMW, 3);
	*cmds++ = A3XX_VBIF_DDR_OUTPUT_RECOVERABLE_HALT_CTRL0;
	/* AND to unmask the HALT bit */
	*cmds++ = ~(VBIF_RECOVERABLE_HALT_CTRL);
	/* OR to reset the HALT bit */
	*cmds++ = 0;

	/* release all commands with wait_for_me */
	cmds += cp_wait_for_me(adreno_dev, cmds);

	tlbiall = kgsl_mmu_get_reg_ahbaddr(&device->mmu,
				KGSL_IOMMU_CONTEXT_USER,
				KGSL_IOMMU_CTX_TLBIALL) >> 2;

	*cmds++ = cp_packet(adreno_dev, CP_WIDE_REG_WRITE, 2);
	*cmds++ = tlbiall;
	*cmds++ = 1;

	tlbsync = kgsl_mmu_get_reg_ahbaddr(&device->mmu,
				KGSL_IOMMU_CONTEXT_USER,
				KGSL_IOMMU_CTX_TLBSYNC) >> 2;

	*cmds++ = cp_packet(adreno_dev, CP_WIDE_REG_WRITE, 2);
	*cmds++ = tlbsync;
	*cmds++ = 0;

	tlbstatus = kgsl_mmu_get_reg_ahbaddr(&device->mmu,
			KGSL_IOMMU_CONTEXT_USER,
			KGSL_IOMMU_CTX_TLBSTATUS) >> 2;
	cmds += _adreno_iommu_wait_reg_mem(adreno_dev, cmds, tlbstatus, 0,
			KGSL_IOMMU_CTX_TLBSTATUS_SACTIVE, 0xF);
	/* release all commands with wait_for_me */
	cmds += cp_wait_for_me(adreno_dev, cmds);

	cmds += _adreno_iommu_add_idle_cmds(adreno_dev, cmds);

	return cmds - cmds_orig;
}

static unsigned int _adreno_iommu_set_pt_v2_a5xx(struct kgsl_device *device,
					unsigned int *cmds_orig,
					phys_addr_t pt_val)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	uint64_t ttbr0_val;
	unsigned int reg_pt_val;
	unsigned int *cmds = cmds_orig;
	unsigned int ttbr0, tlbiall, tlbstatus, tlbsync, mmu_ctrl;

	cmds += _adreno_iommu_add_idle_cmds(adreno_dev, cmds);

	ttbr0_val = kgsl_mmu_get_default_ttbr0(&device->mmu,
			KGSL_IOMMU_CONTEXT_USER);
	ttbr0_val &= ~KGSL_IOMMU_CTX_TTBR0_ADDR_MASK;
	ttbr0_val |= (pt_val & KGSL_IOMMU_CTX_TTBR0_ADDR_MASK);
	ttbr0 = kgsl_mmu_get_reg_ahbaddr(&device->mmu, KGSL_IOMMU_CONTEXT_USER,
				KGSL_IOMMU_CTX_TTBR0) >> 2;

	mmu_ctrl = kgsl_mmu_get_reg_ahbaddr(
	   &device->mmu, KGSL_IOMMU_CONTEXT_USER,
	   KGSL_IOMMU_IMPLDEF_MICRO_MMU_CTRL) >> 2;

	/*
	 * glue commands together until next
	 * WAIT_FOR_ME
	 */
	cmds += _adreno_iommu_wait_reg_mem(adreno_dev, cmds,
			adreno_getreg(adreno_dev, ADRENO_REG_CP_WFI_PEND_CTR),
			1, 0xFFFFFFFF, 0xF);

	/* set the iommu lock bit */
	*cmds++ = cp_packet(adreno_dev, CP_REG_RMW, 3);
	*cmds++ = mmu_ctrl;
	/* AND to unmask the lock bit */
	*cmds++ = ~(KGSL_IOMMU_IMPLDEF_MICRO_MMU_CTRL_HALT);
	/* OR to set the IOMMU lock bit */
	*cmds++ = KGSL_IOMMU_IMPLDEF_MICRO_MMU_CTRL_HALT;
	/* wait for smmu to lock */
	cmds += _adreno_iommu_wait_reg_mem(adreno_dev, cmds, mmu_ctrl,
				KGSL_IOMMU_IMPLDEF_MICRO_MMU_CTRL_IDLE,
				KGSL_IOMMU_IMPLDEF_MICRO_MMU_CTRL_IDLE, 0xF);

	/* set ttbr0 */
	if (sizeof(phys_addr_t) > sizeof(unsigned int)) {

		reg_pt_val = ttbr0_val & 0xFFFFFFFF;
		*cmds++ = cp_register(adreno_dev, ttbr0, 1);
		*cmds++ = reg_pt_val;

		reg_pt_val = (unsigned int)((ttbr0_val &
			0xFFFFFFFF00000000ULL) >> 32);
		*cmds++ = cp_register(adreno_dev, ttbr0+1, 1);
		*cmds++ = reg_pt_val;
	} else {
		reg_pt_val = ttbr0_val;
		*cmds++ = cp_register(adreno_dev, ttbr0, 1);
		*cmds++ = reg_pt_val;
	}

	/* unlock the IOMMU lock */
	*cmds++ = cp_packet(adreno_dev, CP_REG_RMW, 3);
	*cmds++ = mmu_ctrl;
	/* AND to unmask the lock bit */
	*cmds++ = ~(KGSL_IOMMU_IMPLDEF_MICRO_MMU_CTRL_HALT);
	/* OR with 0 so lock bit is unset */
	*cmds++ = 0;

	tlbiall = kgsl_mmu_get_reg_ahbaddr(&device->mmu,
				KGSL_IOMMU_CONTEXT_USER,
				KGSL_IOMMU_CTX_TLBIALL) >> 2;
	*cmds++ = cp_register(adreno_dev, tlbiall, 1);
	*cmds++ = 1;

	tlbsync = kgsl_mmu_get_reg_ahbaddr(&device->mmu,
				KGSL_IOMMU_CONTEXT_USER,
				KGSL_IOMMU_CTX_TLBSYNC) >> 2;
	*cmds++ = cp_register(adreno_dev, tlbsync, 1);
	*cmds++ = 0;

	tlbstatus = kgsl_mmu_get_reg_ahbaddr(&device->mmu,
			KGSL_IOMMU_CONTEXT_USER,
			KGSL_IOMMU_CTX_TLBSTATUS) >> 2;
	cmds += _adreno_iommu_wait_reg_mem(adreno_dev,
			cmds, tlbstatus, 0,
			KGSL_IOMMU_CTX_TLBSTATUS_SACTIVE, 0xF);

	/* release all commands with wait_for_me */
	cmds += cp_wait_for_me(adreno_dev, cmds);

	cmds += _adreno_iommu_add_idle_cmds(adreno_dev, cmds);

	return cmds - cmds_orig;
}

/**
 * adreno_iommu_set_pt_generate_cmds() - Generate commands to change pagetable
 * @rb: The RB pointer in which these commaands are to be submitted
 * @cmds: The pointer where the commands are placed
 * @pt: The pagetable to switch to
 */
unsigned int adreno_iommu_set_pt_generate_cmds(
					struct adreno_ringbuffer *rb,
					unsigned int *cmds,
					struct kgsl_pagetable *pt)
{
	struct kgsl_device *device = rb->device;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	phys_addr_t pt_val;
	unsigned int *cmds_orig = cmds;

	/* If we are in a fault the MMU will be reset soon */
	if (test_bit(ADRENO_DEVICE_FAULT, &adreno_dev->priv))
		return 0;

	pt_val = kgsl_mmu_get_pt_base_addr(&device->mmu, pt);

	cmds += _adreno_iommu_add_idle_indirect_cmds(adreno_dev, cmds,
		device->mmu.setstate_memory.gpuaddr +
		KGSL_IOMMU_SETSTATE_NOP_OFFSET);

	if (kgsl_msm_supports_iommu_v2())
		if (adreno_is_a5xx(adreno_dev))
			cmds += _adreno_iommu_set_pt_v2_a5xx(device, cmds,
						pt_val);
		else if (adreno_is_a4xx(adreno_dev))
			cmds += _adreno_iommu_set_pt_v2_a4xx(device, cmds,
						pt_val);
		else
			cmds += _adreno_iommu_set_pt_v2_a3xx(device, cmds,
						pt_val);
	else
		cmds += _adreno_iommu_set_pt_v1(rb, cmds, pt_val, pt->name);

	/* invalidate all base pointers */
	cmds += cp_invalidate_state(adreno_dev, cmds);

	return cmds - cmds_orig;
}

/**
 * adreno_iommu_set_pt_ib() - Generate commands to swicth pagetable. The
 * commands generated use an IB
 * @rb: The RB in which the commands will be executed
 * @cmds: Memory pointer where commands are generated
 * @pt: The pagetable to switch to
 */
unsigned int adreno_iommu_set_pt_ib(struct adreno_ringbuffer *rb,
				unsigned int *cmds,
				struct kgsl_pagetable *pt)
{
	struct kgsl_device *device = rb->device;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	unsigned int *cmds_orig = cmds;
	phys_addr_t pt_val;
	uint64_t ttbr0_val;

	pt_val = kgsl_mmu_get_pt_base_addr(&(rb->device->mmu), pt);

	/* put the ptname in pagetable desc */
	*cmds++ = cp_mem_packet(adreno_dev, CP_MEM_WRITE, 2, 1);
	cmds += cp_gpuaddr(adreno_dev, cmds,
			(rb->pagetable_desc.gpuaddr +
			offsetof(struct adreno_ringbuffer_pagetable_info,
			incoming_ptname)));
	*cmds++ = pt->name;
	/* Write the ttbr0 value to pagetable desc memory */
	ttbr0_val = kgsl_mmu_get_default_ttbr0(&device->mmu,
			KGSL_IOMMU_CONTEXT_USER);
	ttbr0_val &= ~KGSL_IOMMU_CTX_TTBR0_ADDR_MASK;
	ttbr0_val |= (pt_val & KGSL_IOMMU_CTX_TTBR0_ADDR_MASK);

	if (KGSL_IOMMU_CTX_TTBR0_ADDR_MASK & 0xFFFFFFFF00000000ULL) {
		*cmds++ =  cp_mem_packet(adreno_dev, CP_MEM_WRITE, 2, 1);
		cmds += cp_gpuaddr(adreno_dev, cmds,
			   (rb->pagetable_desc.gpuaddr +
			   offsetof(struct adreno_ringbuffer_pagetable_info,
			   ttbr0) + sizeof(uint64_t)));
		*cmds++ = ttbr0_val & 0xFFFFFFFF;
		*cmds++ = cp_mem_packet(adreno_dev, CP_MEM_WRITE, 2, 1);
		cmds += cp_gpuaddr(adreno_dev, cmds,
			   (rb->pagetable_desc.gpuaddr +
			   offsetof(struct adreno_ringbuffer_pagetable_info,
			   ttbr0) + sizeof(uint64_t) + sizeof(unsigned int)));
		*cmds++ = ttbr0_val >> 32;
	} else {
		*cmds++ = cp_mem_packet(adreno_dev, CP_MEM_WRITE, 2, 1);
		cmds += cp_gpuaddr(adreno_dev, cmds,
			   (rb->pagetable_desc.gpuaddr +
			   offsetof(struct adreno_ringbuffer_pagetable_info,
			   ttbr0) + sizeof(uint64_t)));
		*cmds++ = ttbr0_val & 0xFFFFFFFF;
	}

	*cmds++ = cp_packet(adreno_dev, CP_WAIT_MEM_WRITES, 1);
	*cmds++ = 0;
	cmds += cp_wait_for_me(adreno_dev, cmds);
	*cmds++ = cp_mem_packet(adreno_dev, CP_INDIRECT_BUFFER_PFE, 2, 1);
	cmds += cp_gpuaddr(adreno_dev, cmds, rb->pt_update_desc.gpuaddr);
	*cmds++ = rb->pt_update_desc.size / sizeof(unsigned int);

	return cmds - cmds_orig;
}

/**
 * __add_curr_ctxt_cmds() - Add commands to set a context id in memstore
 * @rb: The RB in which the commands will be added for execution
 * @cmds: Pointer to memory where commands are added
 * @drawctxt: The context whose id is being set in memstore
 *
 * Returns the number of dwords
 */
static unsigned int __add_curr_ctxt_cmds(struct adreno_ringbuffer *rb,
			unsigned int *cmds,
			struct adreno_context *drawctxt)
{
	unsigned int *cmds_orig = cmds;
	struct kgsl_device *device = rb->device;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);

	/* write the context identifier to memstore memory */
	*cmds++ = cp_packet(adreno_dev, CP_NOP, 1);
	*cmds++ = KGSL_CONTEXT_TO_MEM_IDENTIFIER;

	*cmds++ = cp_mem_packet(adreno_dev, CP_MEM_WRITE, 2, 1);
	cmds += cp_gpuaddr(adreno_dev, cmds, device->memstore.gpuaddr +
			   KGSL_MEMSTORE_RB_OFFSET(rb, current_context));
	*cmds++ = (drawctxt ? drawctxt->base.id : 0);

	*cmds++ = cp_mem_packet(adreno_dev, CP_MEM_WRITE, 2, 1);
	cmds += cp_gpuaddr(adreno_dev, cmds, device->memstore.gpuaddr +
			KGSL_MEMSTORE_OFFSET(KGSL_MEMSTORE_GLOBAL,
			current_context));
	*cmds++ = (drawctxt ? drawctxt->base.id : 0);

	/* Invalidate UCHE for new context */
	if (adreno_is_a5xx(adreno_dev)) {
		*cmds++ = cp_register(adreno_dev,
			adreno_getreg(adreno_dev,
		ADRENO_REG_UCHE_INVALIDATE0), 1);
		*cmds++ = 0x12;
	} else if (adreno_is_a4xx(adreno_dev)) {
		*cmds++ = cp_register(adreno_dev,
			adreno_getreg(adreno_dev,
			ADRENO_REG_UCHE_INVALIDATE0), 2);
		*cmds++ = 0;
		*cmds++ = 0x12;
	} else if (adreno_is_a3xx(adreno_dev)) {
		*cmds++ = cp_register(adreno_dev,
			adreno_getreg(adreno_dev,
			ADRENO_REG_UCHE_INVALIDATE0), 2);
		*cmds++ = 0;
		*cmds++ = 0x90000000;
	} else
		BUG();

	return cmds - cmds_orig;
}

/*
 * _set_ctxt_cpu() - Set the current context in memstore
 * @rb: The ringbuffer memstore to set curr context
 * @drawctxt: The context whose id is being set in memstore
 */
static void _set_ctxt_cpu(struct adreno_ringbuffer *rb,
			struct adreno_context *drawctxt)
{
	struct kgsl_device *device = rb->device;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);

	if (rb == adreno_dev->cur_rb) {
		_invalidate_uche_cpu(adreno_dev);
		/* Update global memstore with current context */
		kgsl_sharedmem_writel(device, &device->memstore,
			KGSL_MEMSTORE_OFFSET(KGSL_MEMSTORE_GLOBAL,
						current_context),
			drawctxt ? drawctxt->base.id : 0);
	}
	/* Update rb memstore with current context */
	kgsl_sharedmem_writel(device, &device->memstore,
		KGSL_MEMSTORE_RB_OFFSET(rb, current_context),
		drawctxt ? drawctxt->base.id : 0);
}

/**
 * _set_ctxt_gpu() - Add commands to set the current context in memstore
 * @rb: The ringbuffer in which commands to set memstore are added
 * @drawctxt: The context whose id is being set in memstore
 */
static int _set_ctxt_gpu(struct adreno_ringbuffer *rb,
			struct adreno_context *drawctxt)
{
	unsigned int link[15], *cmds;
	int result;

	cmds = &link[0];
	cmds += __add_curr_ctxt_cmds(rb, cmds, drawctxt);
	result = adreno_ringbuffer_issuecmds(rb, 0, link,
			(unsigned int)(cmds - link));
	return result;
}

/**
 * _set_pagetable_cpu() - Use CPU to switch the pagetable
 * @rb: The rb for which pagetable needs to be switched
 * @new_pt: The pagetable to switch to
 */
int _set_pagetable_cpu(struct adreno_ringbuffer *rb,
			struct kgsl_pagetable *new_pt)
{
	struct kgsl_device *device = rb->device;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	int result;

	/* update TTBR0 only if we are updating current RB */
	if (adreno_dev->cur_rb == rb) {
		result = kgsl_mmu_set_pt(&device->mmu, new_pt);
		if (result)
			return result;
		/* write the new pt set to memory var */
		kgsl_sharedmem_writel(device,
			&adreno_dev->ringbuffers[0].pagetable_desc,
			offsetof(
			struct adreno_ringbuffer_pagetable_info,
			current_global_ptname), new_pt->name);
	}

	/* Update the RB pagetable here */
	kgsl_sharedmem_writel(device, &rb->pagetable_desc,
		offsetof(
		struct adreno_ringbuffer_pagetable_info,
		current_rb_ptname), new_pt->name);

	return 0;
}

/**
 * _set_pagetable_gpu() - Use GPU to switch the pagetable
 * @rb: The rb in which commands to switch pagetable are to be
 *    submitted
 * @new_pt: The pagetable to switch to
 */
int _set_pagetable_gpu(struct adreno_ringbuffer *rb,
			struct kgsl_pagetable *new_pt)
{
	unsigned int *link = NULL, *cmds;
	struct kgsl_device *device = rb->device;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	int result;

	link = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (link == NULL) {
		result = -ENOMEM;
		goto done;
	}

	cmds = link;

	kgsl_mmu_enable_clk(&device->mmu);

	/* pt switch may use privileged memory */
	if (adreno_is_a4xx(adreno_dev))
		cmds += adreno_iommu_set_apriv(adreno_dev, cmds, 1);

	if (ADRENO_FEATURE(adreno_dev, ADRENO_HAS_REG_TO_REG_CMDS))
		cmds += adreno_iommu_set_pt_ib(rb, cmds, new_pt);
	else
		cmds += adreno_iommu_set_pt_generate_cmds(rb, cmds, new_pt);

	if (adreno_is_a4xx(adreno_dev))
		cmds += adreno_iommu_set_apriv(adreno_dev, cmds, 0);

	if ((unsigned int) (cmds - link) > (PAGE_SIZE / sizeof(unsigned int))) {
		KGSL_DRV_ERR(device, "Temp command buffer overflow\n");
		BUG();
	}
	/*
	 * This returns the per context timestamp but we need to
	 * use the global timestamp for iommu clock disablement
	 */
	result = adreno_ringbuffer_issuecmds(rb,
			KGSL_CMD_FLAGS_PMODE, link,
			(unsigned int)(cmds - link));

	/*
	 * On error disable the IOMMU clock right away otherwise turn it off
	 * after the command has been retired
	 */
	if (result)
		kgsl_mmu_disable_clk(&device->mmu);
	else
		adreno_ringbuffer_mmu_disable_clk_on_ts(device, rb,
						rb->timestamp);

done:
	kfree(link);
	return result;
}

/**
 * adreno_iommu_init() - Adreno iommu init
 * @adreno_dev: Adreno device
 */
void adreno_iommu_init(struct adreno_device *adreno_dev)
{
	struct kgsl_device *device = &adreno_dev->dev;
	struct kgsl_iommu *iommu = device->mmu.priv;
	struct kgsl_iommu_unit *iommu_unit = &iommu->iommu_unit;

	if (kgsl_mmu_get_mmutype() == KGSL_MMU_TYPE_NONE)
		return;

	/* Overwrite the ahb_base_offset for iommu v2 targets here */
	if (kgsl_msm_supports_iommu_v2()) {
		if (adreno_is_a405(adreno_dev))
			iommu_unit->ahb_base_offset =
					KGSL_IOMMU_V2_AHB_BASE_OFFSET_A405;
		else if (adreno_is_a530(adreno_dev))
			iommu_unit->ahb_base_offset =
					KGSL_IOMMU_V2_AHB_BASE_OFFSET_A530;
		else
			iommu_unit->ahb_base_offset =
					KGSL_IOMMU_V2_AHB_BASE_OFFSET;
	}

	/*
	 * A nop is required in an indirect buffer when switching
	 * pagetables in-stream
	 */
	kgsl_sharedmem_writel(device, &device->mmu.setstate_memory,
				KGSL_IOMMU_SETSTATE_NOP_OFFSET,
				cp_packet(adreno_dev, CP_NOP, 1));

	/* set iommu features here */
	if (adreno_is_a420(adreno_dev))
		device->mmu.features |= KGSL_MMU_FLUSH_TLB_ON_MAP;

}

/**
 * adreno_mmu_set_pt_ctx() - Change the pagetable of the current RB
 * @device: Pointer to device to which the rb belongs
 * @rb: The RB pointer on which pagetable is to be changed
 * @new_pt: The new pt the device will change to
 * @drawctxt: The context whose pagetable the ringbuffer is switching to,
 * NULL means KGSL_CONTEXT_GLOBAL
 *
 * Returns 0 on success else error code.
 */
int adreno_iommu_set_pt_ctx(struct adreno_ringbuffer *rb,
			struct kgsl_pagetable *new_pt,
			struct adreno_context *drawctxt)
{
	struct kgsl_device *device = rb->device;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	struct kgsl_pagetable *cur_pt = device->mmu.defaultpagetable;
	int result = 0;
	int cpu_path = 0;

	if (rb->drawctxt_active)
		cur_pt = rb->drawctxt_active->base.proc_priv->pagetable;

	cpu_path = _ctx_switch_use_cpu_path(adreno_dev, new_pt, rb);

	/* Pagetable switch */
	if (new_pt != cur_pt) {
		if (cpu_path)
			result = _set_pagetable_cpu(rb, new_pt);
		else
			result = _set_pagetable_gpu(rb, new_pt);
	}

	if (result) {
		KGSL_DRV_ERR(device, "Error switching pagetable %d\n", result);
		return result;
	}

	/* Context switch */
	if (cpu_path)
		_set_ctxt_cpu(rb, drawctxt);
	else
		result = _set_ctxt_gpu(rb, drawctxt);

	if (result)
		KGSL_DRV_ERR(device, "Error switching context %d\n", result);

	return result;
}
/**
 * adreno_iommu_set_pt_generate_rb_cmds() - Generate commands to switch pt
 * in a ringbuffer descriptor
 * @rb: The RB whose descriptor is used
 * @pt: The pt to switch to
 */
void adreno_iommu_set_pt_generate_rb_cmds(struct adreno_ringbuffer *rb,
						struct kgsl_pagetable *pt)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(rb->device);

	if (!ADRENO_FEATURE(adreno_dev, ADRENO_HAS_REG_TO_REG_CMDS) ||
		rb->pt_update_desc.hostptr)
		return;

	rb->pt_update_desc.hostptr = rb->pagetable_desc.hostptr +
			sizeof(struct adreno_ringbuffer_pagetable_info);
	rb->pt_update_desc.size =
		adreno_iommu_set_pt_generate_cmds(rb,
				rb->pt_update_desc.hostptr, pt) *
				sizeof(unsigned int);
	rb->pt_update_desc.gpuaddr = rb->pagetable_desc.gpuaddr +
			sizeof(struct adreno_ringbuffer_pagetable_info);
}
