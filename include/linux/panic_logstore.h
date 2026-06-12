/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * panic_logstore.h - Kernel panic log storage to persistent filesystem
 *
 * When the kernel panics (or is manually triggered), this module dumps
 * the kernel ring buffer to /mnt/oplus/op2/last_panic.txt so that the
 * last messages before the crash can be retrieved after reboot.
 *
 * Adapted from libxzr/kernel-playground (4.19) for Android GKI 5.10.
 * Key 5.10 differences:
 *   - call_blocking_lsm_notifier() instead of call_lsm_notifier()
 *   - kmsg_dump API unchanged
 *
 * Copyright (c) 2024, libxzr
 * Copyright (c) 2025, OPLUS SM8250 5.10 port
 */

#ifndef _LINUX_PANIC_LOGSTORE_H
#define _LINUX_PANIC_LOGSTORE_H

#ifdef CONFIG_SECURITY_SELINUX_DEVELOP
int sel_get_enforce(void);
void sel_set_enforce(int new_value);
#endif

#ifdef CONFIG_PANIC_LOGSTORE

/**
 * do_logstore() - Dump kernel ring buffer to persistent file
 *
 * This function temporarily switches SELinux to permissive mode,
 * overrides credentials to root, opens the target file, and writes
 * the entire kernel ring buffer via kmsg_dump_get_line(). After
 * writing, it re-enforces SELinux and reverts credentials.
 *
 * Must be called from process context with interrupts enabled.
 * Optimistically assumes filesystems and storage drivers are functional.
 */
void do_logstore(void);

#else

static inline void do_logstore(void) {}

#endif /* CONFIG_PANIC_LOGSTORE */

#endif /* _LINUX_PANIC_LOGSTORE_H */
