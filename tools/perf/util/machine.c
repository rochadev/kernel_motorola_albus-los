#include "debug.h"
#include "event.h"
#include "machine.h"
#include "map.h"
#include "thread.h"
#include <stdbool.h>

static struct thread *__machine__findnew_thread(struct machine *machine, pid_t pid,
						bool create)
{
	struct rb_node **p = &machine->threads.rb_node;
	struct rb_node *parent = NULL;
	struct thread *th;

	/*
	 * Font-end cache - PID lookups come in blocks,
	 * so most of the time we dont have to look up
	 * the full rbtree:
	 */
	if (machine->last_match && machine->last_match->pid == pid)
		return machine->last_match;

	while (*p != NULL) {
		parent = *p;
		th = rb_entry(parent, struct thread, rb_node);

		if (th->pid == pid) {
			machine->last_match = th;
			return th;
		}

		if (pid < th->pid)
			p = &(*p)->rb_left;
		else
			p = &(*p)->rb_right;
	}

	if (!create)
		return NULL;

	th = thread__new(pid);
	if (th != NULL) {
		rb_link_node(&th->rb_node, parent, p);
		rb_insert_color(&th->rb_node, &machine->threads);
		machine->last_match = th;
	}

	return th;
}

struct thread *machine__findnew_thread(struct machine *machine, pid_t pid)
{
	return __machine__findnew_thread(machine, pid, true);
}

struct thread *machine__find_thread(struct machine *machine, pid_t pid)
{
	return __machine__findnew_thread(machine, pid, false);
}

int machine__process_comm_event(struct machine *machine, union perf_event *event)
{
	struct thread *thread = machine__findnew_thread(machine, event->comm.tid);

	if (dump_trace)
		perf_event__fprintf_comm(event, stdout);

	if (thread == NULL || thread__set_comm(thread, event->comm.comm)) {
		dump_printf("problem processing PERF_RECORD_COMM, skipping event.\n");
		return -1;
	}

	return 0;
}

int machine__process_lost_event(struct machine *machine __maybe_unused,
				union perf_event *event)
{
	dump_printf(": id:%" PRIu64 ": lost:%" PRIu64 "\n",
		    event->lost.id, event->lost.lost);
	return 0;
}

static void machine__set_kernel_mmap_len(struct machine *machine,
					 union perf_event *event)
{
	machine->vmlinux_maps[MAP__FUNCTION]->start = event->mmap.start;
	machine->vmlinux_maps[MAP__FUNCTION]->end   = (event->mmap.start +
						       event->mmap.len);
	/*
	 * Be a bit paranoid here, some perf.data file came with
	 * a zero sized synthesized MMAP event for the kernel.
	 */
	if (machine->vmlinux_maps[MAP__FUNCTION]->end == 0)
		machine->vmlinux_maps[MAP__FUNCTION]->end = ~0ULL;
}

static int machine__process_kernel_mmap_event(struct machine *machine,
					      union perf_event *event)
{
	struct map *map;
	char kmmap_prefix[PATH_MAX];
	enum dso_kernel_type kernel_type;
	bool is_kernel_mmap;

	machine__mmap_name(machine, kmmap_prefix, sizeof(kmmap_prefix));
	if (machine__is_host(machine))
		kernel_type = DSO_TYPE_KERNEL;
	else
		kernel_type = DSO_TYPE_GUEST_KERNEL;

	is_kernel_mmap = memcmp(event->mmap.filename,
				kmmap_prefix,
				strlen(kmmap_prefix) - 1) == 0;
	if (event->mmap.filename[0] == '/' ||
	    (!is_kernel_mmap && event->mmap.filename[0] == '[')) {

		char short_module_name[1024];
		char *name, *dot;

		if (event->mmap.filename[0] == '/') {
			name = strrchr(event->mmap.filename, '/');
			if (name == NULL)
				goto out_problem;

			++name; /* skip / */
			dot = strrchr(name, '.');
			if (dot == NULL)
				goto out_problem;
			snprintf(short_module_name, sizeof(short_module_name),
					"[%.*s]", (int)(dot - name), name);
			strxfrchar(short_module_name, '-', '_');
		} else
			strcpy(short_module_name, event->mmap.filename);

		map = machine__new_module(machine, event->mmap.start,
					  event->mmap.filename);
		if (map == NULL)
			goto out_problem;

		name = strdup(short_module_name);
		if (name == NULL)
			goto out_problem;

		map->dso->short_name = name;
		map->dso->sname_alloc = 1;
		map->end = map->start + event->mmap.len;
	} else if (is_kernel_mmap) {
		const char *symbol_name = (event->mmap.filename +
				strlen(kmmap_prefix));
		/*
		 * Should be there already, from the build-id table in
		 * the header.
		 */
		struct dso *kernel = __dsos__findnew(&machine->kernel_dsos,
						     kmmap_prefix);
		if (kernel == NULL)
			goto out_problem;

		kernel->kernel = kernel_type;
		if (__machine__create_kernel_maps(machine, kernel) < 0)
			goto out_problem;

		machine__set_kernel_mmap_len(machine, event);

		/*
		 * Avoid using a zero address (kptr_restrict) for the ref reloc
		 * symbol. Effectively having zero here means that at record
		 * time /proc/sys/kernel/kptr_restrict was non zero.
		 */
		if (event->mmap.pgoff != 0) {
			maps__set_kallsyms_ref_reloc_sym(machine->vmlinux_maps,
							 symbol_name,
							 event->mmap.pgoff);
		}

		if (machine__is_default_guest(machine)) {
			/*
			 * preload dso of guest kernel and modules
			 */
			dso__load(kernel, machine->vmlinux_maps[MAP__FUNCTION],
				  NULL);
		}
	}
	return 0;
out_problem:
	return -1;
}

int machine__process_mmap_event(struct machine *machine, union perf_event *event)
{
	u8 cpumode = event->header.misc & PERF_RECORD_MISC_CPUMODE_MASK;
	struct thread *thread;
	struct map *map;
	int ret = 0;

	if (dump_trace)
		perf_event__fprintf_mmap(event, stdout);

	if (cpumode == PERF_RECORD_MISC_GUEST_KERNEL ||
	    cpumode == PERF_RECORD_MISC_KERNEL) {
		ret = machine__process_kernel_mmap_event(machine, event);
		if (ret < 0)
			goto out_problem;
		return 0;
	}

	thread = machine__findnew_thread(machine, event->mmap.pid);
	if (thread == NULL)
		goto out_problem;
	map = map__new(&machine->user_dsos, event->mmap.start,
			event->mmap.len, event->mmap.pgoff,
			event->mmap.pid, event->mmap.filename,
			MAP__FUNCTION);
	if (map == NULL)
		goto out_problem;

	thread__insert_map(thread, map);
	return 0;

out_problem:
	dump_printf("problem processing PERF_RECORD_MMAP, skipping event.\n");
	return 0;
}

int machine__process_fork_event(struct machine *machine, union perf_event *event)
{
	struct thread *thread = machine__findnew_thread(machine, event->fork.tid);
	struct thread *parent = machine__findnew_thread(machine, event->fork.ptid);

	if (dump_trace)
		perf_event__fprintf_task(event, stdout);

	if (thread == NULL || parent == NULL ||
	    thread__fork(thread, parent) < 0) {
		dump_printf("problem processing PERF_RECORD_FORK, skipping event.\n");
		return -1;
	}

	return 0;
}

int machine__process_exit_event(struct machine *machine, union perf_event *event)
{
	struct thread *thread = machine__find_thread(machine, event->fork.tid);

	if (dump_trace)
		perf_event__fprintf_task(event, stdout);

	if (thread != NULL)
		machine__remove_thread(machine, thread);

	return 0;
}

int machine__process_event(struct machine *machine, union perf_event *event)
{
	int ret;

	switch (event->header.type) {
	case PERF_RECORD_COMM:
		ret = machine__process_comm_event(machine, event); break;
	case PERF_RECORD_MMAP:
		ret = machine__process_mmap_event(machine, event); break;
	case PERF_RECORD_FORK:
		ret = machine__process_fork_event(machine, event); break;
	case PERF_RECORD_EXIT:
		ret = machine__process_exit_event(machine, event); break;
	case PERF_RECORD_LOST:
		ret = machine__process_lost_event(machine, event); break;
	default:
		ret = -1;
		break;
	}

	return ret;
}
