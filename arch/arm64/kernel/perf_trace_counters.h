/* Copyright (c) 2013-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM perf_trace_counters

#if !defined(_PERF_TRACE_COUNTERS_H_) || defined(TRACE_HEADER_MULTI_READ)
#define _PERF_TRACE_COUNTERS_H_

/* Ctr index for PMCNTENSET/CLR */
#define CC 0x80000000
#define C0 0x1
#define C1 0x2
#define C2 0x4
#define C3 0x8
#define C_ALL (CC | C0 | C1 | C2 | C3)
#define NUM_L1_CTRS 4

#include <linux/sched.h>
#include <linux/cpumask.h>
#include <linux/tracepoint.h>

DECLARE_PER_CPU(u32, cntenset_val);
DECLARE_PER_CPU(u32, previous_ccnt);
DECLARE_PER_CPU(u32[NUM_L1_CTRS], previous_l1_cnts);
TRACE_EVENT(sched_switch_with_ctrs,

		TP_PROTO(pid_t prev, pid_t next),

		TP_ARGS(prev, next),

		TP_STRUCT__entry(
			__field(pid_t,	old_pid)
			__field(pid_t,	new_pid)
			__field(u32, cctr)
			__field(u32, ctr0)
			__field(u32, ctr1)
			__field(u32, ctr2)
			__field(u32, ctr3)
			__field(u32, lctr0)
			__field(u32, lctr1)
		),

		TP_fast_assign(
			u32 cpu = smp_processor_id();
			u32 i;
			u32 cnten_val;
			u32 total_ccnt = 0;
			u32 total_cnt = 0;
			u32 delta_l1_cnts[NUM_L1_CTRS];
			__entry->old_pid	= prev;
			__entry->new_pid	= next;

			cnten_val = per_cpu(cntenset_val, cpu);

			if (cnten_val & CC) {
				asm volatile("mrs %0, pmccntr_el0"
							: "=r" (total_ccnt));
				/* Read value */
				__entry->cctr = total_ccnt -
					per_cpu(previous_ccnt, cpu);
				per_cpu(previous_ccnt, cpu) = total_ccnt;
			}
			for (i = 0; i < NUM_L1_CTRS; i++) {
				if (cnten_val & (1 << i)) {
					/* Select */
					asm volatile("msr pmselr_el0, %0"
							: : "r" (i));
					isb();
					asm volatile("mrs %0, pmxevcntr_el0"
							: "=r" (total_cnt));
					/* Read value */
					delta_l1_cnts[i] = total_cnt -
					  per_cpu(previous_l1_cnts[i], cpu);
					per_cpu(previous_l1_cnts[i], cpu) =
						total_cnt;
				} else
					delta_l1_cnts[i] = 0;
			}

			__entry->ctr0 = delta_l1_cnts[0];
			__entry->ctr1 = delta_l1_cnts[1];
			__entry->ctr2 = delta_l1_cnts[2];
			__entry->ctr3 = delta_l1_cnts[3];
			__entry->lctr0 = 0;
			__entry->lctr1 = 0;
		),

		TP_printk("prev_pid=%d, next_pid=%d, CCNTR: %u, CTR0: %u, CTR1: %u, CTR2: %u, CTR3: %u, L2CTR0: %u, L2CTR1: %u",
				__entry->old_pid, __entry->new_pid,
				__entry->cctr, __entry->ctr0, __entry->ctr1,
				__entry->ctr2, __entry->ctr3,
				__entry->lctr0, __entry->lctr1)
);

#endif
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH ../../arch/arm64/kernel
#define TRACE_INCLUDE_FILE perf_trace_counters
#include <trace/define_trace.h>
