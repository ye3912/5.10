# SPDX-License-Identifier: GPL-2.0
# OPLUS locking strategy — vendor_hook callbacks for mutex/rwsem/futex
#
# Ported from 4.19: kernel/sched_assist/Makefile (oplus_locking_strategy target)

obj-$(CONFIG_OPLUS_LOCKING_STRATEGY) += oplus_locking_strategy.o

oplus_locking_strategy-y += locking_main.o
oplus_locking_strategy-y += mutex.o
oplus_locking_strategy-y += rwsem.o
oplus_locking_strategy-y += futex.o
oplus_locking_strategy-y += sysfs.o
