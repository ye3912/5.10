// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2022 Oplus. All rights reserved.
 *
 * Ported from 4.19: kernel/sched_assist/sync/sysfs.c
 * 5.10 adaptations:
 *  - struct file_operations → struct proc_ops (5.10 proc_create requires it)
 *  - S_IRUGO | S_IWUGO → 0666 (equivalent, more explicit)
 */

#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>

#include "locking_main.h"

/* Default off */
int thread_info_ctrl = 0;

static ssize_t thread_ctrl_write(struct file *file, const char __user *buf,
				 size_t count, loff_t *ppos)
{
	char kbuf[5] = {0};
	int err, onoff;

	if (count >= 5)
		return -EFAULT;

	if (copy_from_user(kbuf, buf, count)) {
		pr_err("ERROR : Failed to copy_from_user to set thread_ctrl flag\n");
		return -EFAULT;
	}
	err = kstrtoint(strstrip(kbuf), 0, &onoff);
	if (err < 0) {
		pr_err("ERROR : Failed to kstrtoint to set thread_ctrl flag\n");
		return -EFAULT;
	}
	thread_info_ctrl = !!onoff;
	return count;
}

static int thread_ctrl_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", thread_info_ctrl);
	return 0;
}

static int thread_ctrl_open(struct inode *inode, struct file *file)
{
	return single_open(file, thread_ctrl_show, inode);
}

static const struct proc_ops thread_ctrl_ops = {
	.proc_open	= thread_ctrl_open,
	.proc_write	= thread_ctrl_write,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

#define OPLUS_LOCKING_PROC_DIR		"oplus_locking"
struct proc_dir_entry *d_oplus_locking;

void lk_sysfs_init(void)
{
	d_oplus_locking = proc_mkdir(OPLUS_LOCKING_PROC_DIR, NULL);

	if (d_oplus_locking) {
		proc_create("thread_info_ctrl", 0666, d_oplus_locking,
			    &thread_ctrl_ops);
		pr_info("sysfs init success!!\n");
	}
}

void lk_sysfs_exit(void)
{
	if (d_oplus_locking) {
		remove_proc_entry("thread_info_ctrl", d_oplus_locking);
		remove_proc_entry(OPLUS_LOCKING_PROC_DIR, NULL);
	}
}
