/*
 * Copyright (C) 2006 Jens Axboe <axboe@kernel.dk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */
#include <linux/kernel.h>
#include <linux/blkdev.h>
#include <linux/blktrace_api.h>
#include <linux/percpu.h>
#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/debugfs.h>
#include <linux/time.h>
#include <trace/block.h>
#include <linux/uaccess.h>
#include "trace_output.h"

static unsigned int blktrace_seq __read_mostly = 1;

static struct trace_array *blk_tr;
static bool blk_tracer_enabled __read_mostly;

/* Select an alternative, minimalistic output than the original one */
#define TRACE_BLK_OPT_CLASSIC	0x1

static struct tracer_opt blk_tracer_opts[] = {
	/* Default disable the minimalistic output */
	{ TRACER_OPT(blk_classic, TRACE_BLK_OPT_CLASSIC) },
	{ }
};

static struct tracer_flags blk_tracer_flags = {
	.val  = 0,
	.opts = blk_tracer_opts,
};

/* Global reference count of probes */
static atomic_t blk_probes_ref = ATOMIC_INIT(0);

static void blk_register_tracepoints(void);
static void blk_unregister_tracepoints(void);

/*
 * Send out a notify message.
 */
static void trace_note(struct blk_trace *bt, pid_t pid, int action,
		       const void *data, size_t len)
{
	struct blk_io_trace *t;

	if (!bt->rchan)
		return;

	t = relay_reserve(bt->rchan, sizeof(*t) + len);
	if (t) {
		const int cpu = smp_processor_id();

		t->magic = BLK_IO_TRACE_MAGIC | BLK_IO_TRACE_VERSION;
		t->time = ktime_to_ns(ktime_get());
		t->device = bt->dev;
		t->action = action;
		t->pid = pid;
		t->cpu = cpu;
		t->pdu_len = len;
		memcpy((void *) t + sizeof(*t), data, len);
	}
}

/*
 * Send out a notify for this process, if we haven't done so since a trace
 * started
 */
static void trace_note_tsk(struct blk_trace *bt, struct task_struct *tsk)
{
	tsk->btrace_seq = blktrace_seq;
	trace_note(bt, tsk->pid, BLK_TN_PROCESS, tsk->comm, sizeof(tsk->comm));
}

static void trace_note_time(struct blk_trace *bt)
{
	struct timespec now;
	unsigned long flags;
	u32 words[2];

	getnstimeofday(&now);
	words[0] = now.tv_sec;
	words[1] = now.tv_nsec;

	local_irq_save(flags);
	trace_note(bt, 0, BLK_TN_TIMESTAMP, words, sizeof(words));
	local_irq_restore(flags);
}

void __trace_note_message(struct blk_trace *bt, const char *fmt, ...)
{
	int n;
	va_list args;
	unsigned long flags;
	char *buf;

	if (blk_tr) {
		va_start(args, fmt);
		ftrace_vprintk(fmt, args);
		va_end(args);
		return;
	}

	if (!bt->msg_data)
		return;

	local_irq_save(flags);
	buf = per_cpu_ptr(bt->msg_data, smp_processor_id());
	va_start(args, fmt);
	n = vscnprintf(buf, BLK_TN_MAX_MSG, fmt, args);
	va_end(args);

	trace_note(bt, 0, BLK_TN_MESSAGE, buf, n);
	local_irq_restore(flags);
}
EXPORT_SYMBOL_GPL(__trace_note_message);

static int act_log_check(struct blk_trace *bt, u32 what, sector_t sector,
			 pid_t pid)
{
	if (((bt->act_mask << BLK_TC_SHIFT) & what) == 0)
		return 1;
	if (sector < bt->start_lba || sector > bt->end_lba)
		return 1;
	if (bt->pid && pid != bt->pid)
		return 1;

	return 0;
}

/*
 * Data direction bit lookup
 */
static u32 ddir_act[2] __read_mostly = { BLK_TC_ACT(BLK_TC_READ),
					 BLK_TC_ACT(BLK_TC_WRITE) };

/* The ilog2() calls fall out because they're constant */
#define MASK_TC_BIT(rw, __name) ((rw & (1 << BIO_RW_ ## __name)) << \
	  (ilog2(BLK_TC_ ## __name) + BLK_TC_SHIFT - BIO_RW_ ## __name))

/*
 * The worker for the various blk_add_trace*() types. Fills out a
 * blk_io_trace structure and places it in a per-cpu subbuffer.
 */
static void __blk_add_trace(struct blk_trace *bt, sector_t sector, int bytes,
		     int rw, u32 what, int error, int pdu_len, void *pdu_data)
{
	struct task_struct *tsk = current;
	struct ring_buffer_event *event = NULL;
	struct blk_io_trace *t;
	unsigned long flags = 0;
	unsigned long *sequence;
	pid_t pid;
	int cpu, pc = 0;

	if (unlikely(bt->trace_state != Blktrace_running ||
		     !blk_tracer_enabled))
		return;

	what |= ddir_act[rw & WRITE];
	what |= MASK_TC_BIT(rw, BARRIER);
	what |= MASK_TC_BIT(rw, SYNCIO);
	what |= MASK_TC_BIT(rw, AHEAD);
	what |= MASK_TC_BIT(rw, META);
	what |= MASK_TC_BIT(rw, DISCARD);

	pid = tsk->pid;
	if (unlikely(act_log_check(bt, what, sector, pid)))
		return;
	cpu = raw_smp_processor_id();

	if (blk_tr) {
		tracing_record_cmdline(current);

		pc = preempt_count();
		event = trace_buffer_lock_reserve(blk_tr, TRACE_BLK,
						  sizeof(*t) + pdu_len,
						  0, pc);
		if (!event)
			return;
		t = ring_buffer_event_data(event);
		goto record_it;
	}

	/*
	 * A word about the locking here - we disable interrupts to reserve
	 * some space in the relay per-cpu buffer, to prevent an irq
	 * from coming in and stepping on our toes.
	 */
	local_irq_save(flags);

	if (unlikely(tsk->btrace_seq != blktrace_seq))
		trace_note_tsk(bt, tsk);

	t = relay_reserve(bt->rchan, sizeof(*t) + pdu_len);
	if (t) {
		sequence = per_cpu_ptr(bt->sequence, cpu);

		t->magic = BLK_IO_TRACE_MAGIC | BLK_IO_TRACE_VERSION;
		t->sequence = ++(*sequence);
		t->time = ktime_to_ns(ktime_get());
record_it:
		/*
		 * These two are not needed in ftrace as they are in the
		 * generic trace_entry, filled by tracing_generic_entry_update,
		 * but for the trace_event->bin() synthesizer benefit we do it
		 * here too.
		 */
		t->cpu = cpu;
		t->pid = pid;

		t->sector = sector;
		t->bytes = bytes;
		t->action = what;
		t->device = bt->dev;
		t->error = error;
		t->pdu_len = pdu_len;

		if (pdu_len)
			memcpy((void *) t + sizeof(*t), pdu_data, pdu_len);

		if (blk_tr) {
			trace_buffer_unlock_commit(blk_tr, event, 0, pc);
			return;
		}
	}

	local_irq_restore(flags);
}

static struct dentry *blk_tree_root;
static DEFINE_MUTEX(blk_tree_mutex);

static void blk_trace_cleanup(struct blk_trace *bt)
{
	debugfs_remove(bt->msg_file);
	debugfs_remove(bt->dropped_file);
	relay_close(bt->rchan);
	free_percpu(bt->sequence);
	free_percpu(bt->msg_data);
	kfree(bt);
	if (atomic_dec_and_test(&blk_probes_ref))
		blk_unregister_tracepoints();
}

int blk_trace_remove(struct request_queue *q)
{
	struct blk_trace *bt;

	bt = xchg(&q->blk_trace, NULL);
	if (!bt)
		return -EINVAL;

	if (bt->trace_state == Blktrace_setup ||
	    bt->trace_state == Blktrace_stopped)
		blk_trace_cleanup(bt);

	return 0;
}
EXPORT_SYMBOL_GPL(blk_trace_remove);

static int blk_dropped_open(struct inode *inode, struct file *filp)
{
	filp->private_data = inode->i_private;

	return 0;
}

static ssize_t blk_dropped_read(struct file *filp, char __user *buffer,
				size_t count, loff_t *ppos)
{
	struct blk_trace *bt = filp->private_data;
	char buf[16];

	snprintf(buf, sizeof(buf), "%u\n", atomic_read(&bt->dropped));

	return simple_read_from_buffer(buffer, count, ppos, buf, strlen(buf));
}

static const struct file_operations blk_dropped_fops = {
	.owner =	THIS_MODULE,
	.open =		blk_dropped_open,
	.read =		blk_dropped_read,
};

static int blk_msg_open(struct inode *inode, struct file *filp)
{
	filp->private_data = inode->i_private;

	return 0;
}

static ssize_t blk_msg_write(struct file *filp, const char __user *buffer,
				size_t count, loff_t *ppos)
{
	char *msg;
	struct blk_trace *bt;

	if (count > BLK_TN_MAX_MSG)
		return -EINVAL;

	msg = kmalloc(count, GFP_KERNEL);
	if (msg == NULL)
		return -ENOMEM;

	if (copy_from_user(msg, buffer, count)) {
		kfree(msg);
		return -EFAULT;
	}

	bt = filp->private_data;
	__trace_note_message(bt, "%s", msg);
	kfree(msg);

	return count;
}

static const struct file_operations blk_msg_fops = {
	.owner =	THIS_MODULE,
	.open =		blk_msg_open,
	.write =	blk_msg_write,
};

/*
 * Keep track of how many times we encountered a full subbuffer, to aid
 * the user space app in telling how many lost events there were.
 */
static int blk_subbuf_start_callback(struct rchan_buf *buf, void *subbuf,
				     void *prev_subbuf, size_t prev_padding)
{
	struct blk_trace *bt;

	if (!relay_buf_full(buf))
		return 1;

	bt = buf->chan->private_data;
	atomic_inc(&bt->dropped);
	return 0;
}

static int blk_remove_buf_file_callback(struct dentry *dentry)
{
	struct dentry *parent = dentry->d_parent;
	debugfs_remove(dentry);

	/*
	* this will fail for all but the last file, but that is ok. what we
	* care about is the top level buts->name directory going away, when
	* the last trace file is gone. Then we don't have to rmdir() that
	* manually on trace stop, so it nicely solves the issue with
	* force killing of running traces.
	*/

	debugfs_remove(parent);
	return 0;
}

static struct dentry *blk_create_buf_file_callback(const char *filename,
						   struct dentry *parent,
						   int mode,
						   struct rchan_buf *buf,
						   int *is_global)
{
	return debugfs_create_file(filename, mode, parent, buf,
					&relay_file_operations);
}

static struct rchan_callbacks blk_relay_callbacks = {
	.subbuf_start		= blk_subbuf_start_callback,
	.create_buf_file	= blk_create_buf_file_callback,
	.remove_buf_file	= blk_remove_buf_file_callback,
};

/*
 * Setup everything required to start tracing
 */
int do_blk_trace_setup(struct request_queue *q, char *name, dev_t dev,
			struct blk_user_trace_setup *buts)
{
	struct blk_trace *old_bt, *bt = NULL;
	struct dentry *dir = NULL;
	int ret, i;

	if (!buts->buf_size || !buts->buf_nr)
		return -EINVAL;

	strncpy(buts->name, name, BLKTRACE_BDEV_SIZE);
	buts->name[BLKTRACE_BDEV_SIZE - 1] = '\0';

	/*
	 * some device names have larger paths - convert the slashes
	 * to underscores for this to work as expected
	 */
	for (i = 0; i < strlen(buts->name); i++)
		if (buts->name[i] == '/')
			buts->name[i] = '_';

	ret = -ENOMEM;
	bt = kzalloc(sizeof(*bt), GFP_KERNEL);
	if (!bt)
		goto err;

	bt->sequence = alloc_percpu(unsigned long);
	if (!bt->sequence)
		goto err;

	bt->msg_data = __alloc_percpu(BLK_TN_MAX_MSG, __alignof__(char));
	if (!bt->msg_data)
		goto err;

	ret = -ENOENT;

	if (!blk_tree_root) {
		blk_tree_root = debugfs_create_dir("block", NULL);
		if (!blk_tree_root)
			goto err;
	}

	dir = debugfs_create_dir(buts->name, blk_tree_root);

	if (!dir)
		goto err;

	bt->dir = dir;
	bt->dev = dev;
	atomic_set(&bt->dropped, 0);

	ret = -EIO;
	bt->dropped_file = debugfs_create_file("dropped", 0444, dir, bt,
					       &blk_dropped_fops);
	if (!bt->dropped_file)
		goto err;

	bt->msg_file = debugfs_create_file("msg", 0222, dir, bt, &blk_msg_fops);
	if (!bt->msg_file)
		goto err;

	bt->rchan = relay_open("trace", dir, buts->buf_size,
				buts->buf_nr, &blk_relay_callbacks, bt);
	if (!bt->rchan)
		goto err;

	bt->act_mask = buts->act_mask;
	if (!bt->act_mask)
		bt->act_mask = (u16) -1;

	bt->start_lba = buts->start_lba;
	bt->end_lba = buts->end_lba;
	if (!bt->end_lba)
		bt->end_lba = -1ULL;

	bt->pid = buts->pid;
	bt->trace_state = Blktrace_setup;

	ret = -EBUSY;
	old_bt = xchg(&q->blk_trace, bt);
	if (old_bt) {
		(void) xchg(&q->blk_trace, old_bt);
		goto err;
	}

	if (atomic_add_return(1, &blk_probes_ref) == 1)
		blk_register_tracepoints();

	return 0;
err:
	if (bt) {
		if (bt->msg_file)
			debugfs_remove(bt->msg_file);
		if (bt->dropped_file)
			debugfs_remove(bt->dropped_file);
		free_percpu(bt->sequence);
		free_percpu(bt->msg_data);
		if (bt->rchan)
			relay_close(bt->rchan);
		kfree(bt);
	}
	return ret;
}

int blk_trace_setup(struct request_queue *q, char *name, dev_t dev,
		    char __user *arg)
{
	struct blk_user_trace_setup buts;
	int ret;

	ret = copy_from_user(&buts, arg, sizeof(buts));
	if (ret)
		return -EFAULT;

	ret = do_blk_trace_setup(q, name, dev, &buts);
	if (ret)
		return ret;

	if (copy_to_user(arg, &buts, sizeof(buts)))
		return -EFAULT;

	return 0;
}
EXPORT_SYMBOL_GPL(blk_trace_setup);

int blk_trace_startstop(struct request_queue *q, int start)
{
	int ret;
	struct blk_trace *bt = q->blk_trace;

	if (bt == NULL)
		return -EINVAL;

	/*
	 * For starting a trace, we can transition from a setup or stopped
	 * trace. For stopping a trace, the state must be running
	 */
	ret = -EINVAL;
	if (start) {
		if (bt->trace_state == Blktrace_setup ||
		    bt->trace_state == Blktrace_stopped) {
			blktrace_seq++;
			smp_mb();
			bt->trace_state = Blktrace_running;

			trace_note_time(bt);
			ret = 0;
		}
	} else {
		if (bt->trace_state == Blktrace_running) {
			bt->trace_state = Blktrace_stopped;
			relay_flush(bt->rchan);
			ret = 0;
		}
	}

	return ret;
}
EXPORT_SYMBOL_GPL(blk_trace_startstop);

/**
 * blk_trace_ioctl: - handle the ioctls associated with tracing
 * @bdev:	the block device
 * @cmd:	the ioctl cmd
 * @arg:	the argument data, if any
 *
 **/
int blk_trace_ioctl(struct block_device *bdev, unsigned cmd, char __user *arg)
{
	struct request_queue *q;
	int ret, start = 0;
	char b[BDEVNAME_SIZE];

	q = bdev_get_queue(bdev);
	if (!q)
		return -ENXIO;

	mutex_lock(&bdev->bd_mutex);

	switch (cmd) {
	case BLKTRACESETUP:
		bdevname(bdev, b);
		ret = blk_trace_setup(q, b, bdev->bd_dev, arg);
		break;
	case BLKTRACESTART:
		start = 1;
	case BLKTRACESTOP:
		ret = blk_trace_startstop(q, start);
		break;
	case BLKTRACETEARDOWN:
		ret = blk_trace_remove(q);
		break;
	default:
		ret = -ENOTTY;
		break;
	}

	mutex_unlock(&bdev->bd_mutex);
	return ret;
}

/**
 * blk_trace_shutdown: - stop and cleanup trace structures
 * @q:    the request queue associated with the device
 *
 **/
void blk_trace_shutdown(struct request_queue *q)
{
	if (q->blk_trace) {
		blk_trace_startstop(q, 0);
		blk_trace_remove(q);
	}
}

/*
 * blktrace probes
 */

/**
 * blk_add_trace_rq - Add a trace for a request oriented action
 * @q:		queue the io is for
 * @rq:		the source request
 * @what:	the action
 *
 * Description:
 *     Records an action against a request. Will log the bio offset + size.
 *
 **/
static void blk_add_trace_rq(struct request_queue *q, struct request *rq,
				    u32 what)
{
	struct blk_trace *bt = q->blk_trace;
	int rw = rq->cmd_flags & 0x03;

	if (likely(!bt))
		return;

	if (blk_discard_rq(rq))
		rw |= (1 << BIO_RW_DISCARD);

	if (blk_pc_request(rq)) {
		what |= BLK_TC_ACT(BLK_TC_PC);
		__blk_add_trace(bt, 0, rq->data_len, rw, what, rq->errors,
				sizeof(rq->cmd), rq->cmd);
	} else  {
		what |= BLK_TC_ACT(BLK_TC_FS);
		__blk_add_trace(bt, rq->hard_sector, rq->hard_nr_sectors << 9,
				rw, what, rq->errors, 0, NULL);
	}
}

static void blk_add_trace_rq_abort(struct request_queue *q, struct request *rq)
{
	blk_add_trace_rq(q, rq, BLK_TA_ABORT);
}

static void blk_add_trace_rq_insert(struct request_queue *q, struct request *rq)
{
	blk_add_trace_rq(q, rq, BLK_TA_INSERT);
}

static void blk_add_trace_rq_issue(struct request_queue *q, struct request *rq)
{
	blk_add_trace_rq(q, rq, BLK_TA_ISSUE);
}

static void blk_add_trace_rq_requeue(struct request_queue *q,
				     struct request *rq)
{
	blk_add_trace_rq(q, rq, BLK_TA_REQUEUE);
}

static void blk_add_trace_rq_complete(struct request_queue *q,
				      struct request *rq)
{
	blk_add_trace_rq(q, rq, BLK_TA_COMPLETE);
}

/**
 * blk_add_trace_bio - Add a trace for a bio oriented action
 * @q:		queue the io is for
 * @bio:	the source bio
 * @what:	the action
 *
 * Description:
 *     Records an action against a bio. Will log the bio offset + size.
 *
 **/
static void blk_add_trace_bio(struct request_queue *q, struct bio *bio,
				     u32 what)
{
	struct blk_trace *bt = q->blk_trace;

	if (likely(!bt))
		return;

	__blk_add_trace(bt, bio->bi_sector, bio->bi_size, bio->bi_rw, what,
			!bio_flagged(bio, BIO_UPTODATE), 0, NULL);
}

static void blk_add_trace_bio_bounce(struct request_queue *q, struct bio *bio)
{
	blk_add_trace_bio(q, bio, BLK_TA_BOUNCE);
}

static void blk_add_trace_bio_complete(struct request_queue *q, struct bio *bio)
{
	blk_add_trace_bio(q, bio, BLK_TA_COMPLETE);
}

static void blk_add_trace_bio_backmerge(struct request_queue *q,
					struct bio *bio)
{
	blk_add_trace_bio(q, bio, BLK_TA_BACKMERGE);
}

static void blk_add_trace_bio_frontmerge(struct request_queue *q,
					 struct bio *bio)
{
	blk_add_trace_bio(q, bio, BLK_TA_FRONTMERGE);
}

static void blk_add_trace_bio_queue(struct request_queue *q, struct bio *bio)
{
	blk_add_trace_bio(q, bio, BLK_TA_QUEUE);
}

static void blk_add_trace_getrq(struct request_queue *q,
				struct bio *bio, int rw)
{
	if (bio)
		blk_add_trace_bio(q, bio, BLK_TA_GETRQ);
	else {
		struct blk_trace *bt = q->blk_trace;

		if (bt)
			__blk_add_trace(bt, 0, 0, rw, BLK_TA_GETRQ, 0, 0, NULL);
	}
}


static void blk_add_trace_sleeprq(struct request_queue *q,
				  struct bio *bio, int rw)
{
	if (bio)
		blk_add_trace_bio(q, bio, BLK_TA_SLEEPRQ);
	else {
		struct blk_trace *bt = q->blk_trace;

		if (bt)
			__blk_add_trace(bt, 0, 0, rw, BLK_TA_SLEEPRQ,
					0, 0, NULL);
	}
}

static void blk_add_trace_plug(struct request_queue *q)
{
	struct blk_trace *bt = q->blk_trace;

	if (bt)
		__blk_add_trace(bt, 0, 0, 0, BLK_TA_PLUG, 0, 0, NULL);
}

static void blk_add_trace_unplug_io(struct request_queue *q)
{
	struct blk_trace *bt = q->blk_trace;

	if (bt) {
		unsigned int pdu = q->rq.count[READ] + q->rq.count[WRITE];
		__be64 rpdu = cpu_to_be64(pdu);

		__blk_add_trace(bt, 0, 0, 0, BLK_TA_UNPLUG_IO, 0,
				sizeof(rpdu), &rpdu);
	}
}

static void blk_add_trace_unplug_timer(struct request_queue *q)
{
	struct blk_trace *bt = q->blk_trace;

	if (bt) {
		unsigned int pdu = q->rq.count[READ] + q->rq.count[WRITE];
		__be64 rpdu = cpu_to_be64(pdu);

		__blk_add_trace(bt, 0, 0, 0, BLK_TA_UNPLUG_TIMER, 0,
				sizeof(rpdu), &rpdu);
	}
}

static void blk_add_trace_split(struct request_queue *q, struct bio *bio,
				unsigned int pdu)
{
	struct blk_trace *bt = q->blk_trace;

	if (bt) {
		__be64 rpdu = cpu_to_be64(pdu);

		__blk_add_trace(bt, bio->bi_sector, bio->bi_size, bio->bi_rw,
				BLK_TA_SPLIT, !bio_flagged(bio, BIO_UPTODATE),
				sizeof(rpdu), &rpdu);
	}
}

/**
 * blk_add_trace_remap - Add a trace for a remap operation
 * @q:		queue the io is for
 * @bio:	the source bio
 * @dev:	target device
 * @from:	source sector
 * @to:		target sector
 *
 * Description:
 *     Device mapper or raid target sometimes need to split a bio because
 *     it spans a stripe (or similar). Add a trace for that action.
 *
 **/
static void blk_add_trace_remap(struct request_queue *q, struct bio *bio,
				       dev_t dev, sector_t from, sector_t to)
{
	struct blk_trace *bt = q->blk_trace;
	struct blk_io_trace_remap r;

	if (likely(!bt))
		return;

	r.device = cpu_to_be32(dev);
	r.device_from = cpu_to_be32(bio->bi_bdev->bd_dev);
	r.sector = cpu_to_be64(to);

	__blk_add_trace(bt, from, bio->bi_size, bio->bi_rw, BLK_TA_REMAP,
			!bio_flagged(bio, BIO_UPTODATE), sizeof(r), &r);
}

/**
 * blk_add_driver_data - Add binary message with driver-specific data
 * @q:		queue the io is for
 * @rq:		io request
 * @data:	driver-specific data
 * @len:	length of driver-specific data
 *
 * Description:
 *     Some drivers might want to write driver-specific data per request.
 *
 **/
void blk_add_driver_data(struct request_queue *q,
			 struct request *rq,
			 void *data, size_t len)
{
	struct blk_trace *bt = q->blk_trace;

	if (likely(!bt))
		return;

	if (blk_pc_request(rq))
		__blk_add_trace(bt, 0, rq->data_len, 0, BLK_TA_DRV_DATA,
				rq->errors, len, data);
	else
		__blk_add_trace(bt, rq->hard_sector, rq->hard_nr_sectors << 9,
				0, BLK_TA_DRV_DATA, rq->errors, len, data);
}
EXPORT_SYMBOL_GPL(blk_add_driver_data);

static void blk_register_tracepoints(void)
{
	int ret;

	ret = register_trace_block_rq_abort(blk_add_trace_rq_abort);
	WARN_ON(ret);
	ret = register_trace_block_rq_insert(blk_add_trace_rq_insert);
	WARN_ON(ret);
	ret = register_trace_block_rq_issue(blk_add_trace_rq_issue);
	WARN_ON(ret);
	ret = register_trace_block_rq_requeue(blk_add_trace_rq_requeue);
	WARN_ON(ret);
	ret = register_trace_block_rq_complete(blk_add_trace_rq_complete);
	WARN_ON(ret);
	ret = register_trace_block_bio_bounce(blk_add_trace_bio_bounce);
	WARN_ON(ret);
	ret = register_trace_block_bio_complete(blk_add_trace_bio_complete);
	WARN_ON(ret);
	ret = register_trace_block_bio_backmerge(blk_add_trace_bio_backmerge);
	WARN_ON(ret);
	ret = register_trace_block_bio_frontmerge(blk_add_trace_bio_frontmerge);
	WARN_ON(ret);
	ret = register_trace_block_bio_queue(blk_add_trace_bio_queue);
	WARN_ON(ret);
	ret = register_trace_block_getrq(blk_add_trace_getrq);
	WARN_ON(ret);
	ret = register_trace_block_sleeprq(blk_add_trace_sleeprq);
	WARN_ON(ret);
	ret = register_trace_block_plug(blk_add_trace_plug);
	WARN_ON(ret);
	ret = register_trace_block_unplug_timer(blk_add_trace_unplug_timer);
	WARN_ON(ret);
	ret = register_trace_block_unplug_io(blk_add_trace_unplug_io);
	WARN_ON(ret);
	ret = register_trace_block_split(blk_add_trace_split);
	WARN_ON(ret);
	ret = register_trace_block_remap(blk_add_trace_remap);
	WARN_ON(ret);
}

static void blk_unregister_tracepoints(void)
{
	unregister_trace_block_remap(blk_add_trace_remap);
	unregister_trace_block_split(blk_add_trace_split);
	unregister_trace_block_unplug_io(blk_add_trace_unplug_io);
	unregister_trace_block_unplug_timer(blk_add_trace_unplug_timer);
	unregister_trace_block_plug(blk_add_trace_plug);
	unregister_trace_block_sleeprq(blk_add_trace_sleeprq);
	unregister_trace_block_getrq(blk_add_trace_getrq);
	unregister_trace_block_bio_queue(blk_add_trace_bio_queue);
	unregister_trace_block_bio_frontmerge(blk_add_trace_bio_frontmerge);
	unregister_trace_block_bio_backmerge(blk_add_trace_bio_backmerge);
	unregister_trace_block_bio_complete(blk_add_trace_bio_complete);
	unregister_trace_block_bio_bounce(blk_add_trace_bio_bounce);
	unregister_trace_block_rq_complete(blk_add_trace_rq_complete);
	unregister_trace_block_rq_requeue(blk_add_trace_rq_requeue);
	unregister_trace_block_rq_issue(blk_add_trace_rq_issue);
	unregister_trace_block_rq_insert(blk_add_trace_rq_insert);
	unregister_trace_block_rq_abort(blk_add_trace_rq_abort);

	tracepoint_synchronize_unregister();
}

/*
 * struct blk_io_tracer formatting routines
 */

static void fill_rwbs(char *rwbs, const struct blk_io_trace *t)
{
	int i = 0;

	if (t->action & BLK_TC_DISCARD)
		rwbs[i++] = 'D';
	else if (t->action & BLK_TC_WRITE)
		rwbs[i++] = 'W';
	else if (t->bytes)
		rwbs[i++] = 'R';
	else
		rwbs[i++] = 'N';

	if (t->action & BLK_TC_AHEAD)
		rwbs[i++] = 'A';
	if (t->action & BLK_TC_BARRIER)
		rwbs[i++] = 'B';
	if (t->action & BLK_TC_SYNC)
		rwbs[i++] = 'S';
	if (t->action & BLK_TC_META)
		rwbs[i++] = 'M';

	rwbs[i] = '\0';
}

static inline
const struct blk_io_trace *te_blk_io_trace(const struct trace_entry *ent)
{
	return (const struct blk_io_trace *)ent;
}

static inline const void *pdu_start(const struct trace_entry *ent)
{
	return te_blk_io_trace(ent) + 1;
}

static inline u32 t_sec(const struct trace_entry *ent)
{
	return te_blk_io_trace(ent)->bytes >> 9;
}

static inline unsigned long long t_sector(const struct trace_entry *ent)
{
	return te_blk_io_trace(ent)->sector;
}

static inline __u16 t_error(const struct trace_entry *ent)
{
	return te_blk_io_trace(ent)->sector;
}

static __u64 get_pdu_int(const struct trace_entry *ent)
{
	const __u64 *val = pdu_start(ent);
	return be64_to_cpu(*val);
}

static void get_pdu_remap(const struct trace_entry *ent,
			  struct blk_io_trace_remap *r)
{
	const struct blk_io_trace_remap *__r = pdu_start(ent);
	__u64 sector = __r->sector;

	r->device = be32_to_cpu(__r->device);
	r->device_from = be32_to_cpu(__r->device_from);
	r->sector = be64_to_cpu(sector);
}

static int blk_log_action_iter(struct trace_iterator *iter, const char *act)
{
	char rwbs[6];
	unsigned long long ts  = ns2usecs(iter->ts);
	unsigned long usec_rem = do_div(ts, USEC_PER_SEC);
	unsigned secs	       = (unsigned long)ts;
	const struct trace_entry *ent = iter->ent;
	const struct blk_io_trace *t = (const struct blk_io_trace *)ent;

	fill_rwbs(rwbs, t);

	return trace_seq_printf(&iter->seq,
				"%3d,%-3d %2d %5d.%06lu %5u %2s %3s ",
				MAJOR(t->device), MINOR(t->device), iter->cpu,
				secs, usec_rem, ent->pid, act, rwbs);
}

static int blk_log_action_seq(struct trace_seq *s, const struct blk_io_trace *t,
			      const char *act)
{
	char rwbs[6];
	fill_rwbs(rwbs, t);
	return trace_seq_printf(s, "%3d,%-3d %2s %3s ",
				MAJOR(t->device), MINOR(t->device), act, rwbs);
}

static int blk_log_generic(struct trace_seq *s, const struct trace_entry *ent)
{
	char cmd[TASK_COMM_LEN];

	trace_find_cmdline(ent->pid, cmd);

	if (t_sec(ent))
		return trace_seq_printf(s, "%llu + %u [%s]\n",
					t_sector(ent), t_sec(ent), cmd);
	return trace_seq_printf(s, "[%s]\n", cmd);
}

static int blk_log_with_error(struct trace_seq *s,
			      const struct trace_entry *ent)
{
	if (t_sec(ent))
		return trace_seq_printf(s, "%llu + %u [%d]\n", t_sector(ent),
					t_sec(ent), t_error(ent));
	return trace_seq_printf(s, "%llu [%d]\n", t_sector(ent), t_error(ent));
}

static int blk_log_remap(struct trace_seq *s, const struct trace_entry *ent)
{
	struct blk_io_trace_remap r = { .device = 0, };

	get_pdu_remap(ent, &r);
	return trace_seq_printf(s, "%llu + %u <- (%d,%d) %llu\n",
			       t_sector(ent),
			       t_sec(ent), MAJOR(r.device), MINOR(r.device),
			       (unsigned long long)r.sector);
}

static int blk_log_plug(struct trace_seq *s, const struct trace_entry *ent)
{
	char cmd[TASK_COMM_LEN];

	trace_find_cmdline(ent->pid, cmd);

	return trace_seq_printf(s, "[%s]\n", cmd);
}

static int blk_log_unplug(struct trace_seq *s, const struct trace_entry *ent)
{
	char cmd[TASK_COMM_LEN];

	trace_find_cmdline(ent->pid, cmd);

	return trace_seq_printf(s, "[%s] %llu\n", cmd, get_pdu_int(ent));
}

static int blk_log_split(struct trace_seq *s, const struct trace_entry *ent)
{
	char cmd[TASK_COMM_LEN];

	trace_find_cmdline(ent->pid, cmd);

	return trace_seq_printf(s, "%llu / %llu [%s]\n", t_sector(ent),
				get_pdu_int(ent), cmd);
}

/*
 * struct tracer operations
 */

static void blk_tracer_print_header(struct seq_file *m)
{
	if (!(blk_tracer_flags.val & TRACE_BLK_OPT_CLASSIC))
		return;
	seq_puts(m, "# DEV   CPU TIMESTAMP     PID ACT FLG\n"
		    "#  |     |     |           |   |   |\n");
}

static void blk_tracer_start(struct trace_array *tr)
{
	if (atomic_add_return(1, &blk_probes_ref) == 1)
		blk_register_tracepoints();
	trace_flags &= ~TRACE_ITER_CONTEXT_INFO;
}

static int blk_tracer_init(struct trace_array *tr)
{
	blk_tr = tr;
	blk_tracer_start(tr);
	blk_tracer_enabled = true;
	return 0;
}

static void blk_tracer_stop(struct trace_array *tr)
{
	trace_flags |= TRACE_ITER_CONTEXT_INFO;
	if (atomic_dec_and_test(&blk_probes_ref))
		blk_unregister_tracepoints();
}

static void blk_tracer_reset(struct trace_array *tr)
{
	if (!atomic_read(&blk_probes_ref))
		return;

	blk_tracer_enabled = false;
	blk_tracer_stop(tr);
}

static struct {
	const char *act[2];
	int	   (*print)(struct trace_seq *s, const struct trace_entry *ent);
} what2act[] __read_mostly = {
	[__BLK_TA_QUEUE]	= {{  "Q", "queue" },	   blk_log_generic },
	[__BLK_TA_BACKMERGE]	= {{  "M", "backmerge" },  blk_log_generic },
	[__BLK_TA_FRONTMERGE]	= {{  "F", "frontmerge" }, blk_log_generic },
	[__BLK_TA_GETRQ]	= {{  "G", "getrq" },	   blk_log_generic },
	[__BLK_TA_SLEEPRQ]	= {{  "S", "sleeprq" },	   blk_log_generic },
	[__BLK_TA_REQUEUE]	= {{  "R", "requeue" },	   blk_log_with_error },
	[__BLK_TA_ISSUE]	= {{  "D", "issue" },	   blk_log_generic },
	[__BLK_TA_COMPLETE]	= {{  "C", "complete" },   blk_log_with_error },
	[__BLK_TA_PLUG]		= {{  "P", "plug" },	   blk_log_plug },
	[__BLK_TA_UNPLUG_IO]	= {{  "U", "unplug_io" },  blk_log_unplug },
	[__BLK_TA_UNPLUG_TIMER]	= {{ "UT", "unplug_timer" }, blk_log_unplug },
	[__BLK_TA_INSERT]	= {{  "I", "insert" },	   blk_log_generic },
	[__BLK_TA_SPLIT]	= {{  "X", "split" },	   blk_log_split },
	[__BLK_TA_BOUNCE]	= {{  "B", "bounce" },	   blk_log_generic },
	[__BLK_TA_REMAP]	= {{  "A", "remap" },	   blk_log_remap },
};

static enum print_line_t blk_trace_event_print(struct trace_iterator *iter,
					       int flags)
{
	struct trace_seq *s = &iter->seq;
	const struct blk_io_trace *t = (struct blk_io_trace *)iter->ent;
	const u16 what = t->action & ((1 << BLK_TC_SHIFT) - 1);
	int ret;

	if (!trace_print_context(iter))
		return TRACE_TYPE_PARTIAL_LINE;

	if (unlikely(what == 0 || what > ARRAY_SIZE(what2act)))
		ret = trace_seq_printf(s, "Bad pc action %x\n", what);
	else {
		const bool long_act = !!(trace_flags & TRACE_ITER_VERBOSE);
		ret = blk_log_action_seq(s, t, what2act[what].act[long_act]);
		if (ret)
			ret = what2act[what].print(s, iter->ent);
	}

	return ret ? TRACE_TYPE_HANDLED : TRACE_TYPE_PARTIAL_LINE;
}

static int blk_trace_synthesize_old_trace(struct trace_iterator *iter)
{
	struct trace_seq *s = &iter->seq;
	struct blk_io_trace *t = (struct blk_io_trace *)iter->ent;
	const int offset = offsetof(struct blk_io_trace, sector);
	struct blk_io_trace old = {
		.magic	  = BLK_IO_TRACE_MAGIC | BLK_IO_TRACE_VERSION,
		.time     = ns2usecs(iter->ts),
	};

	if (!trace_seq_putmem(s, &old, offset))
		return 0;
	return trace_seq_putmem(s, &t->sector,
				sizeof(old) - offset + t->pdu_len);
}

static enum print_line_t
blk_trace_event_print_binary(struct trace_iterator *iter, int flags)
{
	return blk_trace_synthesize_old_trace(iter) ?
			TRACE_TYPE_HANDLED : TRACE_TYPE_PARTIAL_LINE;
}

static enum print_line_t blk_tracer_print_line(struct trace_iterator *iter)
{
	const struct blk_io_trace *t;
	u16 what;
	int ret;

	if (!(blk_tracer_flags.val & TRACE_BLK_OPT_CLASSIC))
		return TRACE_TYPE_UNHANDLED;

	t = (const struct blk_io_trace *)iter->ent;
	what = t->action & ((1 << BLK_TC_SHIFT) - 1);

	if (unlikely(what == 0 || what > ARRAY_SIZE(what2act)))
		ret = trace_seq_printf(&iter->seq, "Bad pc action %x\n", what);
	else {
		const bool long_act = !!(trace_flags & TRACE_ITER_VERBOSE);
		ret = blk_log_action_iter(iter, what2act[what].act[long_act]);
		if (ret)
			ret = what2act[what].print(&iter->seq, iter->ent);
	}

	return ret ? TRACE_TYPE_HANDLED : TRACE_TYPE_PARTIAL_LINE;
}

static struct tracer blk_tracer __read_mostly = {
	.name		= "blk",
	.init		= blk_tracer_init,
	.reset		= blk_tracer_reset,
	.start		= blk_tracer_start,
	.stop		= blk_tracer_stop,
	.print_header	= blk_tracer_print_header,
	.print_line	= blk_tracer_print_line,
	.flags		= &blk_tracer_flags,
};

static struct trace_event trace_blk_event = {
	.type		= TRACE_BLK,
	.trace		= blk_trace_event_print,
	.binary		= blk_trace_event_print_binary,
};

static int __init init_blk_tracer(void)
{
	if (!register_ftrace_event(&trace_blk_event)) {
		pr_warning("Warning: could not register block events\n");
		return 1;
	}

	if (register_tracer(&blk_tracer) != 0) {
		pr_warning("Warning: could not register the block tracer\n");
		unregister_ftrace_event(&trace_blk_event);
		return 1;
	}

	return 0;
}

device_initcall(init_blk_tracer);

static int blk_trace_remove_queue(struct request_queue *q)
{
	struct blk_trace *bt;

	bt = xchg(&q->blk_trace, NULL);
	if (bt == NULL)
		return -EINVAL;

	kfree(bt);
	return 0;
}

/*
 * Setup everything required to start tracing
 */
static int blk_trace_setup_queue(struct request_queue *q, dev_t dev)
{
	struct blk_trace *old_bt, *bt = NULL;
	int ret;

	ret = -ENOMEM;
	bt = kzalloc(sizeof(*bt), GFP_KERNEL);
	if (!bt)
		goto err;

	bt->dev = dev;
	bt->act_mask = (u16)-1;
	bt->end_lba = -1ULL;
	bt->trace_state = Blktrace_running;

	old_bt = xchg(&q->blk_trace, bt);
	if (old_bt != NULL) {
		(void)xchg(&q->blk_trace, old_bt);
		kfree(bt);
		ret = -EBUSY;
	}
	return 0;
err:
	return ret;
}

/*
 * sysfs interface to enable and configure tracing
 */

static ssize_t sysfs_blk_trace_enable_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	struct hd_struct *p = dev_to_part(dev);
	struct block_device *bdev;
	ssize_t ret = -ENXIO;

	lock_kernel();
	bdev = bdget(part_devt(p));
	if (bdev != NULL) {
		struct request_queue *q = bdev_get_queue(bdev);

		if (q != NULL) {
			mutex_lock(&bdev->bd_mutex);
			ret = sprintf(buf, "%u\n", !!q->blk_trace);
			mutex_unlock(&bdev->bd_mutex);
		}

		bdput(bdev);
	}

	unlock_kernel();
	return ret;
}

static ssize_t sysfs_blk_trace_enable_store(struct device *dev,
					    struct device_attribute *attr,
					    const char *buf, size_t count)
{
	struct block_device *bdev;
	struct request_queue *q;
	struct hd_struct *p;
	int value;
	ssize_t ret = -ENXIO;

	if (count == 0 || sscanf(buf, "%d", &value) != 1)
		goto out;

	lock_kernel();
	p = dev_to_part(dev);
	bdev = bdget(part_devt(p));
	if (bdev == NULL)
		goto out_unlock_kernel;

	q = bdev_get_queue(bdev);
	if (q == NULL)
		goto out_bdput;

	mutex_lock(&bdev->bd_mutex);
	if (value)
		ret = blk_trace_setup_queue(q, bdev->bd_dev);
	else
		ret = blk_trace_remove_queue(q);
	mutex_unlock(&bdev->bd_mutex);

	if (ret == 0)
		ret = count;
out_bdput:
	bdput(bdev);
out_unlock_kernel:
	unlock_kernel();
out:
	return ret;
}

static ssize_t sysfs_blk_trace_attr_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf);
static ssize_t sysfs_blk_trace_attr_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count);
#define BLK_TRACE_DEVICE_ATTR(_name) \
	DEVICE_ATTR(_name, S_IRUGO | S_IWUSR, \
		    sysfs_blk_trace_attr_show, \
		    sysfs_blk_trace_attr_store)

static DEVICE_ATTR(enable, S_IRUGO | S_IWUSR,
		   sysfs_blk_trace_enable_show, sysfs_blk_trace_enable_store);
static BLK_TRACE_DEVICE_ATTR(act_mask);
static BLK_TRACE_DEVICE_ATTR(pid);
static BLK_TRACE_DEVICE_ATTR(start_lba);
static BLK_TRACE_DEVICE_ATTR(end_lba);

static struct attribute *blk_trace_attrs[] = {
	&dev_attr_enable.attr,
	&dev_attr_act_mask.attr,
	&dev_attr_pid.attr,
	&dev_attr_start_lba.attr,
	&dev_attr_end_lba.attr,
	NULL
};

struct attribute_group blk_trace_attr_group = {
	.name  = "trace",
	.attrs = blk_trace_attrs,
};

static int blk_str2act_mask(const char *str)
{
	int mask = 0;
	char *copy = kstrdup(str, GFP_KERNEL), *s;

	if (copy == NULL)
		return -ENOMEM;

	s = strstrip(copy);

	while (1) {
		char *sep = strchr(s, ',');

		if (sep != NULL)
			*sep = '\0';

		if (strcasecmp(s, "barrier") == 0)
			mask |= BLK_TC_BARRIER;
		else if (strcasecmp(s, "complete") == 0)
			mask |= BLK_TC_COMPLETE;
		else if (strcasecmp(s, "fs") == 0)
			mask |= BLK_TC_FS;
		else if (strcasecmp(s, "issue") == 0)
			mask |= BLK_TC_ISSUE;
		else if (strcasecmp(s, "pc") == 0)
			mask |= BLK_TC_PC;
		else if (strcasecmp(s, "queue") == 0)
			mask |= BLK_TC_QUEUE;
		else if (strcasecmp(s, "read") == 0)
			mask |= BLK_TC_READ;
		else if (strcasecmp(s, "requeue") == 0)
			mask |= BLK_TC_REQUEUE;
		else if (strcasecmp(s, "sync") == 0)
			mask |= BLK_TC_SYNC;
		else if (strcasecmp(s, "write") == 0)
			mask |= BLK_TC_WRITE;

		if (sep == NULL)
			break;

		s = sep + 1;
	}
	kfree(copy);

	return mask;
}

static ssize_t sysfs_blk_trace_attr_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct hd_struct *p = dev_to_part(dev);
	struct request_queue *q;
	struct block_device *bdev;
	ssize_t ret = -ENXIO;

	lock_kernel();
	bdev = bdget(part_devt(p));
	if (bdev == NULL)
		goto out_unlock_kernel;

	q = bdev_get_queue(bdev);
	if (q == NULL)
		goto out_bdput;
	mutex_lock(&bdev->bd_mutex);
	if (q->blk_trace == NULL)
		ret = sprintf(buf, "disabled\n");
	else if (attr == &dev_attr_act_mask)
		ret = sprintf(buf, "%#x\n", q->blk_trace->act_mask);
	else if (attr == &dev_attr_pid)
		ret = sprintf(buf, "%u\n", q->blk_trace->pid);
	else if (attr == &dev_attr_start_lba)
		ret = sprintf(buf, "%llu\n", q->blk_trace->start_lba);
	else if (attr == &dev_attr_end_lba)
		ret = sprintf(buf, "%llu\n", q->blk_trace->end_lba);
	mutex_unlock(&bdev->bd_mutex);
out_bdput:
	bdput(bdev);
out_unlock_kernel:
	unlock_kernel();
	return ret;
}

static ssize_t sysfs_blk_trace_attr_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
{
	struct block_device *bdev;
	struct request_queue *q;
	struct hd_struct *p;
	u64 value;
	ssize_t ret = -ENXIO;

	if (count == 0)
		goto out;

	if (attr == &dev_attr_act_mask) {
		if (sscanf(buf, "%llx", &value) != 1) {
			/* Assume it is a list of trace category names */
			value = blk_str2act_mask(buf);
			if (value < 0)
				goto out;
		}
	} else if (sscanf(buf, "%llu", &value) != 1)
		goto out;

	lock_kernel();
	p = dev_to_part(dev);
	bdev = bdget(part_devt(p));
	if (bdev == NULL)
		goto out_unlock_kernel;

	q = bdev_get_queue(bdev);
	if (q == NULL)
		goto out_bdput;

	mutex_lock(&bdev->bd_mutex);
	ret = 0;
	if (q->blk_trace == NULL)
		ret = blk_trace_setup_queue(q, bdev->bd_dev);

	if (ret == 0) {
		if (attr == &dev_attr_act_mask)
			q->blk_trace->act_mask = value;
		else if (attr == &dev_attr_pid)
			q->blk_trace->pid = value;
		else if (attr == &dev_attr_start_lba)
			q->blk_trace->start_lba = value;
		else if (attr == &dev_attr_end_lba)
			q->blk_trace->end_lba = value;
		ret = count;
	}
	mutex_unlock(&bdev->bd_mutex);
out_bdput:
	bdput(bdev);
out_unlock_kernel:
	unlock_kernel();
out:
	return ret;
}
