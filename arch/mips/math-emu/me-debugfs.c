#include <linux/cpumask.h>
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/percpu.h>
#include <linux/types.h>
#include <asm/fpu_emulator.h>
#include <asm/local.h>

DEFINE_PER_CPU(struct mips_fpu_emulator_stats, fpuemustats);

static int fpuemu_stat_get(void *data, u64 *val)
{
	int cpu;
	unsigned long sum = 0;

	for_each_online_cpu(cpu) {
		struct mips_fpu_emulator_stats *ps;
		local_t *pv;

		ps = &per_cpu(fpuemustats, cpu);
		pv = (void *)ps + (unsigned long)data;
		sum += local_read(pv);
	}
	*val = sum;
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(fops_fpuemu_stat, fpuemu_stat_get, NULL, "%llu\n");

extern struct dentry *mips_debugfs_dir;
static int __init debugfs_fpuemu(void)
{
	struct dentry *d, *dir;

	if (!mips_debugfs_dir)
		return -ENODEV;
	dir = debugfs_create_dir("fpuemustats", mips_debugfs_dir);
	if (!dir)
		return -ENOMEM;

#define FPU_STAT_CREATE(M)						\
	do {								\
		d = debugfs_create_file(#M , S_IRUGO, dir,		\
			(void *)offsetof(struct mips_fpu_emulator_stats, M), \
			&fops_fpuemu_stat);				\
		if (!d)							\
			return -ENOMEM;					\
	} while (0)

	FPU_STAT_CREATE(emulated);
	FPU_STAT_CREATE(loads);
	FPU_STAT_CREATE(stores);
	FPU_STAT_CREATE(cp1ops);
	FPU_STAT_CREATE(cp1xops);
	FPU_STAT_CREATE(errors);

	return 0;
}
__initcall(debugfs_fpuemu);
