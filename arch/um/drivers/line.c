/*
 * Copyright (C) 2001 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 * Licensed under the GPL
 */

#include "linux/irqreturn.h"
#include "linux/kd.h"
#include "linux/sched.h"
#include "linux/slab.h"
#include "chan.h"
#include "irq_kern.h"
#include "irq_user.h"
#include "kern_util.h"
#include "os.h"

#define LINE_BUFSIZE 4096

static irqreturn_t line_interrupt(int irq, void *data)
{
	struct chan *chan = data;
	struct line *line = chan->line;

	if (line)
		chan_interrupt(&line->chan_list, &line->task, line->tty, irq);
	return IRQ_HANDLED;
}

static void line_timer_cb(struct work_struct *work)
{
	struct line *line = container_of(work, struct line, task.work);

	if (!line->throttled)
		chan_interrupt(&line->chan_list, &line->task, line->tty,
			       line->driver->read_irq);
}

/*
 * Returns the free space inside the ring buffer of this line.
 *
 * Should be called while holding line->lock (this does not modify data).
 */
static int write_room(struct line *line)
{
	int n;

	if (line->buffer == NULL)
		return LINE_BUFSIZE - 1;

	/* This is for the case where the buffer is wrapped! */
	n = line->head - line->tail;

	if (n <= 0)
		n += LINE_BUFSIZE; /* The other case */
	return n - 1;
}

int line_write_room(struct tty_struct *tty)
{
	struct line *line = tty->driver_data;
	unsigned long flags;
	int room;

	spin_lock_irqsave(&line->lock, flags);
	room = write_room(line);
	spin_unlock_irqrestore(&line->lock, flags);

	return room;
}

int line_chars_in_buffer(struct tty_struct *tty)
{
	struct line *line = tty->driver_data;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&line->lock, flags);
	/* write_room subtracts 1 for the needed NULL, so we readd it.*/
	ret = LINE_BUFSIZE - (write_room(line) + 1);
	spin_unlock_irqrestore(&line->lock, flags);

	return ret;
}

/*
 * This copies the content of buf into the circular buffer associated with
 * this line.
 * The return value is the number of characters actually copied, i.e. the ones
 * for which there was space: this function is not supposed to ever flush out
 * the circular buffer.
 *
 * Must be called while holding line->lock!
 */
static int buffer_data(struct line *line, const char *buf, int len)
{
	int end, room;

	if (line->buffer == NULL) {
		line->buffer = kmalloc(LINE_BUFSIZE, GFP_ATOMIC);
		if (line->buffer == NULL) {
			printk(KERN_ERR "buffer_data - atomic allocation "
			       "failed\n");
			return 0;
		}
		line->head = line->buffer;
		line->tail = line->buffer;
	}

	room = write_room(line);
	len = (len > room) ? room : len;

	end = line->buffer + LINE_BUFSIZE - line->tail;

	if (len < end) {
		memcpy(line->tail, buf, len);
		line->tail += len;
	}
	else {
		/* The circular buffer is wrapping */
		memcpy(line->tail, buf, end);
		buf += end;
		memcpy(line->buffer, buf, len - end);
		line->tail = line->buffer + len - end;
	}

	return len;
}

/*
 * Flushes the ring buffer to the output channels. That is, write_chan is
 * called, passing it line->head as buffer, and an appropriate count.
 *
 * On exit, returns 1 when the buffer is empty,
 * 0 when the buffer is not empty on exit,
 * and -errno when an error occurred.
 *
 * Must be called while holding line->lock!*/
static int flush_buffer(struct line *line)
{
	int n, count;

	if ((line->buffer == NULL) || (line->head == line->tail))
		return 1;

	if (line->tail < line->head) {
		/* line->buffer + LINE_BUFSIZE is the end of the buffer! */
		count = line->buffer + LINE_BUFSIZE - line->head;

		n = write_chan(&line->chan_list, line->head, count,
			       line->driver->write_irq);
		if (n < 0)
			return n;
		if (n == count) {
			/*
			 * We have flushed from ->head to buffer end, now we
			 * must flush only from the beginning to ->tail.
			 */
			line->head = line->buffer;
		} else {
			line->head += n;
			return 0;
		}
	}

	count = line->tail - line->head;
	n = write_chan(&line->chan_list, line->head, count,
		       line->driver->write_irq);

	if (n < 0)
		return n;

	line->head += n;
	return line->head == line->tail;
}

void line_flush_buffer(struct tty_struct *tty)
{
	struct line *line = tty->driver_data;
	unsigned long flags;

	spin_lock_irqsave(&line->lock, flags);
	flush_buffer(line);
	spin_unlock_irqrestore(&line->lock, flags);
}

/*
 * We map both ->flush_chars and ->put_char (which go in pair) onto
 * ->flush_buffer and ->write. Hope it's not that bad.
 */
void line_flush_chars(struct tty_struct *tty)
{
	line_flush_buffer(tty);
}

int line_put_char(struct tty_struct *tty, unsigned char ch)
{
	return line_write(tty, &ch, sizeof(ch));
}

int line_write(struct tty_struct *tty, const unsigned char *buf, int len)
{
	struct line *line = tty->driver_data;
	unsigned long flags;
	int n, ret = 0;

	spin_lock_irqsave(&line->lock, flags);
	if (line->head != line->tail)
		ret = buffer_data(line, buf, len);
	else {
		n = write_chan(&line->chan_list, buf, len,
			       line->driver->write_irq);
		if (n < 0) {
			ret = n;
			goto out_up;
		}

		len -= n;
		ret += n;
		if (len > 0)
			ret += buffer_data(line, buf + n, len);
	}
out_up:
	spin_unlock_irqrestore(&line->lock, flags);
	return ret;
}

void line_set_termios(struct tty_struct *tty, struct ktermios * old)
{
	/* nothing */
}

static const struct {
	int  cmd;
	char *level;
	char *name;
} tty_ioctls[] = {
	/* don't print these, they flood the log ... */
	{ TCGETS,      NULL,       "TCGETS"      },
	{ TCSETS,      NULL,       "TCSETS"      },
	{ TCSETSW,     NULL,       "TCSETSW"     },
	{ TCFLSH,      NULL,       "TCFLSH"      },
	{ TCSBRK,      NULL,       "TCSBRK"      },

	/* general tty stuff */
	{ TCSETSF,     KERN_DEBUG, "TCSETSF"     },
	{ TCGETA,      KERN_DEBUG, "TCGETA"      },
	{ TIOCMGET,    KERN_DEBUG, "TIOCMGET"    },
	{ TCSBRKP,     KERN_DEBUG, "TCSBRKP"     },
	{ TIOCMSET,    KERN_DEBUG, "TIOCMSET"    },

	/* linux-specific ones */
	{ TIOCLINUX,   KERN_INFO,  "TIOCLINUX"   },
	{ KDGKBMODE,   KERN_INFO,  "KDGKBMODE"   },
	{ KDGKBTYPE,   KERN_INFO,  "KDGKBTYPE"   },
	{ KDSIGACCEPT, KERN_INFO,  "KDSIGACCEPT" },
};

int line_ioctl(struct tty_struct *tty, unsigned int cmd,
				unsigned long arg)
{
	int ret;
	int i;

	ret = 0;
	switch(cmd) {
#ifdef TIOCGETP
	case TIOCGETP:
	case TIOCSETP:
	case TIOCSETN:
#endif
#ifdef TIOCGETC
	case TIOCGETC:
	case TIOCSETC:
#endif
#ifdef TIOCGLTC
	case TIOCGLTC:
	case TIOCSLTC:
#endif
	/* Note: these are out of date as we now have TCGETS2 etc but this
	   whole lot should probably go away */
	case TCGETS:
	case TCSETSF:
	case TCSETSW:
	case TCSETS:
	case TCGETA:
	case TCSETAF:
	case TCSETAW:
	case TCSETA:
	case TCXONC:
	case TCFLSH:
	case TIOCOUTQ:
	case TIOCINQ:
	case TIOCGLCKTRMIOS:
	case TIOCSLCKTRMIOS:
	case TIOCPKT:
	case TIOCGSOFTCAR:
	case TIOCSSOFTCAR:
		return -ENOIOCTLCMD;
#if 0
	case TCwhatever:
		/* do something */
		break;
#endif
	default:
		for (i = 0; i < ARRAY_SIZE(tty_ioctls); i++)
			if (cmd == tty_ioctls[i].cmd)
				break;
		if (i == ARRAY_SIZE(tty_ioctls)) {
			printk(KERN_ERR "%s: %s: unknown ioctl: 0x%x\n",
			       __func__, tty->name, cmd);
		}
		ret = -ENOIOCTLCMD;
		break;
	}
	return ret;
}

void line_throttle(struct tty_struct *tty)
{
	struct line *line = tty->driver_data;

	deactivate_chan(&line->chan_list, line->driver->read_irq);
	line->throttled = 1;
}

void line_unthrottle(struct tty_struct *tty)
{
	struct line *line = tty->driver_data;

	line->throttled = 0;
	chan_interrupt(&line->chan_list, &line->task, tty,
		       line->driver->read_irq);

	/*
	 * Maybe there is enough stuff pending that calling the interrupt
	 * throttles us again.  In this case, line->throttled will be 1
	 * again and we shouldn't turn the interrupt back on.
	 */
	if (!line->throttled)
		reactivate_chan(&line->chan_list, line->driver->read_irq);
}

static irqreturn_t line_write_interrupt(int irq, void *data)
{
	struct chan *chan = data;
	struct line *line = chan->line;
	struct tty_struct *tty = line->tty;
	int err;

	/*
	 * Interrupts are disabled here because genirq keep irqs disabled when
	 * calling the action handler.
	 */

	spin_lock(&line->lock);
	err = flush_buffer(line);
	if (err == 0) {
		return IRQ_NONE;
	} else if (err < 0) {
		line->head = line->buffer;
		line->tail = line->buffer;
	}
	spin_unlock(&line->lock);

	if (tty == NULL)
		return IRQ_NONE;

	tty_wakeup(tty);
	return IRQ_HANDLED;
}

int line_setup_irq(int fd, int input, int output, struct line *line, void *data)
{
	const struct line_driver *driver = line->driver;
	int err = 0, flags = IRQF_SHARED | IRQF_SAMPLE_RANDOM;

	if (input)
		err = um_request_irq(driver->read_irq, fd, IRQ_READ,
				       line_interrupt, flags,
				       driver->read_irq_name, data);
	if (err)
		return err;
	if (output)
		err = um_request_irq(driver->write_irq, fd, IRQ_WRITE,
					line_write_interrupt, flags,
					driver->write_irq_name, data);
	line->have_irq = 1;
	return err;
}

/*
 * Normally, a driver like this can rely mostly on the tty layer
 * locking, particularly when it comes to the driver structure.
 * However, in this case, mconsole requests can come in "from the
 * side", and race with opens and closes.
 *
 * mconsole config requests will want to be sure the device isn't in
 * use, and get_config, open, and close will want a stable
 * configuration.  The checking and modification of the configuration
 * is done under a spinlock.  Checking whether the device is in use is
 * line->tty->count > 1, also under the spinlock.
 *
 * line->count serves to decide whether the device should be enabled or
 * disabled on the host.  If it's equal to 0, then we are doing the
 * first open or last close.  Otherwise, open and close just return.
 */

int line_open(struct line *lines, struct tty_struct *tty)
{
	struct line *line = &lines[tty->index];
	int err = -ENODEV;

	mutex_lock(&line->count_lock);
	if (!line->valid)
		goto out_unlock;

	err = 0;
	if (line->count++)
		goto out_unlock;

	BUG_ON(tty->driver_data);
	tty->driver_data = line;
	line->tty = tty;

	err = enable_chan(line);
	if (err) /* line_close() will be called by our caller */
		goto out_unlock;

	INIT_DELAYED_WORK(&line->task, line_timer_cb);

	if (!line->sigio) {
		chan_enable_winch(&line->chan_list, tty);
		line->sigio = 1;
	}

	chan_window_size(&line->chan_list, &tty->winsize.ws_row,
			 &tty->winsize.ws_col);
out_unlock:
	mutex_unlock(&line->count_lock);
	return err;
}

static void unregister_winch(struct tty_struct *tty);

void line_close(struct tty_struct *tty, struct file * filp)
{
	struct line *line = tty->driver_data;

	/*
	 * If line_open fails (and tty->driver_data is never set),
	 * tty_open will call line_close.  So just return in this case.
	 */
	if (line == NULL)
		return;

	/* We ignore the error anyway! */
	flush_buffer(line);

	mutex_lock(&line->count_lock);
	BUG_ON(!line->valid);

	if (--line->count)
		goto out_unlock;

	line->tty = NULL;
	tty->driver_data = NULL;

	if (line->sigio) {
		unregister_winch(tty);
		line->sigio = 0;
	}

out_unlock:
	mutex_unlock(&line->count_lock);
}

void close_lines(struct line *lines, int nlines)
{
	int i;

	for(i = 0; i < nlines; i++)
		close_chan(&lines[i].chan_list, 0);
}

int setup_one_line(struct line *lines, int n, char *init,
		   const struct chan_opts *opts, char **error_out)
{
	struct line *line = &lines[n];
	struct tty_driver *driver = line->driver->driver;
	int err = -EINVAL;

	mutex_lock(&line->count_lock);

	if (line->count) {
		*error_out = "Device is already open";
		goto out;
	}

	if (!strcmp(init, "none")) {
		if (line->valid) {
			line->valid = 0;
			kfree(line->init_str);
			tty_unregister_device(driver, n);
			parse_chan_pair(NULL, line, n, opts, error_out);
			err = 0;
		}
	} else {
		char *new = kstrdup(init, GFP_KERNEL);
		if (!new) {
			*error_out = "Failed to allocate memory";
			return -ENOMEM;
		}
		if (line->valid) {
			tty_unregister_device(driver, n);
			kfree(line->init_str);
		}
		line->init_str = new;
		line->valid = 1;
		err = parse_chan_pair(new, line, n, opts, error_out);
		if (!err) {
			struct device *d = tty_register_device(driver, n, NULL);
			if (IS_ERR(d)) {
				*error_out = "Failed to register device";
				err = PTR_ERR(d);
				parse_chan_pair(NULL, line, n, opts, error_out);
			}
		}
		if (err) {
			line->init_str = NULL;
			line->valid = 0;
			kfree(new);
		}
	}
out:
	mutex_unlock(&line->count_lock);
	return err;
}

/*
 * Common setup code for both startup command line and mconsole initialization.
 * @lines contains the array (of size @num) to modify;
 * @init is the setup string;
 * @error_out is an error string in the case of failure;
 */

int line_setup(char **conf, unsigned int num, char **def,
	       char *init, char *name)
{
	char *error;

	if (*init == '=') {
		/*
		 * We said con=/ssl= instead of con#=, so we are configuring all
		 * consoles at once.
		 */
		*def = init + 1;
	} else {
		char *end;
		unsigned n = simple_strtoul(init, &end, 0);

		if (*end != '=') {
			error = "Couldn't parse device number";
			goto out;
		}
		if (n >= num) {
			error = "Device number out of range";
			goto out;
		}
		conf[n] = end + 1;
	}
	return 0;

out:
	printk(KERN_ERR "Failed to set up %s with "
	       "configuration string \"%s\" : %s\n", name, init, error);
	return -EINVAL;
}

int line_config(struct line *lines, unsigned int num, char *str,
		const struct chan_opts *opts, char **error_out)
{
	char *end;
	int n;

	if (*str == '=') {
		*error_out = "Can't configure all devices from mconsole";
		return -EINVAL;
	}

	n = simple_strtoul(str, &end, 0);
	if (*end++ != '=') {
		*error_out = "Couldn't parse device number";
		return -EINVAL;
	}
	if (n >= num) {
		*error_out = "Device number out of range";
		return -EINVAL;
	}

	return setup_one_line(lines, n, end, opts, error_out);
}

int line_get_config(char *name, struct line *lines, unsigned int num, char *str,
		    int size, char **error_out)
{
	struct line *line;
	char *end;
	int dev, n = 0;

	dev = simple_strtoul(name, &end, 0);
	if ((*end != '\0') || (end == name)) {
		*error_out = "line_get_config failed to parse device number";
		return 0;
	}

	if ((dev < 0) || (dev >= num)) {
		*error_out = "device number out of range";
		return 0;
	}

	line = &lines[dev];

	mutex_lock(&line->count_lock);
	if (!line->valid)
		CONFIG_CHUNK(str, size, n, "none", 1);
	else if (line->tty == NULL)
		CONFIG_CHUNK(str, size, n, line->init_str, 1);
	else n = chan_config_string(&line->chan_list, str, size, error_out);
	mutex_unlock(&line->count_lock);

	return n;
}

int line_id(char **str, int *start_out, int *end_out)
{
	char *end;
	int n;

	n = simple_strtoul(*str, &end, 0);
	if ((*end != '\0') || (end == *str))
		return -1;

	*str = end;
	*start_out = n;
	*end_out = n;
	return n;
}

int line_remove(struct line *lines, unsigned int num, int n, char **error_out)
{
	if (n >= num) {
		*error_out = "Device number out of range";
		return -EINVAL;
	}
	return setup_one_line(lines, n, "none", NULL, error_out);
}

int register_lines(struct line_driver *line_driver,
		   const struct tty_operations *ops,
		   struct line *lines, int nlines)
{
	struct tty_driver *driver = alloc_tty_driver(nlines);
	int err;
	int i;

	if (!driver)
		return -ENOMEM;

	driver->driver_name = line_driver->name;
	driver->name = line_driver->device_name;
	driver->major = line_driver->major;
	driver->minor_start = line_driver->minor_start;
	driver->type = line_driver->type;
	driver->subtype = line_driver->subtype;
	driver->flags = TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV;
	driver->init_termios = tty_std_termios;
	
	for (i = 0; i < nlines; i++) {
		spin_lock_init(&lines[i].lock);
		mutex_init(&lines[i].count_lock);
		lines[i].driver = line_driver;
		INIT_LIST_HEAD(&lines[i].chan_list);
	}
	tty_set_operations(driver, ops);

	err = tty_register_driver(driver);
	if (err) {
		printk(KERN_ERR "register_lines : can't register %s driver\n",
		       line_driver->name);
		put_tty_driver(driver);
		return err;
	}

	line_driver->driver = driver;
	mconsole_register_dev(&line_driver->mc);
	return 0;
}

static DEFINE_SPINLOCK(winch_handler_lock);
static LIST_HEAD(winch_handlers);

struct winch {
	struct list_head list;
	int fd;
	int tty_fd;
	int pid;
	struct tty_struct *tty;
	unsigned long stack;
	struct work_struct work;
};

static void __free_winch(struct work_struct *work)
{
	struct winch *winch = container_of(work, struct winch, work);
	free_irq(WINCH_IRQ, winch);

	if (winch->pid != -1)
		os_kill_process(winch->pid, 1);
	if (winch->stack != 0)
		free_stack(winch->stack, 0);
	kfree(winch);
}

static void free_winch(struct winch *winch)
{
	int fd = winch->fd;
	winch->fd = -1;
	if (fd != -1)
		os_close_file(fd);
	list_del(&winch->list);
	__free_winch(&winch->work);
}

static irqreturn_t winch_interrupt(int irq, void *data)
{
	struct winch *winch = data;
	struct tty_struct *tty;
	struct line *line;
	int fd = winch->fd;
	int err;
	char c;

	if (fd != -1) {
		err = generic_read(fd, &c, NULL);
		if (err < 0) {
			if (err != -EAGAIN) {
				winch->fd = -1;
				list_del(&winch->list);
				os_close_file(fd);
				printk(KERN_ERR "winch_interrupt : "
				       "read failed, errno = %d\n", -err);
				printk(KERN_ERR "fd %d is losing SIGWINCH "
				       "support\n", winch->tty_fd);
				INIT_WORK(&winch->work, __free_winch);
				schedule_work(&winch->work);
				return IRQ_HANDLED;
			}
			goto out;
		}
	}
	tty = winch->tty;
	if (tty != NULL) {
		line = tty->driver_data;
		if (line != NULL) {
			chan_window_size(&line->chan_list, &tty->winsize.ws_row,
					 &tty->winsize.ws_col);
			kill_pgrp(tty->pgrp, SIGWINCH, 1);
		}
	}
 out:
	if (winch->fd != -1)
		reactivate_fd(winch->fd, WINCH_IRQ);
	return IRQ_HANDLED;
}

void register_winch_irq(int fd, int tty_fd, int pid, struct tty_struct *tty,
			unsigned long stack)
{
	struct winch *winch;

	winch = kmalloc(sizeof(*winch), GFP_KERNEL);
	if (winch == NULL) {
		printk(KERN_ERR "register_winch_irq - kmalloc failed\n");
		goto cleanup;
	}

	*winch = ((struct winch) { .list  	= LIST_HEAD_INIT(winch->list),
				   .fd  	= fd,
				   .tty_fd 	= tty_fd,
				   .pid  	= pid,
				   .tty 	= tty,
				   .stack	= stack });

	if (um_request_irq(WINCH_IRQ, fd, IRQ_READ, winch_interrupt,
			   IRQF_SHARED | IRQF_SAMPLE_RANDOM,
			   "winch", winch) < 0) {
		printk(KERN_ERR "register_winch_irq - failed to register "
		       "IRQ\n");
		goto out_free;
	}

	spin_lock(&winch_handler_lock);
	list_add(&winch->list, &winch_handlers);
	spin_unlock(&winch_handler_lock);

	return;

 out_free:
	kfree(winch);
 cleanup:
	os_kill_process(pid, 1);
	os_close_file(fd);
	if (stack != 0)
		free_stack(stack, 0);
}

static void unregister_winch(struct tty_struct *tty)
{
	struct list_head *ele, *next;
	struct winch *winch;

	spin_lock(&winch_handler_lock);

	list_for_each_safe(ele, next, &winch_handlers) {
		winch = list_entry(ele, struct winch, list);
		if (winch->tty == tty) {
			free_winch(winch);
			break;
		}
	}
	spin_unlock(&winch_handler_lock);
}

static void winch_cleanup(void)
{
	struct list_head *ele, *next;
	struct winch *winch;

	spin_lock(&winch_handler_lock);

	list_for_each_safe(ele, next, &winch_handlers) {
		winch = list_entry(ele, struct winch, list);
		free_winch(winch);
	}

	spin_unlock(&winch_handler_lock);
}
__uml_exitcall(winch_cleanup);

char *add_xterm_umid(char *base)
{
	char *umid, *title;
	int len;

	umid = get_umid();
	if (*umid == '\0')
		return base;

	len = strlen(base) + strlen(" ()") + strlen(umid) + 1;
	title = kmalloc(len, GFP_KERNEL);
	if (title == NULL) {
		printk(KERN_ERR "Failed to allocate buffer for xterm title\n");
		return base;
	}

	snprintf(title, len, "%s (%s)", base, umid);
	return title;
}
