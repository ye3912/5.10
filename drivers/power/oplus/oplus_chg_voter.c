/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020-2022 Oplus. All rights reserved.
 *
 * Ported from 4.19 — simplified for 5.10 GKI skeleton.
 * Full debugfs integration deferred to Phase 3.
 */

#define pr_fmt(fmt) "[OPLUS_CHG_VOTER] %s: " fmt, __func__

#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include "oplus_chg_voter.h"

/*
 * voter_comparator — sort votes ascending by value for VOTE_MIN,
 * descending for VOTE_MAX.
 */
static int voter_comparator(const void *a, const void *b)
{
	const struct oplus_chg_vote *va = *(struct oplus_chg_vote **)a;
	const struct oplus_chg_vote *vb = *(struct oplus_chg_vote **)b;
	return va->value - vb->value;
}

/*
 * oplus_chg_voter_reroll — re-evaluate all effective votes and return the winner.
 */
int oplus_chg_voter_reroll(struct oplus_chg_voter *voter)
{
	struct oplus_chg_vote *vote;
	struct oplus_chg_vote *votes[64] = { };
	int effective_value = 0;
	int count = 0;

	mutex_lock(&voter->voter_lock);

	list_for_each_entry(vote, &voter->voted_list, list) {
		if (vote->avaliable && vote->effective) {
			votes[count++] = vote;
			if (count >= ARRAY_SIZE(votes)) {
				pr_warn("too many votes for voter=%s\n",
					voter->name);
				break;
			}
		}
	}

	if (count == 0) {
		pr_debug("no effective votes for voter=%s\n", voter->name);
		mutex_unlock(&voter->voter_lock);
		return 0;
	}

	sort(votes, count, sizeof(*votes), voter_comparator, NULL);
	effective_value = votes[count - 1]->value;
	mutex_unlock(&voter->voter_lock);

	return effective_value;
}

/*
 * oplus_chg_voter_get_effective_value — return the standing effective value.
 */
int oplus_chg_voter_get_effective_value(struct oplus_chg_voter *voter)
{
	return oplus_chg_voter_reroll(voter);
}

/*
 * get_client_vote — locate a named vote within a voter, for reuse.
 */
static struct oplus_chg_vote *
get_client_vote(struct oplus_chg_voter *voter, const char *name)
{
	struct oplus_chg_vote *vote;

	list_for_each_entry(vote, &voter->voted_list, list) {
		if (strcmp(vote->name, name) == 0)
			return vote;
	}

	return NULL;
}

/*
 * oplus_chg_vote — create (or reuse) a named vote within a voter.
 */
struct oplus_chg_vote *oplus_chg_vote(struct oplus_chg_voter *voter,
				      const char *name,
				      bool last_effective)
{
	struct oplus_chg_vote *vote;

	mutex_lock(&voter->voter_lock);
	vote = get_client_vote(voter, name);
	if (vote) {
		vote->avaliable = true;
		vote->effective = last_effective;
		mutex_unlock(&voter->voter_lock);
		return vote;
	}

	vote = kzalloc(sizeof(*vote), GFP_KERNEL);
	if (!vote) {
		mutex_unlock(&voter->voter_lock);
		return NULL;
	}
	vote->name = name;
	vote->vot = voter;
	vote->avaliable = true;
	vote->effective = last_effective;

	list_add_tail(&vote->list, &voter->voted_list);
	mutex_unlock(&voter->voter_lock);
	return vote;
}

/*
 * oplus_chg_unvote — mark a vote as invalid (re-evaluate later).
 */
void oplus_chg_unvote(struct oplus_chg_vote *vote)
{
	if (!vote)
		return;
	mutex_lock(&vote->vot->voter_lock);
	vote->avaliable = false;
	vote->effective = false;
	mutex_unlock(&vote->vot->voter_lock);
}

/*
 * oplus_chg_vote_release — destroy a vote permanently.
 */
void oplus_chg_vote_release(struct oplus_chg_vote *vote)
{
	if (!vote)
		return;
	mutex_lock(&vote->vot->voter_lock);
	list_del(&vote->list);
	mutex_unlock(&vote->vot->voter_lock);
	kfree(vote);
}

/*
 * oplus_chg_voter_register — create a new voting domain.
 */
struct oplus_chg_voter *oplus_chg_voter_register(const char *name)
{
	struct oplus_chg_voter *voter;

	voter = kzalloc(sizeof(*voter), GFP_KERNEL);
	if (!voter)
		return NULL;

	voter->name = name;
	mutex_init(&voter->voter_lock);
	INIT_LIST_HEAD(&voter->vote_list);
	INIT_LIST_HEAD(&voter->voted_list);
	voter->avaliable = true;

	pr_debug("voter=%s registered\n", name);
	return voter;
}

/*
 * oplus_chg_voter_unregister — tear down a voting domain.
 */
void oplus_chg_voter_unregister(struct oplus_chg_voter *voter)
{
	struct oplus_chg_vote *vote, *tmp;

	if (!voter)
		return;

	mutex_lock(&voter->voter_lock);
	list_for_each_entry_safe(vote, tmp, &voter->voted_list, list) {
		list_del(&vote->list);
		kfree(vote);
	}
	voter->avaliable = false;
	mutex_unlock(&voter->voter_lock);
	kfree(voter);
	pr_debug("voter unregistered\n");
}
EXPORT_SYMBOL_GPL(oplus_chg_voter_register);
EXPORT_SYMBOL_GPL(oplus_chg_voter_unregister);
EXPORT_SYMBOL_GPL(oplus_chg_vote);
EXPORT_SYMBOL_GPL(oplus_chg_unvote);
EXPORT_SYMBOL_GPL(oplus_chg_vote_release);
EXPORT_SYMBOL_GPL(oplus_chg_voter_reroll);
EXPORT_SYMBOL_GPL(oplus_chg_voter_get_effective_value);
