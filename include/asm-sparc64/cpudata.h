/* cpudata.h: Per-cpu parameters.
 *
 * Copyright (C) 2003, 2005, 2006 David S. Miller (davem@davemloft.net)
 */

#ifndef _SPARC64_CPUDATA_H
#define _SPARC64_CPUDATA_H

#include <asm/hypervisor.h>
#include <asm/asi.h>

#ifndef __ASSEMBLY__

#include <linux/percpu.h>
#include <linux/threads.h>

typedef struct {
	/* Dcache line 1 */
	unsigned int	__softirq_pending; /* must be 1st, see rtrap.S */
	unsigned int	multiplier;
	unsigned int	counter;
	unsigned int	idle_volume;
	unsigned long	clock_tick;	/* %tick's per second */
	unsigned long	udelay_val;

	/* Dcache line 2, rarely used */
	unsigned int	dcache_size;
	unsigned int	dcache_line_size;
	unsigned int	icache_size;
	unsigned int	icache_line_size;
	unsigned int	ecache_size;
	unsigned int	ecache_line_size;
	unsigned int	__pad3;
	unsigned int	__pad4;
} cpuinfo_sparc;

DECLARE_PER_CPU(cpuinfo_sparc, __cpu_data);
#define cpu_data(__cpu)		per_cpu(__cpu_data, (__cpu))
#define local_cpu_data()	__get_cpu_var(__cpu_data)

/* Trap handling code needs to get at a few critical values upon
 * trap entry and to process TSB misses.  These cannot be in the
 * per_cpu() area as we really need to lock them into the TLB and
 * thus make them part of the main kernel image.  As a result we
 * try to make this as small as possible.
 *
 * This is padded out and aligned to 64-bytes to avoid false sharing
 * on SMP.
 */

/* If you modify the size of this structure, please update
 * TRAP_BLOCK_SZ_SHIFT below.
 */
struct thread_info;
struct trap_per_cpu {
/* D-cache line 1: Basic thread information, cpu and device mondo queues */
	struct thread_info	*thread;
	unsigned long		pgd_paddr;
	unsigned long		cpu_mondo_pa;
	unsigned long		dev_mondo_pa;

/* D-cache line 2: Error Mondo Queue and kernel buffer pointers */
	unsigned long		resum_mondo_pa;
	unsigned long		resum_kernel_buf_pa;
	unsigned long		nonresum_mondo_pa;
	unsigned long		nonresum_kernel_buf_pa;

/* Dcache lines 3 and 4: Hypervisor Fault Status */
	struct hv_fault_status	fault_info;
} __attribute__((aligned(64)));
extern struct trap_per_cpu trap_block[NR_CPUS];
extern void init_cur_cpu_trap(void);
extern void setup_tba(void);

#ifdef CONFIG_SMP
struct cpuid_patch_entry {
	unsigned int	addr;
	unsigned int	cheetah_safari[4];
	unsigned int	cheetah_jbus[4];
	unsigned int	starfire[4];
	unsigned int	sun4v[4];
};
extern struct cpuid_patch_entry __cpuid_patch, __cpuid_patch_end;
#endif

struct sun4v_1insn_patch_entry {
	unsigned int	addr;
	unsigned int	insn;
};
extern struct sun4v_1insn_patch_entry __sun4v_1insn_patch,
	__sun4v_1insn_patch_end;

struct sun4v_2insn_patch_entry {
	unsigned int	addr;
	unsigned int	insns[2];
};
extern struct sun4v_2insn_patch_entry __sun4v_2insn_patch,
	__sun4v_2insn_patch_end;

#endif /* !(__ASSEMBLY__) */

#define TRAP_PER_CPU_THREAD		0x00
#define TRAP_PER_CPU_PGD_PADDR		0x08
#define TRAP_PER_CPU_CPU_MONDO_PA	0x10
#define TRAP_PER_CPU_DEV_MONDO_PA	0x18
#define TRAP_PER_CPU_RESUM_MONDO_PA	0x20
#define TRAP_PER_CPU_RESUM_KBUF_PA	0x28
#define TRAP_PER_CPU_NONRESUM_MONDO_PA	0x30
#define TRAP_PER_CPU_NONRESUM_KBUF_PA	0x38
#define TRAP_PER_CPU_FAULT_INFO		0x40

#define TRAP_BLOCK_SZ_SHIFT		7

#include <asm/scratchpad.h>

#ifdef CONFIG_SMP

#define __GET_CPUID(REG)				\
	/* Spitfire implementation (default). */	\
661:	ldxa		[%g0] ASI_UPA_CONFIG, REG;	\
	srlx		REG, 17, REG;			\
	 and		REG, 0x1f, REG;			\
	nop;						\
	.section	.cpuid_patch, "ax";		\
	/* Instruction location. */			\
	.word		661b;				\
	/* Cheetah Safari implementation. */		\
	ldxa		[%g0] ASI_SAFARI_CONFIG, REG;	\
	srlx		REG, 17, REG;			\
	and		REG, 0x3ff, REG;		\
	nop;						\
	/* Cheetah JBUS implementation. */		\
	ldxa		[%g0] ASI_JBUS_CONFIG, REG;	\
	srlx		REG, 17, REG;			\
	and		REG, 0x1f, REG;			\
	nop;						\
	/* Starfire implementation. */			\
	sethi		%hi(0x1fff40000d0 >> 9), REG;	\
	sllx		REG, 9, REG;			\
	or		REG, 0xd0, REG;			\
	lduwa		[REG] ASI_PHYS_BYPASS_EC_E, REG;\
	/* sun4v implementation. */			\
	mov		SCRATCHPAD_CPUID, REG;		\
	ldxa		[REG] ASI_SCRATCHPAD, REG;	\
	nop;						\
	nop;						\
	.previous;

/* Clobbers TMP, current address space PGD phys address into DEST.  */
#define TRAP_LOAD_PGD_PHYS(DEST, TMP)		\
	__GET_CPUID(TMP)			\
	sethi	%hi(trap_block), DEST;		\
	sllx	TMP, TRAP_BLOCK_SZ_SHIFT, TMP;	\
	or	DEST, %lo(trap_block), DEST;	\
	add	DEST, TMP, DEST;		\
	ldx	[DEST + TRAP_PER_CPU_PGD_PADDR], DEST;

/* Clobbers TMP, loads local processor's IRQ work area into DEST.  */
#define TRAP_LOAD_IRQ_WORK(DEST, TMP)		\
	__GET_CPUID(TMP)			\
	sethi	%hi(__irq_work), DEST;		\
	sllx	TMP, 6, TMP;			\
	or	DEST, %lo(__irq_work), DEST;	\
	add	DEST, TMP, DEST;

/* Clobbers TMP, loads DEST with current thread info pointer.  */
#define TRAP_LOAD_THREAD_REG(DEST, TMP)		\
	__GET_CPUID(TMP)			\
	sethi	%hi(trap_block), DEST;		\
	sllx	TMP, TRAP_BLOCK_SZ_SHIFT, TMP;	\
	or	DEST, %lo(trap_block), DEST;	\
	ldx	[DEST + TMP], DEST;

/* Given the current thread info pointer in THR, load the per-cpu
 * area base of the current processor into DEST.  REG1, REG2, and REG3 are
 * clobbered.
 *
 * You absolutely cannot use DEST as a temporary in this code.  The
 * reason is that traps can happen during execution, and return from
 * trap will load the fully resolved DEST per-cpu base.  This can corrupt
 * the calculations done by the macro mid-stream.
 */
#define LOAD_PER_CPU_BASE(DEST, THR, REG1, REG2, REG3)	\
	ldub	[THR + TI_CPU], REG1;			\
	sethi	%hi(__per_cpu_shift), REG3;		\
	sethi	%hi(__per_cpu_base), REG2;		\
	ldx	[REG3 + %lo(__per_cpu_shift)], REG3;	\
	ldx	[REG2 + %lo(__per_cpu_base)], REG2;	\
	sllx	REG1, REG3, REG3;			\
	add	REG3, REG2, DEST;

#else

#define __GET_CPUID(REG)				\
	mov	0, REG;

/* Uniprocessor versions, we know the cpuid is zero.  */
#define TRAP_LOAD_PGD_PHYS(DEST, TMP)		\
	sethi	%hi(trap_block), DEST;		\
	or	DEST, %lo(trap_block), DEST;	\
	ldx	[DEST + TRAP_PER_CPU_PGD_PADDR], DEST;

#define TRAP_LOAD_IRQ_WORK(DEST, TMP)		\
	sethi	%hi(__irq_work), DEST;		\
	or	DEST, %lo(__irq_work), DEST;

#define TRAP_LOAD_THREAD_REG(DEST, TMP)		\
	sethi	%hi(trap_block), DEST;		\
	ldx	[DEST + %lo(trap_block)], DEST;

/* No per-cpu areas on uniprocessor, so no need to load DEST.  */
#define LOAD_PER_CPU_BASE(DEST, THR, REG1, REG2, REG3)

#endif /* !(CONFIG_SMP) */

#endif /* _SPARC64_CPUDATA_H */
