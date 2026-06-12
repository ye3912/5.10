/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * OPLUS PPS (Programmable Power Supply) stub for 5.10 GKI — Phase 3.
 */
#ifndef _OPLUS_PPS_H
#define _OPLUS_PPS_H

enum {
	PPS_SUPPORT_NONE = 0,
	PPS_SUPPORT_1CP = 1,
	PPS_SUPPORT_2CP = 2,
	PPS_SUPPORT_3CP = 3,
};

static inline int oplus_pps_get_support_type(void) { return PPS_SUPPORT_NONE; }

#endif /* _OPLUS_PPS_H */
