// SPDX-License-Identifier: GPL-2.0-only
/*
 * panic_logstore.c - Dump kernel ring buffer to persistent file on panic
 *
 * When the kernel panics, this module writes the dmesg ring buffer to
 * /mnt/oplus/op2/last_panic.txt so logs can be retrieved after reboot.
 * It temporarily drops SELinux to permissive and overrides creds to root
 * to bypass filesystem access restrictions.
 *
 * A module parameter "trigger" allows manual invocation for hung-task
 * debugging: echo 1 > /sys/module/panic_logstore/parameters/trigger
 *
 * Adapted from libxzr/kernel-playground commit 1add6c0 (4.19) for 5.10:
 *   - call_blocking_lsm_notifier() replaces call_lsm_notifier()
 *   - kmsg_dump_get_line() API unchanged
 *   - override_creds/revert_creds API unchanged
 *
 * Limitations (from original author):
 *   "Optimistically assumes filesystems and storage drivers are not to
 *    blame for the panic. Can't catch logs before the target partition
 *    is properly mounted."
 *
 * Copyright (c) 2024, libxzr
 * Copyright (c) 2025, OPLUS SM8250 5.10 port
 */

#include <linux/cred.h>
#include <linux/err.h>
#include <linux/fcntl.h>
#include <linux/fs.h>
#include <linux/kmsg_dump.h>
#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/panic_logstore.h>
#include <linux/printk.h>
#include <linux/types.h>

/* Target file path for panic log storage */
#define LOGSTORE_PATH "/mnt/oplus/op2/last_panic.txt"
#define LOGSTORE_LINE_SIZE 1024

/* Module parameter: manual trigger for hung-task debugging */
static int trigger;

/**
 * do_logstore() - Dump kernel ring buffer to persistent file
 *
 * Steps:
 *   1. Switch SELinux to permissive (if CONFIG_SECURITY_SELINUX_DEVELOP)
 *   2. Override credentials to root (uid=0, gid=0, full capabilities)
 *   3. Open LOGSTORE_PATH for writing (O_WRONLY | O_CREAT | O_TRUNC)
 *   4. Iterate kmsg_dump_get_line() to write entire ring buffer
 *   5. vfs_fsync() to ensure data reaches storage
 *   6. filp_close()
 *   7. Revert credentials
 *   8. Re-enforce SELinux
 *
 * Must be called from process context with local IRQs enabled.
 * Optimistically assumes filesystems and storage are functional.
 */
void do_logstore(void)
{
	struct kmsg_dumper dumper = { .active = true };
	struct file *file;
	struct cred *new_cred;
	const struct cred *old_cred;
	char line[LOGSTORE_LINE_SIZE];
	size_t len;
	loff_t pos = 0;
	ssize_t written;
	int ret;

#ifdef CONFIG_SECURITY_SELINUX_DEVELOP
	int was_enforcing = 0;
#endif

	new_cred = prepare_kernel_cred(NULL);
	if (!new_cred) {
		pr_emerg("logstore: failed to prepare kernel credentials\n");
		return;
	}

	old_cred = override_creds(new_cred);

#ifdef CONFIG_SECURITY_SELINUX_DEVELOP
	was_enforcing = sel_get_enforce();
	if (was_enforcing)
		sel_set_enforce(0);
#endif

	file = filp_open(LOGSTORE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (IS_ERR(file)) {
		pr_emerg("logstore: failed to open %s: %ld\n",
			 LOGSTORE_PATH, PTR_ERR(file));
		goto err_restore_selinux;
	}

	kmsg_dump_rewind(&dumper);
	while (kmsg_dump_get_line(&dumper, true, line, sizeof(line), &len)) {
		written = kernel_write(file, line, len, &pos);
		if (written < 0) {
			ret = written;
			pr_emerg("logstore: failed to write %s: %d\n",
				 LOGSTORE_PATH, ret);
			goto err_close_file;
		}

		if ((size_t)written != len) {
			pr_emerg("logstore: short write to %s: %zd/%zu\n",
				 LOGSTORE_PATH, written, len);
			break;
		}
	}

	ret = vfs_fsync(file, 0);
	if (ret)
		pr_emerg("logstore: failed to fsync %s: %d\n",
			 LOGSTORE_PATH, ret);

err_close_file:
	ret = filp_close(file, NULL);
	if (ret)
		pr_emerg("logstore: failed to close %s: %d\n",
			 LOGSTORE_PATH, ret);

err_restore_selinux:

#ifdef CONFIG_SECURITY_SELINUX_DEVELOP
	if (was_enforcing)
		sel_set_enforce(1);
#endif

	revert_creds(old_cred);
	put_cred(new_cred);
}

/**
 * trigger_store() - Module parameter callback for manual trigger
 * @val: String written to the trigger parameter.
 * @kp: Kernel parameter metadata.
 *
 * Writing 1 to /sys/module/panic_logstore/parameters/trigger
 * will invoke do_logstore() immediately. Useful for debugging
 * hung tasks where the system hasn't actually panicked yet.
 *
 * Return: 0 on success, negative errno on parse failure.
 */
static int trigger_store(const char *val, const struct kernel_param *kp)
{
	int ret;

	ret = kstrtoint(val, 10, &trigger);
	if (ret)
		return ret;

	if (trigger == 1) {
		pr_emerg("logstore: manual trigger invoked\n");
		do_logstore();
		trigger = 0;
	}

	return 0;
}

static const struct kernel_param_ops trigger_ops = {
	.set = trigger_store,
	.get = param_get_int,
};

module_param_cb(trigger, &trigger_ops, &trigger, 0600);
MODULE_PARM_DESC(trigger, "Manually trigger logstore dump (set to 1)");

MODULE_LICENSE("GPL");
MODULE_AUTHOR("libxzr <libxzr@gmail.com>");
MODULE_AUTHOR("OPLUS SM8250 5.10 port");
MODULE_DESCRIPTION("Dump kernel ring buffer to persistent file on panic");
