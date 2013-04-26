/*
   rbd.c -- Export ceph rados objects as a Linux block device


   based on drivers/block/osdblk.c:

   Copyright 2009 Red Hat, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; see the file COPYING.  If not, write to
   the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.



   For usage instructions, please refer to:

                 Documentation/ABI/testing/sysfs-bus-rbd

 */

#include <linux/ceph/libceph.h>
#include <linux/ceph/osd_client.h>
#include <linux/ceph/mon_client.h>
#include <linux/ceph/decode.h>
#include <linux/parser.h>

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/blkdev.h>

#include "rbd_types.h"

#define RBD_DEBUG	/* Activate rbd_assert() calls */

/*
 * The basic unit of block I/O is a sector.  It is interpreted in a
 * number of contexts in Linux (blk, bio, genhd), but the default is
 * universally 512 bytes.  These symbols are just slightly more
 * meaningful than the bare numbers they represent.
 */
#define	SECTOR_SHIFT	9
#define	SECTOR_SIZE	(1ULL << SECTOR_SHIFT)

#define RBD_DRV_NAME "rbd"
#define RBD_DRV_NAME_LONG "rbd (rados block device)"

#define RBD_MINORS_PER_MAJOR	256		/* max minors per blkdev */

#define RBD_SNAP_DEV_NAME_PREFIX	"snap_"
#define RBD_MAX_SNAP_NAME_LEN	\
			(NAME_MAX - (sizeof (RBD_SNAP_DEV_NAME_PREFIX) - 1))

#define RBD_MAX_SNAP_COUNT	510	/* allows max snapc to fit in 4KB */

#define RBD_SNAP_HEAD_NAME	"-"

/* This allows a single page to hold an image name sent by OSD */
#define RBD_IMAGE_NAME_LEN_MAX	(PAGE_SIZE - sizeof (__le32) - 1)
#define RBD_IMAGE_ID_LEN_MAX	64

#define RBD_OBJ_PREFIX_LEN_MAX	64

/* Feature bits */

#define RBD_FEATURE_LAYERING	(1<<0)
#define RBD_FEATURE_STRIPINGV2	(1<<1)
#define RBD_FEATURES_ALL \
	    (RBD_FEATURE_LAYERING | RBD_FEATURE_STRIPINGV2)

/* Features supported by this (client software) implementation. */

#define RBD_FEATURES_SUPPORTED	(RBD_FEATURES_ALL)

/*
 * An RBD device name will be "rbd#", where the "rbd" comes from
 * RBD_DRV_NAME above, and # is a unique integer identifier.
 * MAX_INT_FORMAT_WIDTH is used in ensuring DEV_NAME_LEN is big
 * enough to hold all possible device names.
 */
#define DEV_NAME_LEN		32
#define MAX_INT_FORMAT_WIDTH	((5 * sizeof (int)) / 2 + 1)

/*
 * block device image metadata (in-memory version)
 */
struct rbd_image_header {
	/* These four fields never change for a given rbd image */
	char *object_prefix;
	u64 features;
	__u8 obj_order;
	__u8 crypt_type;
	__u8 comp_type;

	/* The remaining fields need to be updated occasionally */
	u64 image_size;
	struct ceph_snap_context *snapc;
	char *snap_names;
	u64 *snap_sizes;

	u64 stripe_unit;
	u64 stripe_count;

	u64 obj_version;
};

/*
 * An rbd image specification.
 *
 * The tuple (pool_id, image_id, snap_id) is sufficient to uniquely
 * identify an image.  Each rbd_dev structure includes a pointer to
 * an rbd_spec structure that encapsulates this identity.
 *
 * Each of the id's in an rbd_spec has an associated name.  For a
 * user-mapped image, the names are supplied and the id's associated
 * with them are looked up.  For a layered image, a parent image is
 * defined by the tuple, and the names are looked up.
 *
 * An rbd_dev structure contains a parent_spec pointer which is
 * non-null if the image it represents is a child in a layered
 * image.  This pointer will refer to the rbd_spec structure used
 * by the parent rbd_dev for its own identity (i.e., the structure
 * is shared between the parent and child).
 *
 * Since these structures are populated once, during the discovery
 * phase of image construction, they are effectively immutable so
 * we make no effort to synchronize access to them.
 *
 * Note that code herein does not assume the image name is known (it
 * could be a null pointer).
 */
struct rbd_spec {
	u64		pool_id;
	const char	*pool_name;

	const char	*image_id;
	const char	*image_name;

	u64		snap_id;
	const char	*snap_name;

	struct kref	kref;
};

/*
 * an instance of the client.  multiple devices may share an rbd client.
 */
struct rbd_client {
	struct ceph_client	*client;
	struct kref		kref;
	struct list_head	node;
};

struct rbd_img_request;
typedef void (*rbd_img_callback_t)(struct rbd_img_request *);

#define	BAD_WHICH	U32_MAX		/* Good which or bad which, which? */

struct rbd_obj_request;
typedef void (*rbd_obj_callback_t)(struct rbd_obj_request *);

enum obj_request_type {
	OBJ_REQUEST_NODATA, OBJ_REQUEST_BIO, OBJ_REQUEST_PAGES
};

enum obj_req_flags {
	OBJ_REQ_DONE,		/* completion flag: not done = 0, done = 1 */
	OBJ_REQ_IMG_DATA,	/* object usage: standalone = 0, image = 1 */
	OBJ_REQ_KNOWN,		/* EXISTS flag valid: no = 0, yes = 1 */
	OBJ_REQ_EXISTS,		/* target exists: no = 0, yes = 1 */
};

struct rbd_obj_request {
	const char		*object_name;
	u64			offset;		/* object start byte */
	u64			length;		/* bytes from offset */
	unsigned long		flags;

	/*
	 * An object request associated with an image will have its
	 * img_data flag set; a standalone object request will not.
	 *
	 * A standalone object request will have which == BAD_WHICH
	 * and a null obj_request pointer.
	 *
	 * An object request initiated in support of a layered image
	 * object (to check for its existence before a write) will
	 * have which == BAD_WHICH and a non-null obj_request pointer.
	 *
	 * Finally, an object request for rbd image data will have
	 * which != BAD_WHICH, and will have a non-null img_request
	 * pointer.  The value of which will be in the range
	 * 0..(img_request->obj_request_count-1).
	 */
	union {
		struct rbd_obj_request	*obj_request;	/* STAT op */
		struct {
			struct rbd_img_request	*img_request;
			u64			img_offset;
			/* links for img_request->obj_requests list */
			struct list_head	links;
		};
	};
	u32			which;		/* posn image request list */

	enum obj_request_type	type;
	union {
		struct bio	*bio_list;
		struct {
			struct page	**pages;
			u32		page_count;
		};
	};
	struct page		**copyup_pages;

	struct ceph_osd_request	*osd_req;

	u64			xferred;	/* bytes transferred */
	u64			version;
	int			result;

	rbd_obj_callback_t	callback;
	struct completion	completion;

	struct kref		kref;
};

enum img_req_flags {
	IMG_REQ_WRITE,		/* I/O direction: read = 0, write = 1 */
	IMG_REQ_CHILD,		/* initiator: block = 0, child image = 1 */
	IMG_REQ_LAYERED,	/* ENOENT handling: normal = 0, layered = 1 */
};

struct rbd_img_request {
	struct rbd_device	*rbd_dev;
	u64			offset;	/* starting image byte offset */
	u64			length;	/* byte count from offset */
	unsigned long		flags;
	union {
		u64			snap_id;	/* for reads */
		struct ceph_snap_context *snapc;	/* for writes */
	};
	union {
		struct request		*rq;		/* block request */
		struct rbd_obj_request	*obj_request;	/* obj req initiator */
	};
	struct page		**copyup_pages;
	spinlock_t		completion_lock;/* protects next_completion */
	u32			next_completion;
	rbd_img_callback_t	callback;
	u64			xferred;/* aggregate bytes transferred */
	int			result;	/* first nonzero obj_request result */

	u32			obj_request_count;
	struct list_head	obj_requests;	/* rbd_obj_request structs */

	struct kref		kref;
};

#define for_each_obj_request(ireq, oreq) \
	list_for_each_entry(oreq, &(ireq)->obj_requests, links)
#define for_each_obj_request_from(ireq, oreq) \
	list_for_each_entry_from(oreq, &(ireq)->obj_requests, links)
#define for_each_obj_request_safe(ireq, oreq, n) \
	list_for_each_entry_safe_reverse(oreq, n, &(ireq)->obj_requests, links)

struct rbd_snap {
	const char		*name;
	u64			size;
	struct list_head	node;
	u64			id;
	u64			features;
};

struct rbd_mapping {
	u64                     size;
	u64                     features;
	bool			read_only;
};

/*
 * a single device
 */
struct rbd_device {
	int			dev_id;		/* blkdev unique id */

	int			major;		/* blkdev assigned major */
	struct gendisk		*disk;		/* blkdev's gendisk and rq */

	u32			image_format;	/* Either 1 or 2 */
	struct rbd_client	*rbd_client;

	char			name[DEV_NAME_LEN]; /* blkdev name, e.g. rbd3 */

	spinlock_t		lock;		/* queue, flags, open_count */

	struct rbd_image_header	header;
	unsigned long		flags;		/* possibly lock protected */
	struct rbd_spec		*spec;

	char			*header_name;

	struct ceph_file_layout	layout;

	struct ceph_osd_event   *watch_event;
	struct rbd_obj_request	*watch_request;

	struct rbd_spec		*parent_spec;
	u64			parent_overlap;
	struct rbd_device	*parent;

	/* protects updating the header */
	struct rw_semaphore     header_rwsem;

	struct rbd_mapping	mapping;

	struct list_head	node;

	/* list of snapshots */
	struct list_head	snaps;

	/* sysfs related */
	struct device		dev;
	unsigned long		open_count;	/* protected by lock */
};

/*
 * Flag bits for rbd_dev->flags.  If atomicity is required,
 * rbd_dev->lock is used to protect access.
 *
 * Currently, only the "removing" flag (which is coupled with the
 * "open_count" field) requires atomic access.
 */
enum rbd_dev_flags {
	RBD_DEV_FLAG_EXISTS,	/* mapped snapshot has not been deleted */
	RBD_DEV_FLAG_REMOVING,	/* this mapping is being removed */
};

static DEFINE_MUTEX(ctl_mutex);	  /* Serialize open/close/setup/teardown */

static LIST_HEAD(rbd_dev_list);    /* devices */
static DEFINE_SPINLOCK(rbd_dev_list_lock);

static LIST_HEAD(rbd_client_list);		/* clients */
static DEFINE_SPINLOCK(rbd_client_list_lock);

static int rbd_img_request_submit(struct rbd_img_request *img_request);

static int rbd_dev_snaps_update(struct rbd_device *rbd_dev);

static void rbd_dev_release(struct device *dev);
static void rbd_snap_destroy(struct rbd_snap *snap);

static ssize_t rbd_add(struct bus_type *bus, const char *buf,
		       size_t count);
static ssize_t rbd_remove(struct bus_type *bus, const char *buf,
			  size_t count);
static int rbd_dev_probe(struct rbd_device *rbd_dev);

static struct bus_attribute rbd_bus_attrs[] = {
	__ATTR(add, S_IWUSR, NULL, rbd_add),
	__ATTR(remove, S_IWUSR, NULL, rbd_remove),
	__ATTR_NULL
};

static struct bus_type rbd_bus_type = {
	.name		= "rbd",
	.bus_attrs	= rbd_bus_attrs,
};

static void rbd_root_dev_release(struct device *dev)
{
}

static struct device rbd_root_dev = {
	.init_name =    "rbd",
	.release =      rbd_root_dev_release,
};

static __printf(2, 3)
void rbd_warn(struct rbd_device *rbd_dev, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;

	if (!rbd_dev)
		printk(KERN_WARNING "%s: %pV\n", RBD_DRV_NAME, &vaf);
	else if (rbd_dev->disk)
		printk(KERN_WARNING "%s: %s: %pV\n",
			RBD_DRV_NAME, rbd_dev->disk->disk_name, &vaf);
	else if (rbd_dev->spec && rbd_dev->spec->image_name)
		printk(KERN_WARNING "%s: image %s: %pV\n",
			RBD_DRV_NAME, rbd_dev->spec->image_name, &vaf);
	else if (rbd_dev->spec && rbd_dev->spec->image_id)
		printk(KERN_WARNING "%s: id %s: %pV\n",
			RBD_DRV_NAME, rbd_dev->spec->image_id, &vaf);
	else	/* punt */
		printk(KERN_WARNING "%s: rbd_dev %p: %pV\n",
			RBD_DRV_NAME, rbd_dev, &vaf);
	va_end(args);
}

#ifdef RBD_DEBUG
#define rbd_assert(expr)						\
		if (unlikely(!(expr))) {				\
			printk(KERN_ERR "\nAssertion failure in %s() "	\
						"at line %d:\n\n"	\
					"\trbd_assert(%s);\n\n",	\
					__func__, __LINE__, #expr);	\
			BUG();						\
		}
#else /* !RBD_DEBUG */
#  define rbd_assert(expr)	((void) 0)
#endif /* !RBD_DEBUG */

static void rbd_img_parent_read(struct rbd_obj_request *obj_request);
static int rbd_img_obj_request_submit(struct rbd_obj_request *obj_request);

static int rbd_dev_refresh(struct rbd_device *rbd_dev, u64 *hver);
static int rbd_dev_v2_refresh(struct rbd_device *rbd_dev, u64 *hver);

static int rbd_open(struct block_device *bdev, fmode_t mode)
{
	struct rbd_device *rbd_dev = bdev->bd_disk->private_data;
	bool removing = false;

	if ((mode & FMODE_WRITE) && rbd_dev->mapping.read_only)
		return -EROFS;

	spin_lock_irq(&rbd_dev->lock);
	if (test_bit(RBD_DEV_FLAG_REMOVING, &rbd_dev->flags))
		removing = true;
	else
		rbd_dev->open_count++;
	spin_unlock_irq(&rbd_dev->lock);
	if (removing)
		return -ENOENT;

	mutex_lock_nested(&ctl_mutex, SINGLE_DEPTH_NESTING);
	(void) get_device(&rbd_dev->dev);
	set_device_ro(bdev, rbd_dev->mapping.read_only);
	mutex_unlock(&ctl_mutex);

	return 0;
}

static int rbd_release(struct gendisk *disk, fmode_t mode)
{
	struct rbd_device *rbd_dev = disk->private_data;
	unsigned long open_count_before;

	spin_lock_irq(&rbd_dev->lock);
	open_count_before = rbd_dev->open_count--;
	spin_unlock_irq(&rbd_dev->lock);
	rbd_assert(open_count_before > 0);

	mutex_lock_nested(&ctl_mutex, SINGLE_DEPTH_NESTING);
	put_device(&rbd_dev->dev);
	mutex_unlock(&ctl_mutex);

	return 0;
}

static const struct block_device_operations rbd_bd_ops = {
	.owner			= THIS_MODULE,
	.open			= rbd_open,
	.release		= rbd_release,
};

/*
 * Initialize an rbd client instance.
 * We own *ceph_opts.
 */
static struct rbd_client *rbd_client_create(struct ceph_options *ceph_opts)
{
	struct rbd_client *rbdc;
	int ret = -ENOMEM;

	dout("%s:\n", __func__);
	rbdc = kmalloc(sizeof(struct rbd_client), GFP_KERNEL);
	if (!rbdc)
		goto out_opt;

	kref_init(&rbdc->kref);
	INIT_LIST_HEAD(&rbdc->node);

	mutex_lock_nested(&ctl_mutex, SINGLE_DEPTH_NESTING);

	rbdc->client = ceph_create_client(ceph_opts, rbdc, 0, 0);
	if (IS_ERR(rbdc->client))
		goto out_mutex;
	ceph_opts = NULL; /* Now rbdc->client is responsible for ceph_opts */

	ret = ceph_open_session(rbdc->client);
	if (ret < 0)
		goto out_err;

	spin_lock(&rbd_client_list_lock);
	list_add_tail(&rbdc->node, &rbd_client_list);
	spin_unlock(&rbd_client_list_lock);

	mutex_unlock(&ctl_mutex);
	dout("%s: rbdc %p\n", __func__, rbdc);

	return rbdc;

out_err:
	ceph_destroy_client(rbdc->client);
out_mutex:
	mutex_unlock(&ctl_mutex);
	kfree(rbdc);
out_opt:
	if (ceph_opts)
		ceph_destroy_options(ceph_opts);
	dout("%s: error %d\n", __func__, ret);

	return ERR_PTR(ret);
}

static struct rbd_client *__rbd_get_client(struct rbd_client *rbdc)
{
	kref_get(&rbdc->kref);

	return rbdc;
}

/*
 * Find a ceph client with specific addr and configuration.  If
 * found, bump its reference count.
 */
static struct rbd_client *rbd_client_find(struct ceph_options *ceph_opts)
{
	struct rbd_client *client_node;
	bool found = false;

	if (ceph_opts->flags & CEPH_OPT_NOSHARE)
		return NULL;

	spin_lock(&rbd_client_list_lock);
	list_for_each_entry(client_node, &rbd_client_list, node) {
		if (!ceph_compare_options(ceph_opts, client_node->client)) {
			__rbd_get_client(client_node);

			found = true;
			break;
		}
	}
	spin_unlock(&rbd_client_list_lock);

	return found ? client_node : NULL;
}

/*
 * mount options
 */
enum {
	Opt_last_int,
	/* int args above */
	Opt_last_string,
	/* string args above */
	Opt_read_only,
	Opt_read_write,
	/* Boolean args above */
	Opt_last_bool,
};

static match_table_t rbd_opts_tokens = {
	/* int args above */
	/* string args above */
	{Opt_read_only, "read_only"},
	{Opt_read_only, "ro"},		/* Alternate spelling */
	{Opt_read_write, "read_write"},
	{Opt_read_write, "rw"},		/* Alternate spelling */
	/* Boolean args above */
	{-1, NULL}
};

struct rbd_options {
	bool	read_only;
};

#define RBD_READ_ONLY_DEFAULT	false

static int parse_rbd_opts_token(char *c, void *private)
{
	struct rbd_options *rbd_opts = private;
	substring_t argstr[MAX_OPT_ARGS];
	int token, intval, ret;

	token = match_token(c, rbd_opts_tokens, argstr);
	if (token < 0)
		return -EINVAL;

	if (token < Opt_last_int) {
		ret = match_int(&argstr[0], &intval);
		if (ret < 0) {
			pr_err("bad mount option arg (not int) "
			       "at '%s'\n", c);
			return ret;
		}
		dout("got int token %d val %d\n", token, intval);
	} else if (token > Opt_last_int && token < Opt_last_string) {
		dout("got string token %d val %s\n", token,
		     argstr[0].from);
	} else if (token > Opt_last_string && token < Opt_last_bool) {
		dout("got Boolean token %d\n", token);
	} else {
		dout("got token %d\n", token);
	}

	switch (token) {
	case Opt_read_only:
		rbd_opts->read_only = true;
		break;
	case Opt_read_write:
		rbd_opts->read_only = false;
		break;
	default:
		rbd_assert(false);
		break;
	}
	return 0;
}

/*
 * Get a ceph client with specific addr and configuration, if one does
 * not exist create it.
 */
static struct rbd_client *rbd_get_client(struct ceph_options *ceph_opts)
{
	struct rbd_client *rbdc;

	rbdc = rbd_client_find(ceph_opts);
	if (rbdc)	/* using an existing client */
		ceph_destroy_options(ceph_opts);
	else
		rbdc = rbd_client_create(ceph_opts);

	return rbdc;
}

/*
 * Destroy ceph client
 *
 * Caller must hold rbd_client_list_lock.
 */
static void rbd_client_release(struct kref *kref)
{
	struct rbd_client *rbdc = container_of(kref, struct rbd_client, kref);

	dout("%s: rbdc %p\n", __func__, rbdc);
	spin_lock(&rbd_client_list_lock);
	list_del(&rbdc->node);
	spin_unlock(&rbd_client_list_lock);

	ceph_destroy_client(rbdc->client);
	kfree(rbdc);
}

/*
 * Drop reference to ceph client node. If it's not referenced anymore, release
 * it.
 */
static void rbd_put_client(struct rbd_client *rbdc)
{
	if (rbdc)
		kref_put(&rbdc->kref, rbd_client_release);
}

static bool rbd_image_format_valid(u32 image_format)
{
	return image_format == 1 || image_format == 2;
}

static bool rbd_dev_ondisk_valid(struct rbd_image_header_ondisk *ondisk)
{
	size_t size;
	u32 snap_count;

	/* The header has to start with the magic rbd header text */
	if (memcmp(&ondisk->text, RBD_HEADER_TEXT, sizeof (RBD_HEADER_TEXT)))
		return false;

	/* The bio layer requires at least sector-sized I/O */

	if (ondisk->options.order < SECTOR_SHIFT)
		return false;

	/* If we use u64 in a few spots we may be able to loosen this */

	if (ondisk->options.order > 8 * sizeof (int) - 1)
		return false;

	/*
	 * The size of a snapshot header has to fit in a size_t, and
	 * that limits the number of snapshots.
	 */
	snap_count = le32_to_cpu(ondisk->snap_count);
	size = SIZE_MAX - sizeof (struct ceph_snap_context);
	if (snap_count > size / sizeof (__le64))
		return false;

	/*
	 * Not only that, but the size of the entire the snapshot
	 * header must also be representable in a size_t.
	 */
	size -= snap_count * sizeof (__le64);
	if ((u64) size < le64_to_cpu(ondisk->snap_names_len))
		return false;

	return true;
}

/*
 * Create a new header structure, translate header format from the on-disk
 * header.
 */
static int rbd_header_from_disk(struct rbd_image_header *header,
				 struct rbd_image_header_ondisk *ondisk)
{
	u32 snap_count;
	size_t len;
	size_t size;
	u32 i;

	memset(header, 0, sizeof (*header));

	snap_count = le32_to_cpu(ondisk->snap_count);

	len = strnlen(ondisk->object_prefix, sizeof (ondisk->object_prefix));
	header->object_prefix = kmalloc(len + 1, GFP_KERNEL);
	if (!header->object_prefix)
		return -ENOMEM;
	memcpy(header->object_prefix, ondisk->object_prefix, len);
	header->object_prefix[len] = '\0';

	if (snap_count) {
		u64 snap_names_len = le64_to_cpu(ondisk->snap_names_len);

		/* Save a copy of the snapshot names */

		if (snap_names_len > (u64) SIZE_MAX)
			return -EIO;
		header->snap_names = kmalloc(snap_names_len, GFP_KERNEL);
		if (!header->snap_names)
			goto out_err;
		/*
		 * Note that rbd_dev_v1_header_read() guarantees
		 * the ondisk buffer we're working with has
		 * snap_names_len bytes beyond the end of the
		 * snapshot id array, this memcpy() is safe.
		 */
		memcpy(header->snap_names, &ondisk->snaps[snap_count],
			snap_names_len);

		/* Record each snapshot's size */

		size = snap_count * sizeof (*header->snap_sizes);
		header->snap_sizes = kmalloc(size, GFP_KERNEL);
		if (!header->snap_sizes)
			goto out_err;
		for (i = 0; i < snap_count; i++)
			header->snap_sizes[i] =
				le64_to_cpu(ondisk->snaps[i].image_size);
	} else {
		header->snap_names = NULL;
		header->snap_sizes = NULL;
	}

	header->features = 0;	/* No features support in v1 images */
	header->obj_order = ondisk->options.order;
	header->crypt_type = ondisk->options.crypt_type;
	header->comp_type = ondisk->options.comp_type;

	/* Allocate and fill in the snapshot context */

	header->image_size = le64_to_cpu(ondisk->image_size);
	size = sizeof (struct ceph_snap_context);
	size += snap_count * sizeof (header->snapc->snaps[0]);
	header->snapc = kzalloc(size, GFP_KERNEL);
	if (!header->snapc)
		goto out_err;

	atomic_set(&header->snapc->nref, 1);
	header->snapc->seq = le64_to_cpu(ondisk->snap_seq);
	header->snapc->num_snaps = snap_count;
	for (i = 0; i < snap_count; i++)
		header->snapc->snaps[i] =
			le64_to_cpu(ondisk->snaps[i].id);

	return 0;

out_err:
	kfree(header->snap_sizes);
	header->snap_sizes = NULL;
	kfree(header->snap_names);
	header->snap_names = NULL;
	kfree(header->object_prefix);
	header->object_prefix = NULL;

	return -ENOMEM;
}

static const char *rbd_snap_name(struct rbd_device *rbd_dev, u64 snap_id)
{
	struct rbd_snap *snap;

	if (snap_id == CEPH_NOSNAP)
		return RBD_SNAP_HEAD_NAME;

	list_for_each_entry(snap, &rbd_dev->snaps, node)
		if (snap_id == snap->id)
			return snap->name;

	return NULL;
}

static struct rbd_snap *snap_by_name(struct rbd_device *rbd_dev,
					const char *snap_name)
{
	struct rbd_snap *snap;

	list_for_each_entry(snap, &rbd_dev->snaps, node)
		if (!strcmp(snap_name, snap->name))
			return snap;

	return NULL;
}

static int rbd_dev_set_mapping(struct rbd_device *rbd_dev)
{
	if (!memcmp(rbd_dev->spec->snap_name, RBD_SNAP_HEAD_NAME,
		    sizeof (RBD_SNAP_HEAD_NAME))) {
		rbd_dev->mapping.size = rbd_dev->header.image_size;
		rbd_dev->mapping.features = rbd_dev->header.features;
	} else {
		struct rbd_snap *snap;

		snap = snap_by_name(rbd_dev, rbd_dev->spec->snap_name);
		if (!snap)
			return -ENOENT;
		rbd_dev->mapping.size = snap->size;
		rbd_dev->mapping.features = snap->features;
		rbd_dev->mapping.read_only = true;
	}
	set_bit(RBD_DEV_FLAG_EXISTS, &rbd_dev->flags);

	return 0;
}

static void rbd_header_free(struct rbd_image_header *header)
{
	kfree(header->object_prefix);
	header->object_prefix = NULL;
	kfree(header->snap_sizes);
	header->snap_sizes = NULL;
	kfree(header->snap_names);
	header->snap_names = NULL;
	ceph_put_snap_context(header->snapc);
	header->snapc = NULL;
}

static const char *rbd_segment_name(struct rbd_device *rbd_dev, u64 offset)
{
	char *name;
	u64 segment;
	int ret;

	name = kmalloc(MAX_OBJ_NAME_SIZE + 1, GFP_NOIO);
	if (!name)
		return NULL;
	segment = offset >> rbd_dev->header.obj_order;
	ret = snprintf(name, MAX_OBJ_NAME_SIZE + 1, "%s.%012llx",
			rbd_dev->header.object_prefix, segment);
	if (ret < 0 || ret > MAX_OBJ_NAME_SIZE) {
		pr_err("error formatting segment name for #%llu (%d)\n",
			segment, ret);
		kfree(name);
		name = NULL;
	}

	return name;
}

static u64 rbd_segment_offset(struct rbd_device *rbd_dev, u64 offset)
{
	u64 segment_size = (u64) 1 << rbd_dev->header.obj_order;

	return offset & (segment_size - 1);
}

static u64 rbd_segment_length(struct rbd_device *rbd_dev,
				u64 offset, u64 length)
{
	u64 segment_size = (u64) 1 << rbd_dev->header.obj_order;

	offset &= segment_size - 1;

	rbd_assert(length <= U64_MAX - offset);
	if (offset + length > segment_size)
		length = segment_size - offset;

	return length;
}

/*
 * returns the size of an object in the image
 */
static u64 rbd_obj_bytes(struct rbd_image_header *header)
{
	return 1 << header->obj_order;
}

/*
 * bio helpers
 */

static void bio_chain_put(struct bio *chain)
{
	struct bio *tmp;

	while (chain) {
		tmp = chain;
		chain = chain->bi_next;
		bio_put(tmp);
	}
}

/*
 * zeros a bio chain, starting at specific offset
 */
static void zero_bio_chain(struct bio *chain, int start_ofs)
{
	struct bio_vec *bv;
	unsigned long flags;
	void *buf;
	int i;
	int pos = 0;

	while (chain) {
		bio_for_each_segment(bv, chain, i) {
			if (pos + bv->bv_len > start_ofs) {
				int remainder = max(start_ofs - pos, 0);
				buf = bvec_kmap_irq(bv, &flags);
				memset(buf + remainder, 0,
				       bv->bv_len - remainder);
				bvec_kunmap_irq(buf, &flags);
			}
			pos += bv->bv_len;
		}

		chain = chain->bi_next;
	}
}

/*
 * similar to zero_bio_chain(), zeros data defined by a page array,
 * starting at the given byte offset from the start of the array and
 * continuing up to the given end offset.  The pages array is
 * assumed to be big enough to hold all bytes up to the end.
 */
static void zero_pages(struct page **pages, u64 offset, u64 end)
{
	struct page **page = &pages[offset >> PAGE_SHIFT];

	rbd_assert(end > offset);
	rbd_assert(end - offset <= (u64)SIZE_MAX);
	while (offset < end) {
		size_t page_offset;
		size_t length;
		unsigned long flags;
		void *kaddr;

		page_offset = (size_t)(offset & ~PAGE_MASK);
		length = min(PAGE_SIZE - page_offset, (size_t)(end - offset));
		local_irq_save(flags);
		kaddr = kmap_atomic(*page);
		memset(kaddr + page_offset, 0, length);
		kunmap_atomic(kaddr);
		local_irq_restore(flags);

		offset += length;
		page++;
	}
}

/*
 * Clone a portion of a bio, starting at the given byte offset
 * and continuing for the number of bytes indicated.
 */
static struct bio *bio_clone_range(struct bio *bio_src,
					unsigned int offset,
					unsigned int len,
					gfp_t gfpmask)
{
	struct bio_vec *bv;
	unsigned int resid;
	unsigned short idx;
	unsigned int voff;
	unsigned short end_idx;
	unsigned short vcnt;
	struct bio *bio;

	/* Handle the easy case for the caller */

	if (!offset && len == bio_src->bi_size)
		return bio_clone(bio_src, gfpmask);

	if (WARN_ON_ONCE(!len))
		return NULL;
	if (WARN_ON_ONCE(len > bio_src->bi_size))
		return NULL;
	if (WARN_ON_ONCE(offset > bio_src->bi_size - len))
		return NULL;

	/* Find first affected segment... */

	resid = offset;
	__bio_for_each_segment(bv, bio_src, idx, 0) {
		if (resid < bv->bv_len)
			break;
		resid -= bv->bv_len;
	}
	voff = resid;

	/* ...and the last affected segment */

	resid += len;
	__bio_for_each_segment(bv, bio_src, end_idx, idx) {
		if (resid <= bv->bv_len)
			break;
		resid -= bv->bv_len;
	}
	vcnt = end_idx - idx + 1;

	/* Build the clone */

	bio = bio_alloc(gfpmask, (unsigned int) vcnt);
	if (!bio)
		return NULL;	/* ENOMEM */

	bio->bi_bdev = bio_src->bi_bdev;
	bio->bi_sector = bio_src->bi_sector + (offset >> SECTOR_SHIFT);
	bio->bi_rw = bio_src->bi_rw;
	bio->bi_flags |= 1 << BIO_CLONED;

	/*
	 * Copy over our part of the bio_vec, then update the first
	 * and last (or only) entries.
	 */
	memcpy(&bio->bi_io_vec[0], &bio_src->bi_io_vec[idx],
			vcnt * sizeof (struct bio_vec));
	bio->bi_io_vec[0].bv_offset += voff;
	if (vcnt > 1) {
		bio->bi_io_vec[0].bv_len -= voff;
		bio->bi_io_vec[vcnt - 1].bv_len = resid;
	} else {
		bio->bi_io_vec[0].bv_len = len;
	}

	bio->bi_vcnt = vcnt;
	bio->bi_size = len;
	bio->bi_idx = 0;

	return bio;
}

/*
 * Clone a portion of a bio chain, starting at the given byte offset
 * into the first bio in the source chain and continuing for the
 * number of bytes indicated.  The result is another bio chain of
 * exactly the given length, or a null pointer on error.
 *
 * The bio_src and offset parameters are both in-out.  On entry they
 * refer to the first source bio and the offset into that bio where
 * the start of data to be cloned is located.
 *
 * On return, bio_src is updated to refer to the bio in the source
 * chain that contains first un-cloned byte, and *offset will
 * contain the offset of that byte within that bio.
 */
static struct bio *bio_chain_clone_range(struct bio **bio_src,
					unsigned int *offset,
					unsigned int len,
					gfp_t gfpmask)
{
	struct bio *bi = *bio_src;
	unsigned int off = *offset;
	struct bio *chain = NULL;
	struct bio **end;

	/* Build up a chain of clone bios up to the limit */

	if (!bi || off >= bi->bi_size || !len)
		return NULL;		/* Nothing to clone */

	end = &chain;
	while (len) {
		unsigned int bi_size;
		struct bio *bio;

		if (!bi) {
			rbd_warn(NULL, "bio_chain exhausted with %u left", len);
			goto out_err;	/* EINVAL; ran out of bio's */
		}
		bi_size = min_t(unsigned int, bi->bi_size - off, len);
		bio = bio_clone_range(bi, off, bi_size, gfpmask);
		if (!bio)
			goto out_err;	/* ENOMEM */

		*end = bio;
		end = &bio->bi_next;

		off += bi_size;
		if (off == bi->bi_size) {
			bi = bi->bi_next;
			off = 0;
		}
		len -= bi_size;
	}
	*bio_src = bi;
	*offset = off;

	return chain;
out_err:
	bio_chain_put(chain);

	return NULL;
}

/*
 * The default/initial value for all object request flags is 0.  For
 * each flag, once its value is set to 1 it is never reset to 0
 * again.
 */
static void obj_request_img_data_set(struct rbd_obj_request *obj_request)
{
	if (test_and_set_bit(OBJ_REQ_IMG_DATA, &obj_request->flags)) {
		struct rbd_device *rbd_dev;

		rbd_dev = obj_request->img_request->rbd_dev;
		rbd_warn(rbd_dev, "obj_request %p already marked img_data\n",
			obj_request);
	}
}

static bool obj_request_img_data_test(struct rbd_obj_request *obj_request)
{
	smp_mb();
	return test_bit(OBJ_REQ_IMG_DATA, &obj_request->flags) != 0;
}

static void obj_request_done_set(struct rbd_obj_request *obj_request)
{
	if (test_and_set_bit(OBJ_REQ_DONE, &obj_request->flags)) {
		struct rbd_device *rbd_dev = NULL;

		if (obj_request_img_data_test(obj_request))
			rbd_dev = obj_request->img_request->rbd_dev;
		rbd_warn(rbd_dev, "obj_request %p already marked done\n",
			obj_request);
	}
}

static bool obj_request_done_test(struct rbd_obj_request *obj_request)
{
	smp_mb();
	return test_bit(OBJ_REQ_DONE, &obj_request->flags) != 0;
}

/*
 * This sets the KNOWN flag after (possibly) setting the EXISTS
 * flag.  The latter is set based on the "exists" value provided.
 *
 * Note that for our purposes once an object exists it never goes
 * away again.  It's possible that the response from two existence
 * checks are separated by the creation of the target object, and
 * the first ("doesn't exist") response arrives *after* the second
 * ("does exist").  In that case we ignore the second one.
 */
static void obj_request_existence_set(struct rbd_obj_request *obj_request,
				bool exists)
{
	if (exists)
		set_bit(OBJ_REQ_EXISTS, &obj_request->flags);
	set_bit(OBJ_REQ_KNOWN, &obj_request->flags);
	smp_mb();
}

static bool obj_request_known_test(struct rbd_obj_request *obj_request)
{
	smp_mb();
	return test_bit(OBJ_REQ_KNOWN, &obj_request->flags) != 0;
}

static bool obj_request_exists_test(struct rbd_obj_request *obj_request)
{
	smp_mb();
	return test_bit(OBJ_REQ_EXISTS, &obj_request->flags) != 0;
}

static void rbd_obj_request_get(struct rbd_obj_request *obj_request)
{
	dout("%s: obj %p (was %d)\n", __func__, obj_request,
		atomic_read(&obj_request->kref.refcount));
	kref_get(&obj_request->kref);
}

static void rbd_obj_request_destroy(struct kref *kref);
static void rbd_obj_request_put(struct rbd_obj_request *obj_request)
{
	rbd_assert(obj_request != NULL);
	dout("%s: obj %p (was %d)\n", __func__, obj_request,
		atomic_read(&obj_request->kref.refcount));
	kref_put(&obj_request->kref, rbd_obj_request_destroy);
}

static void rbd_img_request_get(struct rbd_img_request *img_request)
{
	dout("%s: img %p (was %d)\n", __func__, img_request,
		atomic_read(&img_request->kref.refcount));
	kref_get(&img_request->kref);
}

static void rbd_img_request_destroy(struct kref *kref);
static void rbd_img_request_put(struct rbd_img_request *img_request)
{
	rbd_assert(img_request != NULL);
	dout("%s: img %p (was %d)\n", __func__, img_request,
		atomic_read(&img_request->kref.refcount));
	kref_put(&img_request->kref, rbd_img_request_destroy);
}

static inline void rbd_img_obj_request_add(struct rbd_img_request *img_request,
					struct rbd_obj_request *obj_request)
{
	rbd_assert(obj_request->img_request == NULL);

	/* Image request now owns object's original reference */
	obj_request->img_request = img_request;
	obj_request->which = img_request->obj_request_count;
	rbd_assert(!obj_request_img_data_test(obj_request));
	obj_request_img_data_set(obj_request);
	rbd_assert(obj_request->which != BAD_WHICH);
	img_request->obj_request_count++;
	list_add_tail(&obj_request->links, &img_request->obj_requests);
	dout("%s: img %p obj %p w=%u\n", __func__, img_request, obj_request,
		obj_request->which);
}

static inline void rbd_img_obj_request_del(struct rbd_img_request *img_request,
					struct rbd_obj_request *obj_request)
{
	rbd_assert(obj_request->which != BAD_WHICH);

	dout("%s: img %p obj %p w=%u\n", __func__, img_request, obj_request,
		obj_request->which);
	list_del(&obj_request->links);
	rbd_assert(img_request->obj_request_count > 0);
	img_request->obj_request_count--;
	rbd_assert(obj_request->which == img_request->obj_request_count);
	obj_request->which = BAD_WHICH;
	rbd_assert(obj_request_img_data_test(obj_request));
	rbd_assert(obj_request->img_request == img_request);
	obj_request->img_request = NULL;
	obj_request->callback = NULL;
	rbd_obj_request_put(obj_request);
}

static bool obj_request_type_valid(enum obj_request_type type)
{
	switch (type) {
	case OBJ_REQUEST_NODATA:
	case OBJ_REQUEST_BIO:
	case OBJ_REQUEST_PAGES:
		return true;
	default:
		return false;
	}
}

static int rbd_obj_request_submit(struct ceph_osd_client *osdc,
				struct rbd_obj_request *obj_request)
{
	dout("%s: osdc %p obj %p\n", __func__, osdc, obj_request);

	return ceph_osdc_start_request(osdc, obj_request->osd_req, false);
}

static void rbd_img_request_complete(struct rbd_img_request *img_request)
{

	dout("%s: img %p\n", __func__, img_request);

	/*
	 * If no error occurred, compute the aggregate transfer
	 * count for the image request.  We could instead use
	 * atomic64_cmpxchg() to update it as each object request
	 * completes; not clear which way is better off hand.
	 */
	if (!img_request->result) {
		struct rbd_obj_request *obj_request;
		u64 xferred = 0;

		for_each_obj_request(img_request, obj_request)
			xferred += obj_request->xferred;
		img_request->xferred = xferred;
	}

	if (img_request->callback)
		img_request->callback(img_request);
	else
		rbd_img_request_put(img_request);
}

/* Caller is responsible for rbd_obj_request_destroy(obj_request) */

static int rbd_obj_request_wait(struct rbd_obj_request *obj_request)
{
	dout("%s: obj %p\n", __func__, obj_request);

	return wait_for_completion_interruptible(&obj_request->completion);
}

/*
 * The default/initial value for all image request flags is 0.  Each
 * is conditionally set to 1 at image request initialization time
 * and currently never change thereafter.
 */
static void img_request_write_set(struct rbd_img_request *img_request)
{
	set_bit(IMG_REQ_WRITE, &img_request->flags);
	smp_mb();
}

static bool img_request_write_test(struct rbd_img_request *img_request)
{
	smp_mb();
	return test_bit(IMG_REQ_WRITE, &img_request->flags) != 0;
}

static void img_request_child_set(struct rbd_img_request *img_request)
{
	set_bit(IMG_REQ_CHILD, &img_request->flags);
	smp_mb();
}

static bool img_request_child_test(struct rbd_img_request *img_request)
{
	smp_mb();
	return test_bit(IMG_REQ_CHILD, &img_request->flags) != 0;
}

static void img_request_layered_set(struct rbd_img_request *img_request)
{
	set_bit(IMG_REQ_LAYERED, &img_request->flags);
	smp_mb();
}

static bool img_request_layered_test(struct rbd_img_request *img_request)
{
	smp_mb();
	return test_bit(IMG_REQ_LAYERED, &img_request->flags) != 0;
}

static void
rbd_img_obj_request_read_callback(struct rbd_obj_request *obj_request)
{
	u64 xferred = obj_request->xferred;
	u64 length = obj_request->length;

	dout("%s: obj %p img %p result %d %llu/%llu\n", __func__,
		obj_request, obj_request->img_request, obj_request->result,
		xferred, length);
	/*
	 * ENOENT means a hole in the image.  We zero-fill the
	 * entire length of the request.  A short read also implies
	 * zero-fill to the end of the request.  Either way we
	 * update the xferred count to indicate the whole request
	 * was satisfied.
	 */
	rbd_assert(obj_request->type != OBJ_REQUEST_NODATA);
	if (obj_request->result == -ENOENT) {
		if (obj_request->type == OBJ_REQUEST_BIO)
			zero_bio_chain(obj_request->bio_list, 0);
		else
			zero_pages(obj_request->pages, 0, length);
		obj_request->result = 0;
		obj_request->xferred = length;
	} else if (xferred < length && !obj_request->result) {
		if (obj_request->type == OBJ_REQUEST_BIO)
			zero_bio_chain(obj_request->bio_list, xferred);
		else
			zero_pages(obj_request->pages, xferred, length);
		obj_request->xferred = length;
	}
	obj_request_done_set(obj_request);
}

static void rbd_obj_request_complete(struct rbd_obj_request *obj_request)
{
	dout("%s: obj %p cb %p\n", __func__, obj_request,
		obj_request->callback);
	if (obj_request->callback)
		obj_request->callback(obj_request);
	else
		complete_all(&obj_request->completion);
}

static void rbd_osd_trivial_callback(struct rbd_obj_request *obj_request)
{
	dout("%s: obj %p\n", __func__, obj_request);
	obj_request_done_set(obj_request);
}

static void rbd_osd_read_callback(struct rbd_obj_request *obj_request)
{
	struct rbd_img_request *img_request = NULL;
	struct rbd_device *rbd_dev = NULL;
	bool layered = false;

	if (obj_request_img_data_test(obj_request)) {
		img_request = obj_request->img_request;
		layered = img_request && img_request_layered_test(img_request);
		rbd_dev = img_request->rbd_dev;
	}

	dout("%s: obj %p img %p result %d %llu/%llu\n", __func__,
		obj_request, img_request, obj_request->result,
		obj_request->xferred, obj_request->length);
	if (layered && obj_request->result == -ENOENT &&
			obj_request->img_offset < rbd_dev->parent_overlap)
		rbd_img_parent_read(obj_request);
	else if (img_request)
		rbd_img_obj_request_read_callback(obj_request);
	else
		obj_request_done_set(obj_request);
}

static void rbd_osd_write_callback(struct rbd_obj_request *obj_request)
{
	dout("%s: obj %p result %d %llu\n", __func__, obj_request,
		obj_request->result, obj_request->length);
	/*
	 * There is no such thing as a successful short write.  Set
	 * it to our originally-requested length.
	 */
	obj_request->xferred = obj_request->length;
	obj_request_done_set(obj_request);
}

/*
 * For a simple stat call there's nothing to do.  We'll do more if
 * this is part of a write sequence for a layered image.
 */
static void rbd_osd_stat_callback(struct rbd_obj_request *obj_request)
{
	dout("%s: obj %p\n", __func__, obj_request);
	obj_request_done_set(obj_request);
}

static void rbd_osd_req_callback(struct ceph_osd_request *osd_req,
				struct ceph_msg *msg)
{
	struct rbd_obj_request *obj_request = osd_req->r_priv;
	u16 opcode;

	dout("%s: osd_req %p msg %p\n", __func__, osd_req, msg);
	rbd_assert(osd_req == obj_request->osd_req);
	if (obj_request_img_data_test(obj_request)) {
		rbd_assert(obj_request->img_request);
		rbd_assert(obj_request->which != BAD_WHICH);
	} else {
		rbd_assert(obj_request->which == BAD_WHICH);
	}

	if (osd_req->r_result < 0)
		obj_request->result = osd_req->r_result;
	obj_request->version = le64_to_cpu(osd_req->r_reassert_version.version);

	BUG_ON(osd_req->r_num_ops > 2);

	/*
	 * We support a 64-bit length, but ultimately it has to be
	 * passed to blk_end_request(), which takes an unsigned int.
	 */
	obj_request->xferred = osd_req->r_reply_op_len[0];
	rbd_assert(obj_request->xferred < (u64)UINT_MAX);
	opcode = osd_req->r_ops[0].op;
	switch (opcode) {
	case CEPH_OSD_OP_READ:
		rbd_osd_read_callback(obj_request);
		break;
	case CEPH_OSD_OP_WRITE:
		rbd_osd_write_callback(obj_request);
		break;
	case CEPH_OSD_OP_STAT:
		rbd_osd_stat_callback(obj_request);
		break;
	case CEPH_OSD_OP_CALL:
	case CEPH_OSD_OP_NOTIFY_ACK:
	case CEPH_OSD_OP_WATCH:
		rbd_osd_trivial_callback(obj_request);
		break;
	default:
		rbd_warn(NULL, "%s: unsupported op %hu\n",
			obj_request->object_name, (unsigned short) opcode);
		break;
	}

	if (obj_request_done_test(obj_request))
		rbd_obj_request_complete(obj_request);
}

static void rbd_osd_req_format_read(struct rbd_obj_request *obj_request)
{
	struct rbd_img_request *img_request = obj_request->img_request;
	struct ceph_osd_request *osd_req = obj_request->osd_req;
	u64 snap_id;

	rbd_assert(osd_req != NULL);

	snap_id = img_request ? img_request->snap_id : CEPH_NOSNAP;
	ceph_osdc_build_request(osd_req, obj_request->offset,
			NULL, snap_id, NULL);
}

static void rbd_osd_req_format_write(struct rbd_obj_request *obj_request)
{
	struct rbd_img_request *img_request = obj_request->img_request;
	struct ceph_osd_request *osd_req = obj_request->osd_req;
	struct ceph_snap_context *snapc;
	struct timespec mtime = CURRENT_TIME;

	rbd_assert(osd_req != NULL);

	snapc = img_request ? img_request->snapc : NULL;
	ceph_osdc_build_request(osd_req, obj_request->offset,
			snapc, CEPH_NOSNAP, &mtime);
}

static struct ceph_osd_request *rbd_osd_req_create(
					struct rbd_device *rbd_dev,
					bool write_request,
					struct rbd_obj_request *obj_request)
{
	struct ceph_snap_context *snapc = NULL;
	struct ceph_osd_client *osdc;
	struct ceph_osd_request *osd_req;

	if (obj_request_img_data_test(obj_request)) {
		struct rbd_img_request *img_request = obj_request->img_request;

		rbd_assert(write_request ==
				img_request_write_test(img_request));
		if (write_request)
			snapc = img_request->snapc;
	}

	/* Allocate and initialize the request, for the single op */

	osdc = &rbd_dev->rbd_client->client->osdc;
	osd_req = ceph_osdc_alloc_request(osdc, snapc, 1, false, GFP_ATOMIC);
	if (!osd_req)
		return NULL;	/* ENOMEM */

	if (write_request)
		osd_req->r_flags = CEPH_OSD_FLAG_WRITE | CEPH_OSD_FLAG_ONDISK;
	else
		osd_req->r_flags = CEPH_OSD_FLAG_READ;

	osd_req->r_callback = rbd_osd_req_callback;
	osd_req->r_priv = obj_request;

	osd_req->r_oid_len = strlen(obj_request->object_name);
	rbd_assert(osd_req->r_oid_len < sizeof (osd_req->r_oid));
	memcpy(osd_req->r_oid, obj_request->object_name, osd_req->r_oid_len);

	osd_req->r_file_layout = rbd_dev->layout;	/* struct */

	return osd_req;
}

/*
 * Create a copyup osd request based on the information in the
 * object request supplied.  A copyup request has two osd ops,
 * a copyup method call, and a "normal" write request.
 */
static struct ceph_osd_request *
rbd_osd_req_create_copyup(struct rbd_obj_request *obj_request)
{
	struct rbd_img_request *img_request;
	struct ceph_snap_context *snapc;
	struct rbd_device *rbd_dev;
	struct ceph_osd_client *osdc;
	struct ceph_osd_request *osd_req;

	rbd_assert(obj_request_img_data_test(obj_request));
	img_request = obj_request->img_request;
	rbd_assert(img_request);
	rbd_assert(img_request_write_test(img_request));

	/* Allocate and initialize the request, for the two ops */

	snapc = img_request->snapc;
	rbd_dev = img_request->rbd_dev;
	osdc = &rbd_dev->rbd_client->client->osdc;
	osd_req = ceph_osdc_alloc_request(osdc, snapc, 2, false, GFP_ATOMIC);
	if (!osd_req)
		return NULL;	/* ENOMEM */

	osd_req->r_flags = CEPH_OSD_FLAG_WRITE | CEPH_OSD_FLAG_ONDISK;
	osd_req->r_callback = rbd_osd_req_callback;
	osd_req->r_priv = obj_request;

	osd_req->r_oid_len = strlen(obj_request->object_name);
	rbd_assert(osd_req->r_oid_len < sizeof (osd_req->r_oid));
	memcpy(osd_req->r_oid, obj_request->object_name, osd_req->r_oid_len);

	osd_req->r_file_layout = rbd_dev->layout;	/* struct */

	return osd_req;
}


static void rbd_osd_req_destroy(struct ceph_osd_request *osd_req)
{
	ceph_osdc_put_request(osd_req);
}

/* object_name is assumed to be a non-null pointer and NUL-terminated */

static struct rbd_obj_request *rbd_obj_request_create(const char *object_name,
						u64 offset, u64 length,
						enum obj_request_type type)
{
	struct rbd_obj_request *obj_request;
	size_t size;
	char *name;

	rbd_assert(obj_request_type_valid(type));

	size = strlen(object_name) + 1;
	obj_request = kzalloc(sizeof (*obj_request) + size, GFP_KERNEL);
	if (!obj_request)
		return NULL;

	name = (char *)(obj_request + 1);
	obj_request->object_name = memcpy(name, object_name, size);
	obj_request->offset = offset;
	obj_request->length = length;
	obj_request->flags = 0;
	obj_request->which = BAD_WHICH;
	obj_request->type = type;
	INIT_LIST_HEAD(&obj_request->links);
	init_completion(&obj_request->completion);
	kref_init(&obj_request->kref);

	dout("%s: \"%s\" %llu/%llu %d -> obj %p\n", __func__, object_name,
		offset, length, (int)type, obj_request);

	return obj_request;
}

static void rbd_obj_request_destroy(struct kref *kref)
{
	struct rbd_obj_request *obj_request;

	obj_request = container_of(kref, struct rbd_obj_request, kref);

	dout("%s: obj %p\n", __func__, obj_request);

	rbd_assert(obj_request->img_request == NULL);
	rbd_assert(obj_request->which == BAD_WHICH);

	if (obj_request->osd_req)
		rbd_osd_req_destroy(obj_request->osd_req);

	rbd_assert(obj_request_type_valid(obj_request->type));
	switch (obj_request->type) {
	case OBJ_REQUEST_NODATA:
		break;		/* Nothing to do */
	case OBJ_REQUEST_BIO:
		if (obj_request->bio_list)
			bio_chain_put(obj_request->bio_list);
		break;
	case OBJ_REQUEST_PAGES:
		if (obj_request->pages)
			ceph_release_page_vector(obj_request->pages,
						obj_request->page_count);
		break;
	}

	kfree(obj_request);
}

/*
 * Caller is responsible for filling in the list of object requests
 * that comprises the image request, and the Linux request pointer
 * (if there is one).
 */
static struct rbd_img_request *rbd_img_request_create(
					struct rbd_device *rbd_dev,
					u64 offset, u64 length,
					bool write_request,
					bool child_request)
{
	struct rbd_img_request *img_request;
	struct ceph_snap_context *snapc = NULL;

	img_request = kmalloc(sizeof (*img_request), GFP_ATOMIC);
	if (!img_request)
		return NULL;

	if (write_request) {
		down_read(&rbd_dev->header_rwsem);
		snapc = ceph_get_snap_context(rbd_dev->header.snapc);
		up_read(&rbd_dev->header_rwsem);
		if (WARN_ON(!snapc)) {
			kfree(img_request);
			return NULL;	/* Shouldn't happen */
		}

	}

	img_request->rq = NULL;
	img_request->rbd_dev = rbd_dev;
	img_request->offset = offset;
	img_request->length = length;
	img_request->flags = 0;
	if (write_request) {
		img_request_write_set(img_request);
		img_request->snapc = snapc;
	} else {
		img_request->snap_id = rbd_dev->spec->snap_id;
	}
	if (child_request)
		img_request_child_set(img_request);
	if (rbd_dev->parent_spec)
		img_request_layered_set(img_request);
	spin_lock_init(&img_request->completion_lock);
	img_request->next_completion = 0;
	img_request->callback = NULL;
	img_request->result = 0;
	img_request->obj_request_count = 0;
	INIT_LIST_HEAD(&img_request->obj_requests);
	kref_init(&img_request->kref);

	rbd_img_request_get(img_request);	/* Avoid a warning */
	rbd_img_request_put(img_request);	/* TEMPORARY */

	dout("%s: rbd_dev %p %s %llu/%llu -> img %p\n", __func__, rbd_dev,
		write_request ? "write" : "read", offset, length,
		img_request);

	return img_request;
}

static void rbd_img_request_destroy(struct kref *kref)
{
	struct rbd_img_request *img_request;
	struct rbd_obj_request *obj_request;
	struct rbd_obj_request *next_obj_request;

	img_request = container_of(kref, struct rbd_img_request, kref);

	dout("%s: img %p\n", __func__, img_request);

	for_each_obj_request_safe(img_request, obj_request, next_obj_request)
		rbd_img_obj_request_del(img_request, obj_request);
	rbd_assert(img_request->obj_request_count == 0);

	if (img_request_write_test(img_request))
		ceph_put_snap_context(img_request->snapc);

	if (img_request_child_test(img_request))
		rbd_obj_request_put(img_request->obj_request);

	kfree(img_request);
}

static bool rbd_img_obj_end_request(struct rbd_obj_request *obj_request)
{
	struct rbd_img_request *img_request;
	unsigned int xferred;
	int result;
	bool more;

	rbd_assert(obj_request_img_data_test(obj_request));
	img_request = obj_request->img_request;

	rbd_assert(obj_request->xferred <= (u64)UINT_MAX);
	xferred = (unsigned int)obj_request->xferred;
	result = obj_request->result;
	if (result) {
		struct rbd_device *rbd_dev = img_request->rbd_dev;

		rbd_warn(rbd_dev, "%s %llx at %llx (%llx)\n",
			img_request_write_test(img_request) ? "write" : "read",
			obj_request->length, obj_request->img_offset,
			obj_request->offset);
		rbd_warn(rbd_dev, "  result %d xferred %x\n",
			result, xferred);
		if (!img_request->result)
			img_request->result = result;
	}

	/* Image object requests don't own their page array */

	if (obj_request->type == OBJ_REQUEST_PAGES) {
		obj_request->pages = NULL;
		obj_request->page_count = 0;
	}

	if (img_request_child_test(img_request)) {
		rbd_assert(img_request->obj_request != NULL);
		more = obj_request->which < img_request->obj_request_count - 1;
	} else {
		rbd_assert(img_request->rq != NULL);
		more = blk_end_request(img_request->rq, result, xferred);
	}

	return more;
}

static void rbd_img_obj_callback(struct rbd_obj_request *obj_request)
{
	struct rbd_img_request *img_request;
	u32 which = obj_request->which;
	bool more = true;

	rbd_assert(obj_request_img_data_test(obj_request));
	img_request = obj_request->img_request;

	dout("%s: img %p obj %p\n", __func__, img_request, obj_request);
	rbd_assert(img_request != NULL);
	rbd_assert(img_request->obj_request_count > 0);
	rbd_assert(which != BAD_WHICH);
	rbd_assert(which < img_request->obj_request_count);
	rbd_assert(which >= img_request->next_completion);

	spin_lock_irq(&img_request->completion_lock);
	if (which != img_request->next_completion)
		goto out;

	for_each_obj_request_from(img_request, obj_request) {
		rbd_assert(more);
		rbd_assert(which < img_request->obj_request_count);

		if (!obj_request_done_test(obj_request))
			break;
		more = rbd_img_obj_end_request(obj_request);
		which++;
	}

	rbd_assert(more ^ (which == img_request->obj_request_count));
	img_request->next_completion = which;
out:
	spin_unlock_irq(&img_request->completion_lock);

	if (!more)
		rbd_img_request_complete(img_request);
}

/*
 * Split up an image request into one or more object requests, each
 * to a different object.  The "type" parameter indicates whether
 * "data_desc" is the pointer to the head of a list of bio
 * structures, or the base of a page array.  In either case this
 * function assumes data_desc describes memory sufficient to hold
 * all data described by the image request.
 */
static int rbd_img_request_fill(struct rbd_img_request *img_request,
					enum obj_request_type type,
					void *data_desc)
{
	struct rbd_device *rbd_dev = img_request->rbd_dev;
	struct rbd_obj_request *obj_request = NULL;
	struct rbd_obj_request *next_obj_request;
	bool write_request = img_request_write_test(img_request);
	struct bio *bio_list;
	unsigned int bio_offset = 0;
	struct page **pages;
	u64 img_offset;
	u64 resid;
	u16 opcode;

	dout("%s: img %p type %d data_desc %p\n", __func__, img_request,
		(int)type, data_desc);

	opcode = write_request ? CEPH_OSD_OP_WRITE : CEPH_OSD_OP_READ;
	img_offset = img_request->offset;
	resid = img_request->length;
	rbd_assert(resid > 0);

	if (type == OBJ_REQUEST_BIO) {
		bio_list = data_desc;
		rbd_assert(img_offset == bio_list->bi_sector << SECTOR_SHIFT);
	} else {
		rbd_assert(type == OBJ_REQUEST_PAGES);
		pages = data_desc;
	}

	while (resid) {
		struct ceph_osd_request *osd_req;
		const char *object_name;
		u64 offset;
		u64 length;

		object_name = rbd_segment_name(rbd_dev, img_offset);
		if (!object_name)
			goto out_unwind;
		offset = rbd_segment_offset(rbd_dev, img_offset);
		length = rbd_segment_length(rbd_dev, img_offset, resid);
		obj_request = rbd_obj_request_create(object_name,
						offset, length, type);
		kfree(object_name);	/* object request has its own copy */
		if (!obj_request)
			goto out_unwind;

		if (type == OBJ_REQUEST_BIO) {
			unsigned int clone_size;

			rbd_assert(length <= (u64)UINT_MAX);
			clone_size = (unsigned int)length;
			obj_request->bio_list =
					bio_chain_clone_range(&bio_list,
								&bio_offset,
								clone_size,
								GFP_ATOMIC);
			if (!obj_request->bio_list)
				goto out_partial;
		} else {
			unsigned int page_count;

			obj_request->pages = pages;
			page_count = (u32)calc_pages_for(offset, length);
			obj_request->page_count = page_count;
			if ((offset + length) & ~PAGE_MASK)
				page_count--;	/* more on last page */
			pages += page_count;
		}

		osd_req = rbd_osd_req_create(rbd_dev, write_request,
						obj_request);
		if (!osd_req)
			goto out_partial;
		obj_request->osd_req = osd_req;
		obj_request->callback = rbd_img_obj_callback;

		osd_req_op_extent_init(osd_req, 0, opcode, offset, length,
						0, 0);
		if (type == OBJ_REQUEST_BIO)
			osd_req_op_extent_osd_data_bio(osd_req, 0,
					obj_request->bio_list, length);
		else
			osd_req_op_extent_osd_data_pages(osd_req, 0,
					obj_request->pages, length,
					offset & ~PAGE_MASK, false, false);

		if (write_request)
			rbd_osd_req_format_write(obj_request);
		else
			rbd_osd_req_format_read(obj_request);

		obj_request->img_offset = img_offset;
		rbd_img_obj_request_add(img_request, obj_request);

		img_offset += length;
		resid -= length;
	}

	return 0;

out_partial:
	rbd_obj_request_put(obj_request);
out_unwind:
	for_each_obj_request_safe(img_request, obj_request, next_obj_request)
		rbd_obj_request_put(obj_request);

	return -ENOMEM;
}

static void
rbd_img_obj_copyup_callback(struct rbd_obj_request *obj_request)
{
	struct rbd_img_request *img_request;
	struct rbd_device *rbd_dev;
	u64 length;
	u32 page_count;

	rbd_assert(obj_request->type == OBJ_REQUEST_BIO);
	rbd_assert(obj_request_img_data_test(obj_request));
	img_request = obj_request->img_request;
	rbd_assert(img_request);

	rbd_dev = img_request->rbd_dev;
	rbd_assert(rbd_dev);
	length = (u64)1 << rbd_dev->header.obj_order;
	page_count = (u32)calc_pages_for(0, length);

	rbd_assert(obj_request->copyup_pages);
	ceph_release_page_vector(obj_request->copyup_pages, page_count);
	obj_request->copyup_pages = NULL;

	/*
	 * We want the transfer count to reflect the size of the
	 * original write request.  There is no such thing as a
	 * successful short write, so if the request was successful
	 * we can just set it to the originally-requested length.
	 */
	if (!obj_request->result)
		obj_request->xferred = obj_request->length;

	/* Finish up with the normal image object callback */

	rbd_img_obj_callback(obj_request);
}

static void
rbd_img_obj_parent_read_full_callback(struct rbd_img_request *img_request)
{
	struct rbd_obj_request *orig_request;
	struct ceph_osd_request *osd_req;
	struct ceph_osd_client *osdc;
	struct rbd_device *rbd_dev;
	struct page **pages;
	int result;
	u64 obj_size;
	u64 xferred;

	rbd_assert(img_request_child_test(img_request));

	/* First get what we need from the image request */

	pages = img_request->copyup_pages;
	rbd_assert(pages != NULL);
	img_request->copyup_pages = NULL;

	orig_request = img_request->obj_request;
	rbd_assert(orig_request != NULL);
	rbd_assert(orig_request->type == OBJ_REQUEST_BIO);
	result = img_request->result;
	obj_size = img_request->length;
	xferred = img_request->xferred;

	rbd_dev = img_request->rbd_dev;
	rbd_assert(rbd_dev);
	rbd_assert(obj_size == (u64)1 << rbd_dev->header.obj_order);

	rbd_img_request_put(img_request);

	if (result)
		goto out_err;

	/* Allocate the new copyup osd request for the original request */

	result = -ENOMEM;
	rbd_assert(!orig_request->osd_req);
	osd_req = rbd_osd_req_create_copyup(orig_request);
	if (!osd_req)
		goto out_err;
	orig_request->osd_req = osd_req;
	orig_request->copyup_pages = pages;

	/* Initialize the copyup op */

	osd_req_op_cls_init(osd_req, 0, CEPH_OSD_OP_CALL, "rbd", "copyup");
	osd_req_op_cls_request_data_pages(osd_req, 0, pages, obj_size, 0,
						false, false);

	/* Then the original write request op */

	osd_req_op_extent_init(osd_req, 1, CEPH_OSD_OP_WRITE,
					orig_request->offset,
					orig_request->length, 0, 0);
	osd_req_op_extent_osd_data_bio(osd_req, 1, orig_request->bio_list,
					orig_request->length);

	rbd_osd_req_format_write(orig_request);

	/* All set, send it off. */

	orig_request->callback = rbd_img_obj_copyup_callback;
	osdc = &rbd_dev->rbd_client->client->osdc;
	result = rbd_obj_request_submit(osdc, orig_request);
	if (!result)
		return;
out_err:
	/* Record the error code and complete the request */

	orig_request->result = result;
	orig_request->xferred = 0;
	obj_request_done_set(orig_request);
	rbd_obj_request_complete(orig_request);
}

/*
 * Read from the parent image the range of data that covers the
 * entire target of the given object request.  This is used for
 * satisfying a layered image write request when the target of an
 * object request from the image request does not exist.
 *
 * A page array big enough to hold the returned data is allocated
 * and supplied to rbd_img_request_fill() as the "data descriptor."
 * When the read completes, this page array will be transferred to
 * the original object request for the copyup operation.
 *
 * If an error occurs, record it as the result of the original
 * object request and mark it done so it gets completed.
 */
static int rbd_img_obj_parent_read_full(struct rbd_obj_request *obj_request)
{
	struct rbd_img_request *img_request = NULL;
	struct rbd_img_request *parent_request = NULL;
	struct rbd_device *rbd_dev;
	u64 img_offset;
	u64 length;
	struct page **pages = NULL;
	u32 page_count;
	int result;

	rbd_assert(obj_request_img_data_test(obj_request));
	rbd_assert(obj_request->type == OBJ_REQUEST_BIO);

	img_request = obj_request->img_request;
	rbd_assert(img_request != NULL);
	rbd_dev = img_request->rbd_dev;
	rbd_assert(rbd_dev->parent != NULL);

	/*
	 * First things first.  The original osd request is of no
	 * use to use any more, we'll need a new one that can hold
	 * the two ops in a copyup request.  We'll get that later,
	 * but for now we can release the old one.
	 */
	rbd_osd_req_destroy(obj_request->osd_req);
	obj_request->osd_req = NULL;

	/*
	 * Determine the byte range covered by the object in the
	 * child image to which the original request was to be sent.
	 */
	img_offset = obj_request->img_offset - obj_request->offset;
	length = (u64)1 << rbd_dev->header.obj_order;

	/*
	 * There is no defined parent data beyond the parent
	 * overlap, so limit what we read at that boundary if
	 * necessary.
	 */
	if (img_offset + length > rbd_dev->parent_overlap) {
		rbd_assert(img_offset < rbd_dev->parent_overlap);
		length = rbd_dev->parent_overlap - img_offset;
	}

	/*
	 * Allocate a page array big enough to receive the data read
	 * from the parent.
	 */
	page_count = (u32)calc_pages_for(0, length);
	pages = ceph_alloc_page_vector(page_count, GFP_KERNEL);
	if (IS_ERR(pages)) {
		result = PTR_ERR(pages);
		pages = NULL;
		goto out_err;
	}

	result = -ENOMEM;
	parent_request = rbd_img_request_create(rbd_dev->parent,
						img_offset, length,
						false, true);
	if (!parent_request)
		goto out_err;
	rbd_obj_request_get(obj_request);
	parent_request->obj_request = obj_request;

	result = rbd_img_request_fill(parent_request, OBJ_REQUEST_PAGES, pages);
	if (result)
		goto out_err;
	parent_request->copyup_pages = pages;

	parent_request->callback = rbd_img_obj_parent_read_full_callback;
	result = rbd_img_request_submit(parent_request);
	if (!result)
		return 0;

	parent_request->copyup_pages = NULL;
	parent_request->obj_request = NULL;
	rbd_obj_request_put(obj_request);
out_err:
	if (pages)
		ceph_release_page_vector(pages, page_count);
	if (parent_request)
		rbd_img_request_put(parent_request);
	obj_request->result = result;
	obj_request->xferred = 0;
	obj_request_done_set(obj_request);

	return result;
}

static void rbd_img_obj_exists_callback(struct rbd_obj_request *obj_request)
{
	struct rbd_obj_request *orig_request;
	int result;

	rbd_assert(!obj_request_img_data_test(obj_request));

	/*
	 * All we need from the object request is the original
	 * request and the result of the STAT op.  Grab those, then
	 * we're done with the request.
	 */
	orig_request = obj_request->obj_request;
	obj_request->obj_request = NULL;
	rbd_assert(orig_request);
	rbd_assert(orig_request->img_request);

	result = obj_request->result;
	obj_request->result = 0;

	dout("%s: obj %p for obj %p result %d %llu/%llu\n", __func__,
		obj_request, orig_request, result,
		obj_request->xferred, obj_request->length);
	rbd_obj_request_put(obj_request);

	rbd_assert(orig_request);
	rbd_assert(orig_request->img_request);

	/*
	 * Our only purpose here is to determine whether the object
	 * exists, and we don't want to treat the non-existence as
	 * an error.  If something else comes back, transfer the
	 * error to the original request and complete it now.
	 */
	if (!result) {
		obj_request_existence_set(orig_request, true);
	} else if (result == -ENOENT) {
		obj_request_existence_set(orig_request, false);
	} else if (result) {
		orig_request->result = result;
		goto out;
	}

	/*
	 * Resubmit the original request now that we have recorded
	 * whether the target object exists.
	 */
	orig_request->result = rbd_img_obj_request_submit(orig_request);
out:
	if (orig_request->result)
		rbd_obj_request_complete(orig_request);
	rbd_obj_request_put(orig_request);
}

static int rbd_img_obj_exists_submit(struct rbd_obj_request *obj_request)
{
	struct rbd_obj_request *stat_request;
	struct rbd_device *rbd_dev;
	struct ceph_osd_client *osdc;
	struct page **pages = NULL;
	u32 page_count;
	size_t size;
	int ret;

	/*
	 * The response data for a STAT call consists of:
	 *     le64 length;
	 *     struct {
	 *         le32 tv_sec;
	 *         le32 tv_nsec;
	 *     } mtime;
	 */
	size = sizeof (__le64) + sizeof (__le32) + sizeof (__le32);
	page_count = (u32)calc_pages_for(0, size);
	pages = ceph_alloc_page_vector(page_count, GFP_KERNEL);
	if (IS_ERR(pages))
		return PTR_ERR(pages);

	ret = -ENOMEM;
	stat_request = rbd_obj_request_create(obj_request->object_name, 0, 0,
							OBJ_REQUEST_PAGES);
	if (!stat_request)
		goto out;

	rbd_obj_request_get(obj_request);
	stat_request->obj_request = obj_request;
	stat_request->pages = pages;
	stat_request->page_count = page_count;

	rbd_assert(obj_request->img_request);
	rbd_dev = obj_request->img_request->rbd_dev;
	stat_request->osd_req = rbd_osd_req_create(rbd_dev, false,
						stat_request);
	if (!stat_request->osd_req)
		goto out;
	stat_request->callback = rbd_img_obj_exists_callback;

	osd_req_op_init(stat_request->osd_req, 0, CEPH_OSD_OP_STAT);
	osd_req_op_raw_data_in_pages(stat_request->osd_req, 0, pages, size, 0,
					false, false);
	rbd_osd_req_format_read(stat_request);

	osdc = &rbd_dev->rbd_client->client->osdc;
	ret = rbd_obj_request_submit(osdc, stat_request);
out:
	if (ret)
		rbd_obj_request_put(obj_request);

	return ret;
}

static int rbd_img_obj_request_submit(struct rbd_obj_request *obj_request)
{
	struct rbd_img_request *img_request;
	struct rbd_device *rbd_dev;
	bool known;

	rbd_assert(obj_request_img_data_test(obj_request));

	img_request = obj_request->img_request;
	rbd_assert(img_request);
	rbd_dev = img_request->rbd_dev;

	/*
	 * Only writes to layered images need special handling.
	 * Reads and non-layered writes are simple object requests.
	 * Layered writes that start beyond the end of the overlap
	 * with the parent have no parent data, so they too are
	 * simple object requests.  Finally, if the target object is
	 * known to already exist, its parent data has already been
	 * copied, so a write to the object can also be handled as a
	 * simple object request.
	 */
	if (!img_request_write_test(img_request) ||
		!img_request_layered_test(img_request) ||
		rbd_dev->parent_overlap <= obj_request->img_offset ||
		((known = obj_request_known_test(obj_request)) &&
			obj_request_exists_test(obj_request))) {

		struct rbd_device *rbd_dev;
		struct ceph_osd_client *osdc;

		rbd_dev = obj_request->img_request->rbd_dev;
		osdc = &rbd_dev->rbd_client->client->osdc;

		return rbd_obj_request_submit(osdc, obj_request);
	}

	/*
	 * It's a layered write.  The target object might exist but
	 * we may not know that yet.  If we know it doesn't exist,
	 * start by reading the data for the full target object from
	 * the parent so we can use it for a copyup to the target.
	 */
	if (known)
		return rbd_img_obj_parent_read_full(obj_request);

	/* We don't know whether the target exists.  Go find out. */

	return rbd_img_obj_exists_submit(obj_request);
}

static int rbd_img_request_submit(struct rbd_img_request *img_request)
{
	struct rbd_obj_request *obj_request;
	struct rbd_obj_request *next_obj_request;

	dout("%s: img %p\n", __func__, img_request);
	for_each_obj_request_safe(img_request, obj_request, next_obj_request) {
		int ret;

		ret = rbd_img_obj_request_submit(obj_request);
		if (ret)
			return ret;
	}

	return 0;
}

static void rbd_img_parent_read_callback(struct rbd_img_request *img_request)
{
	struct rbd_obj_request *obj_request;
	struct rbd_device *rbd_dev;
	u64 obj_end;

	rbd_assert(img_request_child_test(img_request));

	obj_request = img_request->obj_request;
	rbd_assert(obj_request);
	rbd_assert(obj_request->img_request);

	obj_request->result = img_request->result;
	if (obj_request->result)
		goto out;

	/*
	 * We need to zero anything beyond the parent overlap
	 * boundary.  Since rbd_img_obj_request_read_callback()
	 * will zero anything beyond the end of a short read, an
	 * easy way to do this is to pretend the data from the
	 * parent came up short--ending at the overlap boundary.
	 */
	rbd_assert(obj_request->img_offset < U64_MAX - obj_request->length);
	obj_end = obj_request->img_offset + obj_request->length;
	rbd_dev = obj_request->img_request->rbd_dev;
	if (obj_end > rbd_dev->parent_overlap) {
		u64 xferred = 0;

		if (obj_request->img_offset < rbd_dev->parent_overlap)
			xferred = rbd_dev->parent_overlap -
					obj_request->img_offset;

		obj_request->xferred = min(img_request->xferred, xferred);
	} else {
		obj_request->xferred = img_request->xferred;
	}
out:
	rbd_img_obj_request_read_callback(obj_request);
	rbd_obj_request_complete(obj_request);
}

static void rbd_img_parent_read(struct rbd_obj_request *obj_request)
{
	struct rbd_device *rbd_dev;
	struct rbd_img_request *img_request;
	int result;

	rbd_assert(obj_request_img_data_test(obj_request));
	rbd_assert(obj_request->img_request != NULL);
	rbd_assert(obj_request->result == (s32) -ENOENT);
	rbd_assert(obj_request->type == OBJ_REQUEST_BIO);

	rbd_dev = obj_request->img_request->rbd_dev;
	rbd_assert(rbd_dev->parent != NULL);
	/* rbd_read_finish(obj_request, obj_request->length); */
	img_request = rbd_img_request_create(rbd_dev->parent,
						obj_request->img_offset,
						obj_request->length,
						false, true);
	result = -ENOMEM;
	if (!img_request)
		goto out_err;

	rbd_obj_request_get(obj_request);
	img_request->obj_request = obj_request;

	result = rbd_img_request_fill(img_request, OBJ_REQUEST_BIO,
					obj_request->bio_list);
	if (result)
		goto out_err;

	img_request->callback = rbd_img_parent_read_callback;
	result = rbd_img_request_submit(img_request);
	if (result)
		goto out_err;

	return;
out_err:
	if (img_request)
		rbd_img_request_put(img_request);
	obj_request->result = result;
	obj_request->xferred = 0;
	obj_request_done_set(obj_request);
}

static int rbd_obj_notify_ack(struct rbd_device *rbd_dev,
				   u64 ver, u64 notify_id)
{
	struct rbd_obj_request *obj_request;
	struct ceph_osd_client *osdc = &rbd_dev->rbd_client->client->osdc;
	int ret;

	obj_request = rbd_obj_request_create(rbd_dev->header_name, 0, 0,
							OBJ_REQUEST_NODATA);
	if (!obj_request)
		return -ENOMEM;

	ret = -ENOMEM;
	obj_request->osd_req = rbd_osd_req_create(rbd_dev, false, obj_request);
	if (!obj_request->osd_req)
		goto out;
	obj_request->callback = rbd_obj_request_put;

	osd_req_op_watch_init(obj_request->osd_req, 0, CEPH_OSD_OP_NOTIFY_ACK,
					notify_id, ver, 0);
	rbd_osd_req_format_read(obj_request);

	ret = rbd_obj_request_submit(osdc, obj_request);
out:
	if (ret)
		rbd_obj_request_put(obj_request);

	return ret;
}

static void rbd_watch_cb(u64 ver, u64 notify_id, u8 opcode, void *data)
{
	struct rbd_device *rbd_dev = (struct rbd_device *)data;
	u64 hver;

	if (!rbd_dev)
		return;

	dout("%s: \"%s\" notify_id %llu opcode %u\n", __func__,
		rbd_dev->header_name, (unsigned long long) notify_id,
		(unsigned int) opcode);
	(void)rbd_dev_refresh(rbd_dev, &hver);

	rbd_obj_notify_ack(rbd_dev, hver, notify_id);
}

/*
 * Request sync osd watch/unwatch.  The value of "start" determines
 * whether a watch request is being initiated or torn down.
 */
static int rbd_dev_header_watch_sync(struct rbd_device *rbd_dev, int start)
{
	struct ceph_osd_client *osdc = &rbd_dev->rbd_client->client->osdc;
	struct rbd_obj_request *obj_request;
	int ret;

	rbd_assert(start ^ !!rbd_dev->watch_event);
	rbd_assert(start ^ !!rbd_dev->watch_request);

	if (start) {
		ret = ceph_osdc_create_event(osdc, rbd_watch_cb, rbd_dev,
						&rbd_dev->watch_event);
		if (ret < 0)
			return ret;
		rbd_assert(rbd_dev->watch_event != NULL);
	}

	ret = -ENOMEM;
	obj_request = rbd_obj_request_create(rbd_dev->header_name, 0, 0,
							OBJ_REQUEST_NODATA);
	if (!obj_request)
		goto out_cancel;

	obj_request->osd_req = rbd_osd_req_create(rbd_dev, true, obj_request);
	if (!obj_request->osd_req)
		goto out_cancel;

	if (start)
		ceph_osdc_set_request_linger(osdc, obj_request->osd_req);
	else
		ceph_osdc_unregister_linger_request(osdc,
					rbd_dev->watch_request->osd_req);

	osd_req_op_watch_init(obj_request->osd_req, 0, CEPH_OSD_OP_WATCH,
				rbd_dev->watch_event->cookie,
				rbd_dev->header.obj_version, start);
	rbd_osd_req_format_write(obj_request);

	ret = rbd_obj_request_submit(osdc, obj_request);
	if (ret)
		goto out_cancel;
	ret = rbd_obj_request_wait(obj_request);
	if (ret)
		goto out_cancel;
	ret = obj_request->result;
	if (ret)
		goto out_cancel;

	/*
	 * A watch request is set to linger, so the underlying osd
	 * request won't go away until we unregister it.  We retain
	 * a pointer to the object request during that time (in
	 * rbd_dev->watch_request), so we'll keep a reference to
	 * it.  We'll drop that reference (below) after we've
	 * unregistered it.
	 */
	if (start) {
		rbd_dev->watch_request = obj_request;

		return 0;
	}

	/* We have successfully torn down the watch request */

	rbd_obj_request_put(rbd_dev->watch_request);
	rbd_dev->watch_request = NULL;
out_cancel:
	/* Cancel the event if we're tearing down, or on error */
	ceph_osdc_cancel_event(rbd_dev->watch_event);
	rbd_dev->watch_event = NULL;
	if (obj_request)
		rbd_obj_request_put(obj_request);

	return ret;
}

/*
 * Synchronous osd object method call.  Returns the number of bytes
 * returned in the outbound buffer, or a negative error code.
 */
static int rbd_obj_method_sync(struct rbd_device *rbd_dev,
			     const char *object_name,
			     const char *class_name,
			     const char *method_name,
			     const void *outbound,
			     size_t outbound_size,
			     void *inbound,
			     size_t inbound_size,
			     u64 *version)
{
	struct ceph_osd_client *osdc = &rbd_dev->rbd_client->client->osdc;
	struct rbd_obj_request *obj_request;
	struct page **pages;
	u32 page_count;
	int ret;

	/*
	 * Method calls are ultimately read operations.  The result
	 * should placed into the inbound buffer provided.  They
	 * also supply outbound data--parameters for the object
	 * method.  Currently if this is present it will be a
	 * snapshot id.
	 */
	page_count = (u32)calc_pages_for(0, inbound_size);
	pages = ceph_alloc_page_vector(page_count, GFP_KERNEL);
	if (IS_ERR(pages))
		return PTR_ERR(pages);

	ret = -ENOMEM;
	obj_request = rbd_obj_request_create(object_name, 0, inbound_size,
							OBJ_REQUEST_PAGES);
	if (!obj_request)
		goto out;

	obj_request->pages = pages;
	obj_request->page_count = page_count;

	obj_request->osd_req = rbd_osd_req_create(rbd_dev, false, obj_request);
	if (!obj_request->osd_req)
		goto out;

	osd_req_op_cls_init(obj_request->osd_req, 0, CEPH_OSD_OP_CALL,
					class_name, method_name);
	if (outbound_size) {
		struct ceph_pagelist *pagelist;

		pagelist = kmalloc(sizeof (*pagelist), GFP_NOFS);
		if (!pagelist)
			goto out;

		ceph_pagelist_init(pagelist);
		ceph_pagelist_append(pagelist, outbound, outbound_size);
		osd_req_op_cls_request_data_pagelist(obj_request->osd_req, 0,
						pagelist);
	}
	osd_req_op_cls_response_data_pages(obj_request->osd_req, 0,
					obj_request->pages, inbound_size,
					0, false, false);
	rbd_osd_req_format_read(obj_request);

	ret = rbd_obj_request_submit(osdc, obj_request);
	if (ret)
		goto out;
	ret = rbd_obj_request_wait(obj_request);
	if (ret)
		goto out;

	ret = obj_request->result;
	if (ret < 0)
		goto out;

	rbd_assert(obj_request->xferred < (u64)INT_MAX);
	ret = (int)obj_request->xferred;
	ceph_copy_from_page_vector(pages, inbound, 0, obj_request->xferred);
	if (version)
		*version = obj_request->version;
out:
	if (obj_request)
		rbd_obj_request_put(obj_request);
	else
		ceph_release_page_vector(pages, page_count);

	return ret;
}

static void rbd_request_fn(struct request_queue *q)
		__releases(q->queue_lock) __acquires(q->queue_lock)
{
	struct rbd_device *rbd_dev = q->queuedata;
	bool read_only = rbd_dev->mapping.read_only;
	struct request *rq;
	int result;

	while ((rq = blk_fetch_request(q))) {
		bool write_request = rq_data_dir(rq) == WRITE;
		struct rbd_img_request *img_request;
		u64 offset;
		u64 length;

		/* Ignore any non-FS requests that filter through. */

		if (rq->cmd_type != REQ_TYPE_FS) {
			dout("%s: non-fs request type %d\n", __func__,
				(int) rq->cmd_type);
			__blk_end_request_all(rq, 0);
			continue;
		}

		/* Ignore/skip any zero-length requests */

		offset = (u64) blk_rq_pos(rq) << SECTOR_SHIFT;
		length = (u64) blk_rq_bytes(rq);

		if (!length) {
			dout("%s: zero-length request\n", __func__);
			__blk_end_request_all(rq, 0);
			continue;
		}

		spin_unlock_irq(q->queue_lock);

		/* Disallow writes to a read-only device */

		if (write_request) {
			result = -EROFS;
			if (read_only)
				goto end_request;
			rbd_assert(rbd_dev->spec->snap_id == CEPH_NOSNAP);
		}

		/*
		 * Quit early if the mapped snapshot no longer
		 * exists.  It's still possible the snapshot will
		 * have disappeared by the time our request arrives
		 * at the osd, but there's no sense in sending it if
		 * we already know.
		 */
		if (!test_bit(RBD_DEV_FLAG_EXISTS, &rbd_dev->flags)) {
			dout("request for non-existent snapshot");
			rbd_assert(rbd_dev->spec->snap_id != CEPH_NOSNAP);
			result = -ENXIO;
			goto end_request;
		}

		result = -EINVAL;
		if (offset && length > U64_MAX - offset + 1) {
			rbd_warn(rbd_dev, "bad request range (%llu~%llu)\n",
				offset, length);
			goto end_request;	/* Shouldn't happen */
		}

		result = -ENOMEM;
		img_request = rbd_img_request_create(rbd_dev, offset, length,
							write_request, false);
		if (!img_request)
			goto end_request;

		img_request->rq = rq;

		result = rbd_img_request_fill(img_request, OBJ_REQUEST_BIO,
						rq->bio);
		if (!result)
			result = rbd_img_request_submit(img_request);
		if (result)
			rbd_img_request_put(img_request);
end_request:
		spin_lock_irq(q->queue_lock);
		if (result < 0) {
			rbd_warn(rbd_dev, "%s %llx at %llx result %d\n",
				write_request ? "write" : "read",
				length, offset, result);

			__blk_end_request_all(rq, result);
		}
	}
}

/*
 * a queue callback. Makes sure that we don't create a bio that spans across
 * multiple osd objects. One exception would be with a single page bios,
 * which we handle later at bio_chain_clone_range()
 */
static int rbd_merge_bvec(struct request_queue *q, struct bvec_merge_data *bmd,
			  struct bio_vec *bvec)
{
	struct rbd_device *rbd_dev = q->queuedata;
	sector_t sector_offset;
	sector_t sectors_per_obj;
	sector_t obj_sector_offset;
	int ret;

	/*
	 * Find how far into its rbd object the partition-relative
	 * bio start sector is to offset relative to the enclosing
	 * device.
	 */
	sector_offset = get_start_sect(bmd->bi_bdev) + bmd->bi_sector;
	sectors_per_obj = 1 << (rbd_dev->header.obj_order - SECTOR_SHIFT);
	obj_sector_offset = sector_offset & (sectors_per_obj - 1);

	/*
	 * Compute the number of bytes from that offset to the end
	 * of the object.  Account for what's already used by the bio.
	 */
	ret = (int) (sectors_per_obj - obj_sector_offset) << SECTOR_SHIFT;
	if (ret > bmd->bi_size)
		ret -= bmd->bi_size;
	else
		ret = 0;

	/*
	 * Don't send back more than was asked for.  And if the bio
	 * was empty, let the whole thing through because:  "Note
	 * that a block device *must* allow a single page to be
	 * added to an empty bio."
	 */
	rbd_assert(bvec->bv_len <= PAGE_SIZE);
	if (ret > (int) bvec->bv_len || !bmd->bi_size)
		ret = (int) bvec->bv_len;

	return ret;
}

static void rbd_free_disk(struct rbd_device *rbd_dev)
{
	struct gendisk *disk = rbd_dev->disk;

	if (!disk)
		return;

	rbd_dev->disk = NULL;
	if (disk->flags & GENHD_FL_UP) {
		del_gendisk(disk);
		if (disk->queue)
			blk_cleanup_queue(disk->queue);
	}
	put_disk(disk);
}

static int rbd_obj_read_sync(struct rbd_device *rbd_dev,
				const char *object_name,
				u64 offset, u64 length,
				void *buf, u64 *version)

{
	struct ceph_osd_client *osdc = &rbd_dev->rbd_client->client->osdc;
	struct rbd_obj_request *obj_request;
	struct page **pages = NULL;
	u32 page_count;
	size_t size;
	int ret;

	page_count = (u32) calc_pages_for(offset, length);
	pages = ceph_alloc_page_vector(page_count, GFP_KERNEL);
	if (IS_ERR(pages))
		ret = PTR_ERR(pages);

	ret = -ENOMEM;
	obj_request = rbd_obj_request_create(object_name, offset, length,
							OBJ_REQUEST_PAGES);
	if (!obj_request)
		goto out;

	obj_request->pages = pages;
	obj_request->page_count = page_count;

	obj_request->osd_req = rbd_osd_req_create(rbd_dev, false, obj_request);
	if (!obj_request->osd_req)
		goto out;

	osd_req_op_extent_init(obj_request->osd_req, 0, CEPH_OSD_OP_READ,
					offset, length, 0, 0);
	osd_req_op_extent_osd_data_pages(obj_request->osd_req, 0,
					obj_request->pages,
					obj_request->length,
					obj_request->offset & ~PAGE_MASK,
					false, false);
	rbd_osd_req_format_read(obj_request);

	ret = rbd_obj_request_submit(osdc, obj_request);
	if (ret)
		goto out;
	ret = rbd_obj_request_wait(obj_request);
	if (ret)
		goto out;

	ret = obj_request->result;
	if (ret < 0)
		goto out;

	rbd_assert(obj_request->xferred <= (u64) SIZE_MAX);
	size = (size_t) obj_request->xferred;
	ceph_copy_from_page_vector(pages, buf, 0, size);
	rbd_assert(size <= (size_t) INT_MAX);
	ret = (int) size;
	if (version)
		*version = obj_request->version;
out:
	if (obj_request)
		rbd_obj_request_put(obj_request);
	else
		ceph_release_page_vector(pages, page_count);

	return ret;
}

/*
 * Read the complete header for the given rbd device.
 *
 * Returns a pointer to a dynamically-allocated buffer containing
 * the complete and validated header.  Caller can pass the address
 * of a variable that will be filled in with the version of the
 * header object at the time it was read.
 *
 * Returns a pointer-coded errno if a failure occurs.
 */
static struct rbd_image_header_ondisk *
rbd_dev_v1_header_read(struct rbd_device *rbd_dev, u64 *version)
{
	struct rbd_image_header_ondisk *ondisk = NULL;
	u32 snap_count = 0;
	u64 names_size = 0;
	u32 want_count;
	int ret;

	/*
	 * The complete header will include an array of its 64-bit
	 * snapshot ids, followed by the names of those snapshots as
	 * a contiguous block of NUL-terminated strings.  Note that
	 * the number of snapshots could change by the time we read
	 * it in, in which case we re-read it.
	 */
	do {
		size_t size;

		kfree(ondisk);

		size = sizeof (*ondisk);
		size += snap_count * sizeof (struct rbd_image_snap_ondisk);
		size += names_size;
		ondisk = kmalloc(size, GFP_KERNEL);
		if (!ondisk)
			return ERR_PTR(-ENOMEM);

		ret = rbd_obj_read_sync(rbd_dev, rbd_dev->header_name,
				       0, size, ondisk, version);
		if (ret < 0)
			goto out_err;
		if ((size_t)ret < size) {
			ret = -ENXIO;
			rbd_warn(rbd_dev, "short header read (want %zd got %d)",
				size, ret);
			goto out_err;
		}
		if (!rbd_dev_ondisk_valid(ondisk)) {
			ret = -ENXIO;
			rbd_warn(rbd_dev, "invalid header");
			goto out_err;
		}

		names_size = le64_to_cpu(ondisk->snap_names_len);
		want_count = snap_count;
		snap_count = le32_to_cpu(ondisk->snap_count);
	} while (snap_count != want_count);

	return ondisk;

out_err:
	kfree(ondisk);

	return ERR_PTR(ret);
}

/*
 * reload the ondisk the header
 */
static int rbd_read_header(struct rbd_device *rbd_dev,
			   struct rbd_image_header *header)
{
	struct rbd_image_header_ondisk *ondisk;
	u64 ver = 0;
	int ret;

	ondisk = rbd_dev_v1_header_read(rbd_dev, &ver);
	if (IS_ERR(ondisk))
		return PTR_ERR(ondisk);
	ret = rbd_header_from_disk(header, ondisk);
	if (ret >= 0)
		header->obj_version = ver;
	kfree(ondisk);

	return ret;
}

static void rbd_remove_all_snaps(struct rbd_device *rbd_dev)
{
	struct rbd_snap *snap;
	struct rbd_snap *next;

	list_for_each_entry_safe(snap, next, &rbd_dev->snaps, node) {
		list_del(&snap->node);
		rbd_snap_destroy(snap);
	}
}

static void rbd_update_mapping_size(struct rbd_device *rbd_dev)
{
	sector_t size;

	if (rbd_dev->spec->snap_id != CEPH_NOSNAP)
		return;

	size = (sector_t) rbd_dev->header.image_size / SECTOR_SIZE;
	dout("setting size to %llu sectors", (unsigned long long) size);
	rbd_dev->mapping.size = (u64) size;
	set_capacity(rbd_dev->disk, size);
}

/*
 * only read the first part of the ondisk header, without the snaps info
 */
static int rbd_dev_v1_refresh(struct rbd_device *rbd_dev, u64 *hver)
{
	int ret;
	struct rbd_image_header h;

	ret = rbd_read_header(rbd_dev, &h);
	if (ret < 0)
		return ret;

	down_write(&rbd_dev->header_rwsem);

	/* Update image size, and check for resize of mapped image */
	rbd_dev->header.image_size = h.image_size;
	rbd_update_mapping_size(rbd_dev);

	/* rbd_dev->header.object_prefix shouldn't change */
	kfree(rbd_dev->header.snap_sizes);
	kfree(rbd_dev->header.snap_names);
	/* osd requests may still refer to snapc */
	ceph_put_snap_context(rbd_dev->header.snapc);

	if (hver)
		*hver = h.obj_version;
	rbd_dev->header.obj_version = h.obj_version;
	rbd_dev->header.image_size = h.image_size;
	rbd_dev->header.snapc = h.snapc;
	rbd_dev->header.snap_names = h.snap_names;
	rbd_dev->header.snap_sizes = h.snap_sizes;
	/* Free the extra copy of the object prefix */
	if (strcmp(rbd_dev->header.object_prefix, h.object_prefix))
		rbd_warn(rbd_dev, "object prefix changed (ignoring)");
	kfree(h.object_prefix);

	ret = rbd_dev_snaps_update(rbd_dev);

	up_write(&rbd_dev->header_rwsem);

	return ret;
}

static int rbd_dev_refresh(struct rbd_device *rbd_dev, u64 *hver)
{
	int ret;

	rbd_assert(rbd_image_format_valid(rbd_dev->image_format));
	mutex_lock_nested(&ctl_mutex, SINGLE_DEPTH_NESTING);
	if (rbd_dev->image_format == 1)
		ret = rbd_dev_v1_refresh(rbd_dev, hver);
	else
		ret = rbd_dev_v2_refresh(rbd_dev, hver);
	mutex_unlock(&ctl_mutex);
	revalidate_disk(rbd_dev->disk);
	if (ret)
		rbd_warn(rbd_dev, "got notification but failed to "
			   " update snaps: %d\n", ret);

	return ret;
}

static int rbd_init_disk(struct rbd_device *rbd_dev)
{
	struct gendisk *disk;
	struct request_queue *q;
	u64 segment_size;

	/* create gendisk info */
	disk = alloc_disk(RBD_MINORS_PER_MAJOR);
	if (!disk)
		return -ENOMEM;

	snprintf(disk->disk_name, sizeof(disk->disk_name), RBD_DRV_NAME "%d",
		 rbd_dev->dev_id);
	disk->major = rbd_dev->major;
	disk->first_minor = 0;
	disk->fops = &rbd_bd_ops;
	disk->private_data = rbd_dev;

	q = blk_init_queue(rbd_request_fn, &rbd_dev->lock);
	if (!q)
		goto out_disk;

	/* We use the default size, but let's be explicit about it. */
	blk_queue_physical_block_size(q, SECTOR_SIZE);

	/* set io sizes to object size */
	segment_size = rbd_obj_bytes(&rbd_dev->header);
	blk_queue_max_hw_sectors(q, segment_size / SECTOR_SIZE);
	blk_queue_max_segment_size(q, segment_size);
	blk_queue_io_min(q, segment_size);
	blk_queue_io_opt(q, segment_size);

	blk_queue_merge_bvec(q, rbd_merge_bvec);
	disk->queue = q;

	q->queuedata = rbd_dev;

	rbd_dev->disk = disk;

	set_capacity(rbd_dev->disk, rbd_dev->mapping.size / SECTOR_SIZE);

	return 0;
out_disk:
	put_disk(disk);

	return -ENOMEM;
}

/*
  sysfs
*/

static struct rbd_device *dev_to_rbd_dev(struct device *dev)
{
	return container_of(dev, struct rbd_device, dev);
}

static ssize_t rbd_size_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct rbd_device *rbd_dev = dev_to_rbd_dev(dev);
	sector_t size;

	down_read(&rbd_dev->header_rwsem);
	size = get_capacity(rbd_dev->disk);
	up_read(&rbd_dev->header_rwsem);

	return sprintf(buf, "%llu\n", (unsigned long long) size * SECTOR_SIZE);
}

/*
 * Note this shows the features for whatever's mapped, which is not
 * necessarily the base image.
 */
static ssize_t rbd_features_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct rbd_device *rbd_dev = dev_to_rbd_dev(dev);

	return sprintf(buf, "0x%016llx\n",
			(unsigned long long) rbd_dev->mapping.features);
}

static ssize_t rbd_major_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct rbd_device *rbd_dev = dev_to_rbd_dev(dev);

	return sprintf(buf, "%d\n", rbd_dev->major);
}

static ssize_t rbd_client_id_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct rbd_device *rbd_dev = dev_to_rbd_dev(dev);

	return sprintf(buf, "client%lld\n",
			ceph_client_id(rbd_dev->rbd_client->client));
}

static ssize_t rbd_pool_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct rbd_device *rbd_dev = dev_to_rbd_dev(dev);

	return sprintf(buf, "%s\n", rbd_dev->spec->pool_name);
}

static ssize_t rbd_pool_id_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct rbd_device *rbd_dev = dev_to_rbd_dev(dev);

	return sprintf(buf, "%llu\n",
		(unsigned long long) rbd_dev->spec->pool_id);
}

static ssize_t rbd_name_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct rbd_device *rbd_dev = dev_to_rbd_dev(dev);

	if (rbd_dev->spec->image_name)
		return sprintf(buf, "%s\n", rbd_dev->spec->image_name);

	return sprintf(buf, "(unknown)\n");
}

static ssize_t rbd_image_id_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct rbd_device *rbd_dev = dev_to_rbd_dev(dev);

	return sprintf(buf, "%s\n", rbd_dev->spec->image_id);
}

/*
 * Shows the name of the currently-mapped snapshot (or
 * RBD_SNAP_HEAD_NAME for the base image).
 */
static ssize_t rbd_snap_show(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
	struct rbd_device *rbd_dev = dev_to_rbd_dev(dev);

	return sprintf(buf, "%s\n", rbd_dev->spec->snap_name);
}

/*
 * For an rbd v2 image, shows the pool id, image id, and snapshot id
 * for the parent image.  If there is no parent, simply shows
 * "(no parent image)".
 */
static ssize_t rbd_parent_show(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
	struct rbd_device *rbd_dev = dev_to_rbd_dev(dev);
	struct rbd_spec *spec = rbd_dev->parent_spec;
	int count;
	char *bufp = buf;

	if (!spec)
		return sprintf(buf, "(no parent image)\n");

	count = sprintf(bufp, "pool_id %llu\npool_name %s\n",
			(unsigned long long) spec->pool_id, spec->pool_name);
	if (count < 0)
		return count;
	bufp += count;

	count = sprintf(bufp, "image_id %s\nimage_name %s\n", spec->image_id,
			spec->image_name ? spec->image_name : "(unknown)");
	if (count < 0)
		return count;
	bufp += count;

	count = sprintf(bufp, "snap_id %llu\nsnap_name %s\n",
			(unsigned long long) spec->snap_id, spec->snap_name);
	if (count < 0)
		return count;
	bufp += count;

	count = sprintf(bufp, "overlap %llu\n", rbd_dev->parent_overlap);
	if (count < 0)
		return count;
	bufp += count;

	return (ssize_t) (bufp - buf);
}

static ssize_t rbd_image_refresh(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf,
				 size_t size)
{
	struct rbd_device *rbd_dev = dev_to_rbd_dev(dev);
	int ret;

	ret = rbd_dev_refresh(rbd_dev, NULL);

	return ret < 0 ? ret : size;
}

static DEVICE_ATTR(size, S_IRUGO, rbd_size_show, NULL);
static DEVICE_ATTR(features, S_IRUGO, rbd_features_show, NULL);
static DEVICE_ATTR(major, S_IRUGO, rbd_major_show, NULL);
static DEVICE_ATTR(client_id, S_IRUGO, rbd_client_id_show, NULL);
static DEVICE_ATTR(pool, S_IRUGO, rbd_pool_show, NULL);
static DEVICE_ATTR(pool_id, S_IRUGO, rbd_pool_id_show, NULL);
static DEVICE_ATTR(name, S_IRUGO, rbd_name_show, NULL);
static DEVICE_ATTR(image_id, S_IRUGO, rbd_image_id_show, NULL);
static DEVICE_ATTR(refresh, S_IWUSR, NULL, rbd_image_refresh);
static DEVICE_ATTR(current_snap, S_IRUGO, rbd_snap_show, NULL);
static DEVICE_ATTR(parent, S_IRUGO, rbd_parent_show, NULL);

static struct attribute *rbd_attrs[] = {
	&dev_attr_size.attr,
	&dev_attr_features.attr,
	&dev_attr_major.attr,
	&dev_attr_client_id.attr,
	&dev_attr_pool.attr,
	&dev_attr_pool_id.attr,
	&dev_attr_name.attr,
	&dev_attr_image_id.attr,
	&dev_attr_current_snap.attr,
	&dev_attr_parent.attr,
	&dev_attr_refresh.attr,
	NULL
};

static struct attribute_group rbd_attr_group = {
	.attrs = rbd_attrs,
};

static const struct attribute_group *rbd_attr_groups[] = {
	&rbd_attr_group,
	NULL
};

static void rbd_sysfs_dev_release(struct device *dev)
{
}

static struct device_type rbd_device_type = {
	.name		= "rbd",
	.groups		= rbd_attr_groups,
	.release	= rbd_sysfs_dev_release,
};

static struct rbd_spec *rbd_spec_get(struct rbd_spec *spec)
{
	kref_get(&spec->kref);

	return spec;
}

static void rbd_spec_free(struct kref *kref);
static void rbd_spec_put(struct rbd_spec *spec)
{
	if (spec)
		kref_put(&spec->kref, rbd_spec_free);
}

static struct rbd_spec *rbd_spec_alloc(void)
{
	struct rbd_spec *spec;

	spec = kzalloc(sizeof (*spec), GFP_KERNEL);
	if (!spec)
		return NULL;
	kref_init(&spec->kref);

	return spec;
}

static void rbd_spec_free(struct kref *kref)
{
	struct rbd_spec *spec = container_of(kref, struct rbd_spec, kref);

	kfree(spec->pool_name);
	kfree(spec->image_id);
	kfree(spec->image_name);
	kfree(spec->snap_name);
	kfree(spec);
}

static struct rbd_device *rbd_dev_create(struct rbd_client *rbdc,
				struct rbd_spec *spec)
{
	struct rbd_device *rbd_dev;

	rbd_dev = kzalloc(sizeof (*rbd_dev), GFP_KERNEL);
	if (!rbd_dev)
		return NULL;

	spin_lock_init(&rbd_dev->lock);
	rbd_dev->flags = 0;
	INIT_LIST_HEAD(&rbd_dev->node);
	INIT_LIST_HEAD(&rbd_dev->snaps);
	init_rwsem(&rbd_dev->header_rwsem);

	rbd_dev->spec = spec;
	rbd_dev->rbd_client = rbdc;

	/* Initialize the layout used for all rbd requests */

	rbd_dev->layout.fl_stripe_unit = cpu_to_le32(1 << RBD_MAX_OBJ_ORDER);
	rbd_dev->layout.fl_stripe_count = cpu_to_le32(1);
	rbd_dev->layout.fl_object_size = cpu_to_le32(1 << RBD_MAX_OBJ_ORDER);
	rbd_dev->layout.fl_pg_pool = cpu_to_le32((u32) spec->pool_id);

	return rbd_dev;
}

static void rbd_dev_destroy(struct rbd_device *rbd_dev)
{
	rbd_spec_put(rbd_dev->parent_spec);
	kfree(rbd_dev->header_name);
	rbd_put_client(rbd_dev->rbd_client);
	rbd_spec_put(rbd_dev->spec);
	kfree(rbd_dev);
}

static void rbd_snap_destroy(struct rbd_snap *snap)
{
	kfree(snap->name);
	kfree(snap);
}

static struct rbd_snap *rbd_snap_create(struct rbd_device *rbd_dev,
						const char *snap_name,
						u64 snap_id, u64 snap_size,
						u64 snap_features)
{
	struct rbd_snap *snap;

	snap = kzalloc(sizeof (*snap), GFP_KERNEL);
	if (!snap)
		return ERR_PTR(-ENOMEM);

	snap->name = snap_name;
	snap->id = snap_id;
	snap->size = snap_size;
	snap->features = snap_features;

	return snap;
}

/*
 * Returns a dynamically-allocated snapshot name if successful, or a
 * pointer-coded error otherwise.
 */
static char *rbd_dev_v1_snap_info(struct rbd_device *rbd_dev, u32 which,
		u64 *snap_size, u64 *snap_features)
{
	char *snap_name;
	int i;

	rbd_assert(which < rbd_dev->header.snapc->num_snaps);

	/* Skip over names until we find the one we are looking for */

	snap_name = rbd_dev->header.snap_names;
	for (i = 0; i < which; i++)
		snap_name += strlen(snap_name) + 1;

	snap_name = kstrdup(snap_name, GFP_KERNEL);
	if (!snap_name)
		return ERR_PTR(-ENOMEM);

	*snap_size = rbd_dev->header.snap_sizes[which];
	*snap_features = 0;	/* No features for v1 */

	return snap_name;
}

/*
 * Get the size and object order for an image snapshot, or if
 * snap_id is CEPH_NOSNAP, gets this information for the base
 * image.
 */
static int _rbd_dev_v2_snap_size(struct rbd_device *rbd_dev, u64 snap_id,
				u8 *order, u64 *snap_size)
{
	__le64 snapid = cpu_to_le64(snap_id);
	int ret;
	struct {
		u8 order;
		__le64 size;
	} __attribute__ ((packed)) size_buf = { 0 };

	ret = rbd_obj_method_sync(rbd_dev, rbd_dev->header_name,
				"rbd", "get_size",
				&snapid, sizeof (snapid),
				&size_buf, sizeof (size_buf), NULL);
	dout("%s: rbd_obj_method_sync returned %d\n", __func__, ret);
	if (ret < 0)
		return ret;
	if (ret < sizeof (size_buf))
		return -ERANGE;

	if (order)
		*order = size_buf.order;
	*snap_size = le64_to_cpu(size_buf.size);

	dout("  snap_id 0x%016llx order = %u, snap_size = %llu\n",
		(unsigned long long)snap_id, (unsigned int)*order,
		(unsigned long long)*snap_size);

	return 0;
}

static int rbd_dev_v2_image_size(struct rbd_device *rbd_dev)
{
	return _rbd_dev_v2_snap_size(rbd_dev, CEPH_NOSNAP,
					&rbd_dev->header.obj_order,
					&rbd_dev->header.image_size);
}

static int rbd_dev_v2_object_prefix(struct rbd_device *rbd_dev)
{
	void *reply_buf;
	int ret;
	void *p;

	reply_buf = kzalloc(RBD_OBJ_PREFIX_LEN_MAX, GFP_KERNEL);
	if (!reply_buf)
		return -ENOMEM;

	ret = rbd_obj_method_sync(rbd_dev, rbd_dev->header_name,
				"rbd", "get_object_prefix", NULL, 0,
				reply_buf, RBD_OBJ_PREFIX_LEN_MAX, NULL);
	dout("%s: rbd_obj_method_sync returned %d\n", __func__, ret);
	if (ret < 0)
		goto out;

	p = reply_buf;
	rbd_dev->header.object_prefix = ceph_extract_encoded_string(&p,
						p + ret, NULL, GFP_NOIO);
	ret = 0;

	if (IS_ERR(rbd_dev->header.object_prefix)) {
		ret = PTR_ERR(rbd_dev->header.object_prefix);
		rbd_dev->header.object_prefix = NULL;
	} else {
		dout("  object_prefix = %s\n", rbd_dev->header.object_prefix);
	}
out:
	kfree(reply_buf);

	return ret;
}

static int _rbd_dev_v2_snap_features(struct rbd_device *rbd_dev, u64 snap_id,
		u64 *snap_features)
{
	__le64 snapid = cpu_to_le64(snap_id);
	struct {
		__le64 features;
		__le64 incompat;
	} __attribute__ ((packed)) features_buf = { 0 };
	u64 incompat;
	int ret;

	ret = rbd_obj_method_sync(rbd_dev, rbd_dev->header_name,
				"rbd", "get_features",
				&snapid, sizeof (snapid),
				&features_buf, sizeof (features_buf), NULL);
	dout("%s: rbd_obj_method_sync returned %d\n", __func__, ret);
	if (ret < 0)
		return ret;
	if (ret < sizeof (features_buf))
		return -ERANGE;

	incompat = le64_to_cpu(features_buf.incompat);
	if (incompat & ~RBD_FEATURES_SUPPORTED)
		return -ENXIO;

	*snap_features = le64_to_cpu(features_buf.features);

	dout("  snap_id 0x%016llx features = 0x%016llx incompat = 0x%016llx\n",
		(unsigned long long)snap_id,
		(unsigned long long)*snap_features,
		(unsigned long long)le64_to_cpu(features_buf.incompat));

	return 0;
}

static int rbd_dev_v2_features(struct rbd_device *rbd_dev)
{
	return _rbd_dev_v2_snap_features(rbd_dev, CEPH_NOSNAP,
						&rbd_dev->header.features);
}

static int rbd_dev_v2_parent_info(struct rbd_device *rbd_dev)
{
	struct rbd_spec *parent_spec;
	size_t size;
	void *reply_buf = NULL;
	__le64 snapid;
	void *p;
	void *end;
	char *image_id;
	u64 overlap;
	int ret;

	parent_spec = rbd_spec_alloc();
	if (!parent_spec)
		return -ENOMEM;

	size = sizeof (__le64) +				/* pool_id */
		sizeof (__le32) + RBD_IMAGE_ID_LEN_MAX +	/* image_id */
		sizeof (__le64) +				/* snap_id */
		sizeof (__le64);				/* overlap */
	reply_buf = kmalloc(size, GFP_KERNEL);
	if (!reply_buf) {
		ret = -ENOMEM;
		goto out_err;
	}

	snapid = cpu_to_le64(CEPH_NOSNAP);
	ret = rbd_obj_method_sync(rbd_dev, rbd_dev->header_name,
				"rbd", "get_parent",
				&snapid, sizeof (snapid),
				reply_buf, size, NULL);
	dout("%s: rbd_obj_method_sync returned %d\n", __func__, ret);
	if (ret < 0)
		goto out_err;

	p = reply_buf;
	end = reply_buf + ret;
	ret = -ERANGE;
	ceph_decode_64_safe(&p, end, parent_spec->pool_id, out_err);
	if (parent_spec->pool_id == CEPH_NOPOOL)
		goto out;	/* No parent?  No problem. */

	/* The ceph file layout needs to fit pool id in 32 bits */

	ret = -EIO;
	if (parent_spec->pool_id > (u64)U32_MAX) {
		rbd_warn(NULL, "parent pool id too large (%llu > %u)\n",
			(unsigned long long)parent_spec->pool_id, U32_MAX);
		goto out_err;
	}

	image_id = ceph_extract_encoded_string(&p, end, NULL, GFP_KERNEL);
	if (IS_ERR(image_id)) {
		ret = PTR_ERR(image_id);
		goto out_err;
	}
	parent_spec->image_id = image_id;
	ceph_decode_64_safe(&p, end, parent_spec->snap_id, out_err);
	ceph_decode_64_safe(&p, end, overlap, out_err);

	rbd_dev->parent_overlap = overlap;
	rbd_dev->parent_spec = parent_spec;
	parent_spec = NULL;	/* rbd_dev now owns this */
out:
	ret = 0;
out_err:
	kfree(reply_buf);
	rbd_spec_put(parent_spec);

	return ret;
}

static int rbd_dev_v2_striping_info(struct rbd_device *rbd_dev)
{
	struct {
		__le64 stripe_unit;
		__le64 stripe_count;
	} __attribute__ ((packed)) striping_info_buf = { 0 };
	size_t size = sizeof (striping_info_buf);
	void *p;
	u64 obj_size;
	u64 stripe_unit;
	u64 stripe_count;
	int ret;

	ret = rbd_obj_method_sync(rbd_dev, rbd_dev->header_name,
				"rbd", "get_stripe_unit_count", NULL, 0,
				(char *)&striping_info_buf, size, NULL);
	dout("%s: rbd_obj_method_sync returned %d\n", __func__, ret);
	if (ret < 0)
		return ret;
	if (ret < size)
		return -ERANGE;

	/*
	 * We don't actually support the "fancy striping" feature
	 * (STRIPINGV2) yet, but if the striping sizes are the
	 * defaults the behavior is the same as before.  So find
	 * out, and only fail if the image has non-default values.
	 */
	ret = -EINVAL;
	obj_size = (u64)1 << rbd_dev->header.obj_order;
	p = &striping_info_buf;
	stripe_unit = ceph_decode_64(&p);
	if (stripe_unit != obj_size) {
		rbd_warn(rbd_dev, "unsupported stripe unit "
				"(got %llu want %llu)",
				stripe_unit, obj_size);
		return -EINVAL;
	}
	stripe_count = ceph_decode_64(&p);
	if (stripe_count != 1) {
		rbd_warn(rbd_dev, "unsupported stripe count "
				"(got %llu want 1)", stripe_count);
		return -EINVAL;
	}
	rbd_dev->header.stripe_unit = stripe_unit;
	rbd_dev->header.stripe_count = stripe_count;

	return 0;
}

static char *rbd_dev_image_name(struct rbd_device *rbd_dev)
{
	size_t image_id_size;
	char *image_id;
	void *p;
	void *end;
	size_t size;
	void *reply_buf = NULL;
	size_t len = 0;
	char *image_name = NULL;
	int ret;

	rbd_assert(!rbd_dev->spec->image_name);

	len = strlen(rbd_dev->spec->image_id);
	image_id_size = sizeof (__le32) + len;
	image_id = kmalloc(image_id_size, GFP_KERNEL);
	if (!image_id)
		return NULL;

	p = image_id;
	end = image_id + image_id_size;
	ceph_encode_string(&p, end, rbd_dev->spec->image_id, (u32)len);

	size = sizeof (__le32) + RBD_IMAGE_NAME_LEN_MAX;
	reply_buf = kmalloc(size, GFP_KERNEL);
	if (!reply_buf)
		goto out;

	ret = rbd_obj_method_sync(rbd_dev, RBD_DIRECTORY,
				"rbd", "dir_get_name",
				image_id, image_id_size,
				reply_buf, size, NULL);
	if (ret < 0)
		goto out;
	p = reply_buf;
	end = reply_buf + ret;

	image_name = ceph_extract_encoded_string(&p, end, &len, GFP_KERNEL);
	if (IS_ERR(image_name))
		image_name = NULL;
	else
		dout("%s: name is %s len is %zd\n", __func__, image_name, len);
out:
	kfree(reply_buf);
	kfree(image_id);

	return image_name;
}

/*
 * When a parent image gets probed, we only have the pool, image,
 * and snapshot ids but not the names of any of them.  This call
 * is made later to fill in those names.  It has to be done after
 * rbd_dev_snaps_update() has completed because some of the
 * information (in particular, snapshot name) is not available
 * until then.
 *
 * When an image being mapped (not a parent) is probed, we have the
 * pool name and pool id, image name and image id, and the snapshot
 * name.  The only thing we're missing is the snapshot id.
 */
static int rbd_dev_probe_update_spec(struct rbd_device *rbd_dev)
{
	struct ceph_osd_client *osdc;
	const char *name;
	void *reply_buf = NULL;
	int ret;

	/*
	 * An image being mapped will have the pool name (etc.), but
	 * we need to look up the snapshot id.
	 */
	if (rbd_dev->spec->pool_name) {
		if (strcmp(rbd_dev->spec->snap_name, RBD_SNAP_HEAD_NAME)) {
			struct rbd_snap *snap;

			snap = snap_by_name(rbd_dev, rbd_dev->spec->snap_name);
			if (!snap)
				return -ENOENT;
			rbd_dev->spec->snap_id = snap->id;
		} else {
			rbd_dev->spec->snap_id = CEPH_NOSNAP;
		}

		return 0;
	}

	/* Look up the pool name */

	osdc = &rbd_dev->rbd_client->client->osdc;
	name = ceph_pg_pool_name_by_id(osdc->osdmap, rbd_dev->spec->pool_id);
	if (!name) {
		rbd_warn(rbd_dev, "there is no pool with id %llu",
			rbd_dev->spec->pool_id);	/* Really a BUG() */
		return -EIO;
	}

	rbd_dev->spec->pool_name = kstrdup(name, GFP_KERNEL);
	if (!rbd_dev->spec->pool_name)
		return -ENOMEM;

	/* Fetch the image name; tolerate failure here */

	name = rbd_dev_image_name(rbd_dev);
	if (name)
		rbd_dev->spec->image_name = (char *)name;
	else
		rbd_warn(rbd_dev, "unable to get image name");

	/* Look up the snapshot name. */

	name = rbd_snap_name(rbd_dev, rbd_dev->spec->snap_id);
	if (!name) {
		rbd_warn(rbd_dev, "no snapshot with id %llu",
			rbd_dev->spec->snap_id);	/* Really a BUG() */
		ret = -EIO;
		goto out_err;
	}
	rbd_dev->spec->snap_name = kstrdup(name, GFP_KERNEL);
	if(!rbd_dev->spec->snap_name)
		goto out_err;

	return 0;
out_err:
	kfree(reply_buf);
	kfree(rbd_dev->spec->pool_name);
	rbd_dev->spec->pool_name = NULL;

	return ret;
}

static int rbd_dev_v2_snap_context(struct rbd_device *rbd_dev, u64 *ver)
{
	size_t size;
	int ret;
	void *reply_buf;
	void *p;
	void *end;
	u64 seq;
	u32 snap_count;
	struct ceph_snap_context *snapc;
	u32 i;

	/*
	 * We'll need room for the seq value (maximum snapshot id),
	 * snapshot count, and array of that many snapshot ids.
	 * For now we have a fixed upper limit on the number we're
	 * prepared to receive.
	 */
	size = sizeof (__le64) + sizeof (__le32) +
			RBD_MAX_SNAP_COUNT * sizeof (__le64);
	reply_buf = kzalloc(size, GFP_KERNEL);
	if (!reply_buf)
		return -ENOMEM;

	ret = rbd_obj_method_sync(rbd_dev, rbd_dev->header_name,
				"rbd", "get_snapcontext", NULL, 0,
				reply_buf, size, ver);
	dout("%s: rbd_obj_method_sync returned %d\n", __func__, ret);
	if (ret < 0)
		goto out;

	p = reply_buf;
	end = reply_buf + ret;
	ret = -ERANGE;
	ceph_decode_64_safe(&p, end, seq, out);
	ceph_decode_32_safe(&p, end, snap_count, out);

	/*
	 * Make sure the reported number of snapshot ids wouldn't go
	 * beyond the end of our buffer.  But before checking that,
	 * make sure the computed size of the snapshot context we
	 * allocate is representable in a size_t.
	 */
	if (snap_count > (SIZE_MAX - sizeof (struct ceph_snap_context))
				 / sizeof (u64)) {
		ret = -EINVAL;
		goto out;
	}
	if (!ceph_has_room(&p, end, snap_count * sizeof (__le64)))
		goto out;

	size = sizeof (struct ceph_snap_context) +
				snap_count * sizeof (snapc->snaps[0]);
	snapc = kmalloc(size, GFP_KERNEL);
	if (!snapc) {
		ret = -ENOMEM;
		goto out;
	}
	ret = 0;

	atomic_set(&snapc->nref, 1);
	snapc->seq = seq;
	snapc->num_snaps = snap_count;
	for (i = 0; i < snap_count; i++)
		snapc->snaps[i] = ceph_decode_64(&p);

	rbd_dev->header.snapc = snapc;

	dout("  snap context seq = %llu, snap_count = %u\n",
		(unsigned long long)seq, (unsigned int)snap_count);
out:
	kfree(reply_buf);

	return ret;
}

static char *rbd_dev_v2_snap_name(struct rbd_device *rbd_dev, u32 which)
{
	size_t size;
	void *reply_buf;
	__le64 snap_id;
	int ret;
	void *p;
	void *end;
	char *snap_name;

	size = sizeof (__le32) + RBD_MAX_SNAP_NAME_LEN;
	reply_buf = kmalloc(size, GFP_KERNEL);
	if (!reply_buf)
		return ERR_PTR(-ENOMEM);

	rbd_assert(which < rbd_dev->header.snapc->num_snaps);
	snap_id = cpu_to_le64(rbd_dev->header.snapc->snaps[which]);
	ret = rbd_obj_method_sync(rbd_dev, rbd_dev->header_name,
				"rbd", "get_snapshot_name",
				&snap_id, sizeof (snap_id),
				reply_buf, size, NULL);
	dout("%s: rbd_obj_method_sync returned %d\n", __func__, ret);
	if (ret < 0) {
		snap_name = ERR_PTR(ret);
		goto out;
	}

	p = reply_buf;
	end = reply_buf + ret;
	snap_name = ceph_extract_encoded_string(&p, end, NULL, GFP_KERNEL);
	if (IS_ERR(snap_name))
		goto out;

	dout("  snap_id 0x%016llx snap_name = %s\n",
		(unsigned long long)le64_to_cpu(snap_id), snap_name);
out:
	kfree(reply_buf);

	return snap_name;
}

static char *rbd_dev_v2_snap_info(struct rbd_device *rbd_dev, u32 which,
		u64 *snap_size, u64 *snap_features)
{
	u64 snap_id;
	u64 size;
	u64 features;
	char *snap_name;
	int ret;

	rbd_assert(which < rbd_dev->header.snapc->num_snaps);
	snap_id = rbd_dev->header.snapc->snaps[which];
	ret = _rbd_dev_v2_snap_size(rbd_dev, snap_id, NULL, &size);
	if (ret)
		goto out_err;

	ret = _rbd_dev_v2_snap_features(rbd_dev, snap_id, &features);
	if (ret)
		goto out_err;

	snap_name = rbd_dev_v2_snap_name(rbd_dev, which);
	if (!IS_ERR(snap_name)) {
		*snap_size = size;
		*snap_features = features;
	}

	return snap_name;
out_err:
	return ERR_PTR(ret);
}

static char *rbd_dev_snap_info(struct rbd_device *rbd_dev, u32 which,
		u64 *snap_size, u64 *snap_features)
{
	if (rbd_dev->image_format == 1)
		return rbd_dev_v1_snap_info(rbd_dev, which,
					snap_size, snap_features);
	if (rbd_dev->image_format == 2)
		return rbd_dev_v2_snap_info(rbd_dev, which,
					snap_size, snap_features);
	return ERR_PTR(-EINVAL);
}

static int rbd_dev_v2_refresh(struct rbd_device *rbd_dev, u64 *hver)
{
	int ret;
	__u8 obj_order;

	down_write(&rbd_dev->header_rwsem);

	/* Grab old order first, to see if it changes */

	obj_order = rbd_dev->header.obj_order,
	ret = rbd_dev_v2_image_size(rbd_dev);
	if (ret)
		goto out;
	if (rbd_dev->header.obj_order != obj_order) {
		ret = -EIO;
		goto out;
	}
	rbd_update_mapping_size(rbd_dev);

	ret = rbd_dev_v2_snap_context(rbd_dev, hver);
	dout("rbd_dev_v2_snap_context returned %d\n", ret);
	if (ret)
		goto out;
	ret = rbd_dev_snaps_update(rbd_dev);
	dout("rbd_dev_snaps_update returned %d\n", ret);
	if (ret)
		goto out;
out:
	up_write(&rbd_dev->header_rwsem);

	return ret;
}

/*
 * Scan the rbd device's current snapshot list and compare it to the
 * newly-received snapshot context.  Remove any existing snapshots
 * not present in the new snapshot context.  Add a new snapshot for
 * any snaphots in the snapshot context not in the current list.
 * And verify there are no changes to snapshots we already know
 * about.
 *
 * Assumes the snapshots in the snapshot context are sorted by
 * snapshot id, highest id first.  (Snapshots in the rbd_dev's list
 * are also maintained in that order.)
 *
 * Note that any error occurs while updating the snapshot list
 * aborts the update, and the entire list is cleared.  The snapshot
 * list becomes inconsistent at that point anyway, so it might as
 * well be empty.
 */
static int rbd_dev_snaps_update(struct rbd_device *rbd_dev)
{
	struct ceph_snap_context *snapc = rbd_dev->header.snapc;
	const u32 snap_count = snapc->num_snaps;
	struct list_head *head = &rbd_dev->snaps;
	struct list_head *links = head->next;
	u32 index = 0;
	int ret = 0;

	dout("%s: snap count is %u\n", __func__, (unsigned int)snap_count);
	while (index < snap_count || links != head) {
		u64 snap_id;
		struct rbd_snap *snap;
		char *snap_name;
		u64 snap_size = 0;
		u64 snap_features = 0;

		snap_id = index < snap_count ? snapc->snaps[index]
					     : CEPH_NOSNAP;
		snap = links != head ? list_entry(links, struct rbd_snap, node)
				     : NULL;
		rbd_assert(!snap || snap->id != CEPH_NOSNAP);

		if (snap_id == CEPH_NOSNAP || (snap && snap->id > snap_id)) {
			struct list_head *next = links->next;

			/*
			 * A previously-existing snapshot is not in
			 * the new snap context.
			 *
			 * If the now-missing snapshot is the one
			 * the image represents, clear its existence
			 * flag so we can avoid sending any more
			 * requests to it.
			 */
			if (rbd_dev->spec->snap_id == snap->id)
				clear_bit(RBD_DEV_FLAG_EXISTS, &rbd_dev->flags);
			dout("removing %ssnap id %llu\n",
				rbd_dev->spec->snap_id == snap->id ?
							"mapped " : "",
				(unsigned long long)snap->id);

			list_del(&snap->node);
			rbd_snap_destroy(snap);

			/* Done with this list entry; advance */

			links = next;
			continue;
		}

		snap_name = rbd_dev_snap_info(rbd_dev, index,
					&snap_size, &snap_features);
		if (IS_ERR(snap_name)) {
			ret = PTR_ERR(snap_name);
			dout("failed to get snap info, error %d\n", ret);
			goto out_err;
		}

		dout("entry %u: snap_id = %llu\n", (unsigned int)snap_count,
			(unsigned long long)snap_id);
		if (!snap || (snap_id != CEPH_NOSNAP && snap->id < snap_id)) {
			struct rbd_snap *new_snap;

			/* We haven't seen this snapshot before */

			new_snap = rbd_snap_create(rbd_dev, snap_name,
					snap_id, snap_size, snap_features);
			if (IS_ERR(new_snap)) {
				ret = PTR_ERR(new_snap);
				dout("  failed to add dev, error %d\n", ret);
				goto out_err;
			}

			/* New goes before existing, or at end of list */

			dout("  added dev%s\n", snap ? "" : " at end\n");
			if (snap)
				list_add_tail(&new_snap->node, &snap->node);
			else
				list_add_tail(&new_snap->node, head);
		} else {
			/* Already have this one */

			dout("  already present\n");

			rbd_assert(snap->size == snap_size);
			rbd_assert(!strcmp(snap->name, snap_name));
			rbd_assert(snap->features == snap_features);

			/* Done with this list entry; advance */

			links = links->next;
		}

		/* Advance to the next entry in the snapshot context */

		index++;
	}
	dout("%s: done\n", __func__);

	return 0;
out_err:
	rbd_remove_all_snaps(rbd_dev);

	return ret;
}

static int rbd_bus_add_dev(struct rbd_device *rbd_dev)
{
	struct device *dev;
	int ret;

	mutex_lock_nested(&ctl_mutex, SINGLE_DEPTH_NESTING);

	dev = &rbd_dev->dev;
	dev->bus = &rbd_bus_type;
	dev->type = &rbd_device_type;
	dev->parent = &rbd_root_dev;
	dev->release = rbd_dev_release;
	dev_set_name(dev, "%d", rbd_dev->dev_id);
	ret = device_register(dev);

	mutex_unlock(&ctl_mutex);

	return ret;
}

static void rbd_bus_del_dev(struct rbd_device *rbd_dev)
{
	device_unregister(&rbd_dev->dev);
}

static atomic64_t rbd_dev_id_max = ATOMIC64_INIT(0);

/*
 * Get a unique rbd identifier for the given new rbd_dev, and add
 * the rbd_dev to the global list.  The minimum rbd id is 1.
 */
static void rbd_dev_id_get(struct rbd_device *rbd_dev)
{
	rbd_dev->dev_id = atomic64_inc_return(&rbd_dev_id_max);

	spin_lock(&rbd_dev_list_lock);
	list_add_tail(&rbd_dev->node, &rbd_dev_list);
	spin_unlock(&rbd_dev_list_lock);
	dout("rbd_dev %p given dev id %llu\n", rbd_dev,
		(unsigned long long) rbd_dev->dev_id);
}

/*
 * Remove an rbd_dev from the global list, and record that its
 * identifier is no longer in use.
 */
static void rbd_dev_id_put(struct rbd_device *rbd_dev)
{
	struct list_head *tmp;
	int rbd_id = rbd_dev->dev_id;
	int max_id;

	rbd_assert(rbd_id > 0);

	dout("rbd_dev %p released dev id %llu\n", rbd_dev,
		(unsigned long long) rbd_dev->dev_id);
	spin_lock(&rbd_dev_list_lock);
	list_del_init(&rbd_dev->node);

	/*
	 * If the id being "put" is not the current maximum, there
	 * is nothing special we need to do.
	 */
	if (rbd_id != atomic64_read(&rbd_dev_id_max)) {
		spin_unlock(&rbd_dev_list_lock);
		return;
	}

	/*
	 * We need to update the current maximum id.  Search the
	 * list to find out what it is.  We're more likely to find
	 * the maximum at the end, so search the list backward.
	 */
	max_id = 0;
	list_for_each_prev(tmp, &rbd_dev_list) {
		struct rbd_device *rbd_dev;

		rbd_dev = list_entry(tmp, struct rbd_device, node);
		if (rbd_dev->dev_id > max_id)
			max_id = rbd_dev->dev_id;
	}
	spin_unlock(&rbd_dev_list_lock);

	/*
	 * The max id could have been updated by rbd_dev_id_get(), in
	 * which case it now accurately reflects the new maximum.
	 * Be careful not to overwrite the maximum value in that
	 * case.
	 */
	atomic64_cmpxchg(&rbd_dev_id_max, rbd_id, max_id);
	dout("  max dev id has been reset\n");
}

/*
 * Skips over white space at *buf, and updates *buf to point to the
 * first found non-space character (if any). Returns the length of
 * the token (string of non-white space characters) found.  Note
 * that *buf must be terminated with '\0'.
 */
static inline size_t next_token(const char **buf)
{
        /*
        * These are the characters that produce nonzero for
        * isspace() in the "C" and "POSIX" locales.
        */
        const char *spaces = " \f\n\r\t\v";

        *buf += strspn(*buf, spaces);	/* Find start of token */

	return strcspn(*buf, spaces);   /* Return token length */
}

/*
 * Finds the next token in *buf, and if the provided token buffer is
 * big enough, copies the found token into it.  The result, if
 * copied, is guaranteed to be terminated with '\0'.  Note that *buf
 * must be terminated with '\0' on entry.
 *
 * Returns the length of the token found (not including the '\0').
 * Return value will be 0 if no token is found, and it will be >=
 * token_size if the token would not fit.
 *
 * The *buf pointer will be updated to point beyond the end of the
 * found token.  Note that this occurs even if the token buffer is
 * too small to hold it.
 */
static inline size_t copy_token(const char **buf,
				char *token,
				size_t token_size)
{
        size_t len;

	len = next_token(buf);
	if (len < token_size) {
		memcpy(token, *buf, len);
		*(token + len) = '\0';
	}
	*buf += len;

        return len;
}

/*
 * Finds the next token in *buf, dynamically allocates a buffer big
 * enough to hold a copy of it, and copies the token into the new
 * buffer.  The copy is guaranteed to be terminated with '\0'.  Note
 * that a duplicate buffer is created even for a zero-length token.
 *
 * Returns a pointer to the newly-allocated duplicate, or a null
 * pointer if memory for the duplicate was not available.  If
 * the lenp argument is a non-null pointer, the length of the token
 * (not including the '\0') is returned in *lenp.
 *
 * If successful, the *buf pointer will be updated to point beyond
 * the end of the found token.
 *
 * Note: uses GFP_KERNEL for allocation.
 */
static inline char *dup_token(const char **buf, size_t *lenp)
{
	char *dup;
	size_t len;

	len = next_token(buf);
	dup = kmemdup(*buf, len + 1, GFP_KERNEL);
	if (!dup)
		return NULL;
	*(dup + len) = '\0';
	*buf += len;

	if (lenp)
		*lenp = len;

	return dup;
}

/*
 * Parse the options provided for an "rbd add" (i.e., rbd image
 * mapping) request.  These arrive via a write to /sys/bus/rbd/add,
 * and the data written is passed here via a NUL-terminated buffer.
 * Returns 0 if successful or an error code otherwise.
 *
 * The information extracted from these options is recorded in
 * the other parameters which return dynamically-allocated
 * structures:
 *  ceph_opts
 *      The address of a pointer that will refer to a ceph options
 *      structure.  Caller must release the returned pointer using
 *      ceph_destroy_options() when it is no longer needed.
 *  rbd_opts
 *	Address of an rbd options pointer.  Fully initialized by
 *	this function; caller must release with kfree().
 *  spec
 *	Address of an rbd image specification pointer.  Fully
 *	initialized by this function based on parsed options.
 *	Caller must release with rbd_spec_put().
 *
 * The options passed take this form:
 *  <mon_addrs> <options> <pool_name> <image_name> [<snap_id>]
 * where:
 *  <mon_addrs>
 *      A comma-separated list of one or more monitor addresses.
 *      A monitor address is an ip address, optionally followed
 *      by a port number (separated by a colon).
 *        I.e.:  ip1[:port1][,ip2[:port2]...]
 *  <options>
 *      A comma-separated list of ceph and/or rbd options.
 *  <pool_name>
 *      The name of the rados pool containing the rbd image.
 *  <image_name>
 *      The name of the image in that pool to map.
 *  <snap_id>
 *      An optional snapshot id.  If provided, the mapping will
 *      present data from the image at the time that snapshot was
 *      created.  The image head is used if no snapshot id is
 *      provided.  Snapshot mappings are always read-only.
 */
static int rbd_add_parse_args(const char *buf,
				struct ceph_options **ceph_opts,
				struct rbd_options **opts,
				struct rbd_spec **rbd_spec)
{
	size_t len;
	char *options;
	const char *mon_addrs;
	char *snap_name;
	size_t mon_addrs_size;
	struct rbd_spec *spec = NULL;
	struct rbd_options *rbd_opts = NULL;
	struct ceph_options *copts;
	int ret;

	/* The first four tokens are required */

	len = next_token(&buf);
	if (!len) {
		rbd_warn(NULL, "no monitor address(es) provided");
		return -EINVAL;
	}
	mon_addrs = buf;
	mon_addrs_size = len + 1;
	buf += len;

	ret = -EINVAL;
	options = dup_token(&buf, NULL);
	if (!options)
		return -ENOMEM;
	if (!*options) {
		rbd_warn(NULL, "no options provided");
		goto out_err;
	}

	spec = rbd_spec_alloc();
	if (!spec)
		goto out_mem;

	spec->pool_name = dup_token(&buf, NULL);
	if (!spec->pool_name)
		goto out_mem;
	if (!*spec->pool_name) {
		rbd_warn(NULL, "no pool name provided");
		goto out_err;
	}

	spec->image_name = dup_token(&buf, NULL);
	if (!spec->image_name)
		goto out_mem;
	if (!*spec->image_name) {
		rbd_warn(NULL, "no image name provided");
		goto out_err;
	}

	/*
	 * Snapshot name is optional; default is to use "-"
	 * (indicating the head/no snapshot).
	 */
	len = next_token(&buf);
	if (!len) {
		buf = RBD_SNAP_HEAD_NAME; /* No snapshot supplied */
		len = sizeof (RBD_SNAP_HEAD_NAME) - 1;
	} else if (len > RBD_MAX_SNAP_NAME_LEN) {
		ret = -ENAMETOOLONG;
		goto out_err;
	}
	snap_name = kmemdup(buf, len + 1, GFP_KERNEL);
	if (!snap_name)
		goto out_mem;
	*(snap_name + len) = '\0';
	spec->snap_name = snap_name;

	/* Initialize all rbd options to the defaults */

	rbd_opts = kzalloc(sizeof (*rbd_opts), GFP_KERNEL);
	if (!rbd_opts)
		goto out_mem;

	rbd_opts->read_only = RBD_READ_ONLY_DEFAULT;

	copts = ceph_parse_options(options, mon_addrs,
					mon_addrs + mon_addrs_size - 1,
					parse_rbd_opts_token, rbd_opts);
	if (IS_ERR(copts)) {
		ret = PTR_ERR(copts);
		goto out_err;
	}
	kfree(options);

	*ceph_opts = copts;
	*opts = rbd_opts;
	*rbd_spec = spec;

	return 0;
out_mem:
	ret = -ENOMEM;
out_err:
	kfree(rbd_opts);
	rbd_spec_put(spec);
	kfree(options);

	return ret;
}

/*
 * An rbd format 2 image has a unique identifier, distinct from the
 * name given to it by the user.  Internally, that identifier is
 * what's used to specify the names of objects related to the image.
 *
 * A special "rbd id" object is used to map an rbd image name to its
 * id.  If that object doesn't exist, then there is no v2 rbd image
 * with the supplied name.
 *
 * This function will record the given rbd_dev's image_id field if
 * it can be determined, and in that case will return 0.  If any
 * errors occur a negative errno will be returned and the rbd_dev's
 * image_id field will be unchanged (and should be NULL).
 */
static int rbd_dev_image_id(struct rbd_device *rbd_dev)
{
	int ret;
	size_t size;
	char *object_name;
	void *response;
	char *image_id;

	/*
	 * When probing a parent image, the image id is already
	 * known (and the image name likely is not).  There's no
	 * need to fetch the image id again in this case.  We
	 * do still need to set the image format though.
	 */
	if (rbd_dev->spec->image_id) {
		rbd_dev->image_format = *rbd_dev->spec->image_id ? 2 : 1;

		return 0;
	}

	/*
	 * First, see if the format 2 image id file exists, and if
	 * so, get the image's persistent id from it.
	 */
	size = sizeof (RBD_ID_PREFIX) + strlen(rbd_dev->spec->image_name);
	object_name = kmalloc(size, GFP_NOIO);
	if (!object_name)
		return -ENOMEM;
	sprintf(object_name, "%s%s", RBD_ID_PREFIX, rbd_dev->spec->image_name);
	dout("rbd id object name is %s\n", object_name);

	/* Response will be an encoded string, which includes a length */

	size = sizeof (__le32) + RBD_IMAGE_ID_LEN_MAX;
	response = kzalloc(size, GFP_NOIO);
	if (!response) {
		ret = -ENOMEM;
		goto out;
	}

	/* If it doesn't exist we'll assume it's a format 1 image */

	ret = rbd_obj_method_sync(rbd_dev, object_name,
				"rbd", "get_id", NULL, 0,
				response, RBD_IMAGE_ID_LEN_MAX, NULL);
	dout("%s: rbd_obj_method_sync returned %d\n", __func__, ret);
	if (ret == -ENOENT) {
		image_id = kstrdup("", GFP_KERNEL);
		ret = image_id ? 0 : -ENOMEM;
		if (!ret)
			rbd_dev->image_format = 1;
	} else if (ret > sizeof (__le32)) {
		void *p = response;

		image_id = ceph_extract_encoded_string(&p, p + ret,
						NULL, GFP_NOIO);
		ret = IS_ERR(image_id) ? PTR_ERR(image_id) : 0;
		if (!ret)
			rbd_dev->image_format = 2;
	} else {
		ret = -EINVAL;
	}

	if (!ret) {
		rbd_dev->spec->image_id = image_id;
		dout("image_id is %s\n", image_id);
	}
out:
	kfree(response);
	kfree(object_name);

	return ret;
}

static int rbd_dev_v1_probe(struct rbd_device *rbd_dev)
{
	int ret;
	size_t size;

	/* Record the header object name for this rbd image. */

	size = strlen(rbd_dev->spec->image_name) + sizeof (RBD_SUFFIX);
	rbd_dev->header_name = kmalloc(size, GFP_KERNEL);
	if (!rbd_dev->header_name) {
		ret = -ENOMEM;
		goto out_err;
	}
	sprintf(rbd_dev->header_name, "%s%s",
		rbd_dev->spec->image_name, RBD_SUFFIX);

	/* Populate rbd image metadata */

	ret = rbd_read_header(rbd_dev, &rbd_dev->header);
	if (ret < 0)
		goto out_err;

	/* Version 1 images have no parent (no layering) */

	rbd_dev->parent_spec = NULL;
	rbd_dev->parent_overlap = 0;

	dout("discovered version 1 image, header name is %s\n",
		rbd_dev->header_name);

	return 0;

out_err:
	kfree(rbd_dev->header_name);
	rbd_dev->header_name = NULL;
	kfree(rbd_dev->spec->image_id);
	rbd_dev->spec->image_id = NULL;

	return ret;
}

static int rbd_dev_v2_probe(struct rbd_device *rbd_dev)
{
	size_t size;
	int ret;
	u64 ver = 0;

	/*
	 * Image id was filled in by the caller.  Record the header
	 * object name for this rbd image.
	 */
	size = sizeof (RBD_HEADER_PREFIX) + strlen(rbd_dev->spec->image_id);
	rbd_dev->header_name = kmalloc(size, GFP_KERNEL);
	if (!rbd_dev->header_name)
		return -ENOMEM;
	sprintf(rbd_dev->header_name, "%s%s",
			RBD_HEADER_PREFIX, rbd_dev->spec->image_id);

	/* Get the size and object order for the image */
	ret = rbd_dev_v2_image_size(rbd_dev);
	if (ret)
		goto out_err;

	/* Get the object prefix (a.k.a. block_name) for the image */

	ret = rbd_dev_v2_object_prefix(rbd_dev);
	if (ret)
		goto out_err;

	/* Get the and check features for the image */

	ret = rbd_dev_v2_features(rbd_dev);
	if (ret)
		goto out_err;

	/* If the image supports layering, get the parent info */

	if (rbd_dev->header.features & RBD_FEATURE_LAYERING) {
		ret = rbd_dev_v2_parent_info(rbd_dev);
		if (ret)
			goto out_err;
		rbd_warn(rbd_dev, "WARNING: kernel support for "
					"layered rbd images is EXPERIMENTAL!");
	}

	/* If the image supports fancy striping, get its parameters */

	if (rbd_dev->header.features & RBD_FEATURE_STRIPINGV2) {
		ret = rbd_dev_v2_striping_info(rbd_dev);
		if (ret < 0)
			goto out_err;
	}

	/* crypto and compression type aren't (yet) supported for v2 images */

	rbd_dev->header.crypt_type = 0;
	rbd_dev->header.comp_type = 0;

	/* Get the snapshot context, plus the header version */

	ret = rbd_dev_v2_snap_context(rbd_dev, &ver);
	if (ret)
		goto out_err;
	rbd_dev->header.obj_version = ver;

	dout("discovered version 2 image, header name is %s\n",
		rbd_dev->header_name);

	return 0;
out_err:
	rbd_dev->parent_overlap = 0;
	rbd_spec_put(rbd_dev->parent_spec);
	rbd_dev->parent_spec = NULL;
	kfree(rbd_dev->header_name);
	rbd_dev->header_name = NULL;
	kfree(rbd_dev->header.object_prefix);
	rbd_dev->header.object_prefix = NULL;

	return ret;
}

static int rbd_dev_probe_finish(struct rbd_device *rbd_dev)
{
	struct rbd_device *parent = NULL;
	struct rbd_spec *parent_spec = NULL;
	struct rbd_client *rbdc = NULL;
	int ret;

	/* no need to lock here, as rbd_dev is not registered yet */
	ret = rbd_dev_snaps_update(rbd_dev);
	if (ret)
		return ret;

	ret = rbd_dev_probe_update_spec(rbd_dev);
	if (ret)
		goto err_out_snaps;

	ret = rbd_dev_set_mapping(rbd_dev);
	if (ret)
		goto err_out_snaps;

	/* generate unique id: find highest unique id, add one */
	rbd_dev_id_get(rbd_dev);

	/* Fill in the device name, now that we have its id. */
	BUILD_BUG_ON(DEV_NAME_LEN
			< sizeof (RBD_DRV_NAME) + MAX_INT_FORMAT_WIDTH);
	sprintf(rbd_dev->name, "%s%d", RBD_DRV_NAME, rbd_dev->dev_id);

	/* Get our block major device number. */

	ret = register_blkdev(0, rbd_dev->name);
	if (ret < 0)
		goto err_out_id;
	rbd_dev->major = ret;

	/* Set up the blkdev mapping. */

	ret = rbd_init_disk(rbd_dev);
	if (ret)
		goto err_out_blkdev;

	ret = rbd_bus_add_dev(rbd_dev);
	if (ret)
		goto err_out_disk;

	/*
	 * At this point cleanup in the event of an error is the job
	 * of the sysfs code (initiated by rbd_bus_del_dev()).
	 */
	/* Probe the parent if there is one */

	if (rbd_dev->parent_spec) {
		/*
		 * We need to pass a reference to the client and the
		 * parent spec when creating the parent rbd_dev.
		 * Images related by parent/child relationships
		 * always share both.
		 */
		parent_spec = rbd_spec_get(rbd_dev->parent_spec);
		rbdc = __rbd_get_client(rbd_dev->rbd_client);

		parent = rbd_dev_create(rbdc, parent_spec);
		if (!parent) {
			ret = -ENOMEM;
			goto err_out_spec;
		}
		rbdc = NULL;		/* parent now owns reference */
		parent_spec = NULL;	/* parent now owns reference */
		ret = rbd_dev_probe(parent);
		if (ret < 0)
			goto err_out_parent;
		rbd_dev->parent = parent;
	}

	ret = rbd_dev_header_watch_sync(rbd_dev, 1);
	if (ret)
		goto err_out_bus;

	/* Everything's ready.  Announce the disk to the world. */

	add_disk(rbd_dev->disk);

	pr_info("%s: added with size 0x%llx\n", rbd_dev->disk->disk_name,
		(unsigned long long) rbd_dev->mapping.size);

	return ret;

err_out_parent:
	rbd_dev_destroy(parent);
err_out_spec:
	rbd_spec_put(parent_spec);
	rbd_put_client(rbdc);
err_out_bus:
	/* this will also clean up rest of rbd_dev stuff */

	rbd_bus_del_dev(rbd_dev);

	return ret;
err_out_disk:
	rbd_free_disk(rbd_dev);
err_out_blkdev:
	unregister_blkdev(rbd_dev->major, rbd_dev->name);
err_out_id:
	rbd_dev_id_put(rbd_dev);
err_out_snaps:
	rbd_remove_all_snaps(rbd_dev);

	return ret;
}

/*
 * Probe for the existence of the header object for the given rbd
 * device.  For format 2 images this includes determining the image
 * id.
 */
static int rbd_dev_probe(struct rbd_device *rbd_dev)
{
	int ret;

	/*
	 * Get the id from the image id object.  If it's not a
	 * format 2 image, we'll get ENOENT back, and we'll assume
	 * it's a format 1 image.
	 */
	ret = rbd_dev_image_id(rbd_dev);
	if (ret)
		return ret;
	rbd_assert(rbd_dev->spec->image_id);
	rbd_assert(rbd_image_format_valid(rbd_dev->image_format));

	if (rbd_dev->image_format == 1)
		ret = rbd_dev_v1_probe(rbd_dev);
	else
		ret = rbd_dev_v2_probe(rbd_dev);
	if (ret)
		goto out_err;

	ret = rbd_dev_probe_finish(rbd_dev);
	if (ret)
		rbd_header_free(&rbd_dev->header);

	return ret;
out_err:
	kfree(rbd_dev->spec->image_id);
	rbd_dev->spec->image_id = NULL;

	dout("probe failed, returning %d\n", ret);

	return ret;
}

static ssize_t rbd_add(struct bus_type *bus,
		       const char *buf,
		       size_t count)
{
	struct rbd_device *rbd_dev = NULL;
	struct ceph_options *ceph_opts = NULL;
	struct rbd_options *rbd_opts = NULL;
	struct rbd_spec *spec = NULL;
	struct rbd_client *rbdc;
	struct ceph_osd_client *osdc;
	int rc = -ENOMEM;

	if (!try_module_get(THIS_MODULE))
		return -ENODEV;

	/* parse add command */
	rc = rbd_add_parse_args(buf, &ceph_opts, &rbd_opts, &spec);
	if (rc < 0)
		goto err_out_module;

	rbdc = rbd_get_client(ceph_opts);
	if (IS_ERR(rbdc)) {
		rc = PTR_ERR(rbdc);
		goto err_out_args;
	}
	ceph_opts = NULL;	/* rbd_dev client now owns this */

	/* pick the pool */
	osdc = &rbdc->client->osdc;
	rc = ceph_pg_poolid_by_name(osdc->osdmap, spec->pool_name);
	if (rc < 0)
		goto err_out_client;
	spec->pool_id = (u64)rc;

	/* The ceph file layout needs to fit pool id in 32 bits */

	if (spec->pool_id > (u64)U32_MAX) {
		rbd_warn(NULL, "pool id too large (%llu > %u)\n",
				(unsigned long long)spec->pool_id, U32_MAX);
		rc = -EIO;
		goto err_out_client;
	}

	rbd_dev = rbd_dev_create(rbdc, spec);
	if (!rbd_dev)
		goto err_out_client;
	rbdc = NULL;		/* rbd_dev now owns this */
	spec = NULL;		/* rbd_dev now owns this */

	rbd_dev->mapping.read_only = rbd_opts->read_only;
	kfree(rbd_opts);
	rbd_opts = NULL;	/* done with this */

	rc = rbd_dev_probe(rbd_dev);
	if (rc < 0)
		goto err_out_rbd_dev;

	return count;
err_out_rbd_dev:
	rbd_dev_destroy(rbd_dev);
err_out_client:
	rbd_put_client(rbdc);
err_out_args:
	if (ceph_opts)
		ceph_destroy_options(ceph_opts);
	kfree(rbd_opts);
	rbd_spec_put(spec);
err_out_module:
	module_put(THIS_MODULE);

	dout("Error adding device %s\n", buf);

	return (ssize_t)rc;
}

static struct rbd_device *__rbd_get_dev(unsigned long dev_id)
{
	struct list_head *tmp;
	struct rbd_device *rbd_dev;

	spin_lock(&rbd_dev_list_lock);
	list_for_each(tmp, &rbd_dev_list) {
		rbd_dev = list_entry(tmp, struct rbd_device, node);
		if (rbd_dev->dev_id == dev_id) {
			spin_unlock(&rbd_dev_list_lock);
			return rbd_dev;
		}
	}
	spin_unlock(&rbd_dev_list_lock);
	return NULL;
}

static void rbd_dev_release(struct device *dev)
{
	struct rbd_device *rbd_dev = dev_to_rbd_dev(dev);

	if (rbd_dev->watch_event)
		rbd_dev_header_watch_sync(rbd_dev, 0);

	/* clean up and free blkdev */
	rbd_free_disk(rbd_dev);
	unregister_blkdev(rbd_dev->major, rbd_dev->name);

	/* release allocated disk header fields */
	rbd_header_free(&rbd_dev->header);

	/* done with the id, and with the rbd_dev */
	rbd_dev_id_put(rbd_dev);
	rbd_assert(rbd_dev->rbd_client != NULL);
	rbd_dev_destroy(rbd_dev);

	/* release module ref */
	module_put(THIS_MODULE);
}

static void __rbd_remove(struct rbd_device *rbd_dev)
{
	rbd_remove_all_snaps(rbd_dev);
	rbd_bus_del_dev(rbd_dev);
}

static ssize_t rbd_remove(struct bus_type *bus,
			  const char *buf,
			  size_t count)
{
	struct rbd_device *rbd_dev = NULL;
	int target_id, rc;
	unsigned long ul;
	int ret = count;

	rc = strict_strtoul(buf, 10, &ul);
	if (rc)
		return rc;

	/* convert to int; abort if we lost anything in the conversion */
	target_id = (int) ul;
	if (target_id != ul)
		return -EINVAL;

	mutex_lock_nested(&ctl_mutex, SINGLE_DEPTH_NESTING);

	rbd_dev = __rbd_get_dev(target_id);
	if (!rbd_dev) {
		ret = -ENOENT;
		goto done;
	}

	spin_lock_irq(&rbd_dev->lock);
	if (rbd_dev->open_count)
		ret = -EBUSY;
	else
		set_bit(RBD_DEV_FLAG_REMOVING, &rbd_dev->flags);
	spin_unlock_irq(&rbd_dev->lock);
	if (ret < 0)
		goto done;

	while (rbd_dev->parent_spec) {
		struct rbd_device *first = rbd_dev;
		struct rbd_device *second = first->parent;
		struct rbd_device *third;

		/*
		 * Follow to the parent with no grandparent and
		 * remove it.
		 */
		while (second && (third = second->parent)) {
			first = second;
			second = third;
		}
		__rbd_remove(second);
		rbd_spec_put(first->parent_spec);
		first->parent_spec = NULL;
		first->parent_overlap = 0;
		first->parent = NULL;
	}
	__rbd_remove(rbd_dev);

done:
	mutex_unlock(&ctl_mutex);

	return ret;
}

/*
 * create control files in sysfs
 * /sys/bus/rbd/...
 */
static int rbd_sysfs_init(void)
{
	int ret;

	ret = device_register(&rbd_root_dev);
	if (ret < 0)
		return ret;

	ret = bus_register(&rbd_bus_type);
	if (ret < 0)
		device_unregister(&rbd_root_dev);

	return ret;
}

static void rbd_sysfs_cleanup(void)
{
	bus_unregister(&rbd_bus_type);
	device_unregister(&rbd_root_dev);
}

static int __init rbd_init(void)
{
	int rc;

	if (!libceph_compatible(NULL)) {
		rbd_warn(NULL, "libceph incompatibility (quitting)");

		return -EINVAL;
	}
	rc = rbd_sysfs_init();
	if (rc)
		return rc;
	pr_info("loaded " RBD_DRV_NAME_LONG "\n");
	return 0;
}

static void __exit rbd_exit(void)
{
	rbd_sysfs_cleanup();
}

module_init(rbd_init);
module_exit(rbd_exit);

MODULE_AUTHOR("Sage Weil <sage@newdream.net>");
MODULE_AUTHOR("Yehuda Sadeh <yehuda@hq.newdream.net>");
MODULE_DESCRIPTION("rados block device");

/* following authorship retained from original osdblk.c */
MODULE_AUTHOR("Jeff Garzik <jeff@garzik.org>");

MODULE_LICENSE("GPL");
