#ifndef __TRACE_EVENTS_H
#define __TRACE_EVENTS_H

#include <linux/trace_seq.h>
#include "trace.h"

typedef enum print_line_t (*trace_print_func)(struct trace_iterator *iter,
					      int flags);

struct trace_event {
	struct hlist_node	node;
	int			type;
	trace_print_func	trace;
	trace_print_func	raw;
	trace_print_func	hex;
	trace_print_func	binary;
};

extern enum print_line_t
trace_print_bprintk_msg_only(struct trace_iterator *iter);
extern enum print_line_t
trace_print_printk_msg_only(struct trace_iterator *iter);

extern int
seq_print_ip_sym(struct trace_seq *s, unsigned long ip,
		unsigned long sym_flags);
extern int seq_print_userip_objs(const struct userstack_entry *entry,
				 struct trace_seq *s, unsigned long sym_flags);
extern int seq_print_user_ip(struct trace_seq *s, struct mm_struct *mm,
			     unsigned long ip, unsigned long sym_flags);

extern int trace_print_context(struct trace_iterator *iter);
extern int trace_print_lat_context(struct trace_iterator *iter);

extern struct trace_event *ftrace_find_event(int type);
extern int register_ftrace_event(struct trace_event *event);
extern int unregister_ftrace_event(struct trace_event *event);

extern enum print_line_t trace_nop_print(struct trace_iterator *iter,
					 int flags);

#define MAX_MEMHEX_BYTES	8
#define HEX_CHARS		(MAX_MEMHEX_BYTES*2 + 1)

#define SEQ_PUT_FIELD_RET(s, x)				\
do {							\
	if (!trace_seq_putmem(s, &(x), sizeof(x)))	\
		return TRACE_TYPE_PARTIAL_LINE;		\
} while (0)

#define SEQ_PUT_HEX_FIELD_RET(s, x)			\
do {							\
	BUILD_BUG_ON(sizeof(x) > MAX_MEMHEX_BYTES);	\
	if (!trace_seq_putmem_hex(s, &(x), sizeof(x)))	\
		return TRACE_TYPE_PARTIAL_LINE;		\
} while (0)

#endif

