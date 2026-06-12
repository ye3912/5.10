/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020-2022 Oplus. All rights reserved.
 */

#ifndef _OPLUS_CHG_VOTER_H
#define _OPLUS_CHG_VOTER_H

#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/module.h>

/* Voting types */
enum voter_type {
	VOTE_MIN = 0,
	VOTE_MAX,
	VOTE_SET_ANY,
};

struct oplus_chg_voter {
	struct list_head	vote_list;
	struct list_head	voted_list;
	struct mutex		voter_lock;
	const char		*name;
	struct dentry		*debugfs_dir;
	bool			avaliable;
};

struct oplus_chg_vote {
	struct list_head	list;
	int			value;
	const char		*name;
	struct oplus_chg_voter	*vot;
	bool			avaliable;
	bool			effective;
};

/* Voter APIs */
extern struct oplus_chg_voter *oplus_chg_voter_register(const char *name);
extern void oplus_chg_voter_unregister(struct oplus_chg_voter *voter);
extern struct oplus_chg_vote *oplus_chg_vote(struct oplus_chg_voter *voter,
					     const char *name,
					     bool last_effective);
extern void oplus_chg_unvote(struct oplus_chg_vote *vote);
extern void oplus_chg_vote_release(struct oplus_chg_vote *vote);
extern int oplus_chg_voter_reroll(struct oplus_chg_voter *voter);
extern int oplus_chg_voter_get_effective_value(struct oplus_chg_voter *voter);

#endif /* _OPLUS_CHG_VOTER_H */
