/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * OPLUS battery log stub for 5.10 GKI — Phase 3.
 */
#ifndef _OPLUS_BATTERY_LOG_H
#define _OPLUS_BATTERY_LOG_H

struct battery_log_ops {
	void *data;
};

static inline int battery_log_ops_register(struct battery_log_ops *ops)
{
	return 0;
}

#endif /* _OPLUS_BATTERY_LOG_H */
