// SPDX-License-Identifier: GPL-2.0-only
/*
 * OPLUS charging core — mod registration / property / notifier skeleton.
 *
 * Ported from 4.19 drivers/power/oplus/oplus_chg_core.c for 5.10 GKI.
 * Full device-attachment, power_supply linkage, and sysfs wiring are
 * deferred to Phase 3.  This skeleton provides compile-time API coverage
 * for all core symbols declared in oplus_chg_core.h.
 *
 * 5.10 adaptations:
 *   - EXPORT_SYMBOL_GPL on all public symbols (GKI rule)
 *   - Devres-managed helpers (devm_*) use devres_add/group pattern
 *   - All mods registered through oplus_chg_mod_register() are tracked
 *     in a global linked list; ops/properties return safe defaults.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>

#include "oplus_charger.h"
#include "oplus_gauge.h"
#include "oplus_chg_core.h"
#include "oplus_chg_ops_manager.h"

/* ===== LEGACY STUBS — oplus_gauge / FFC / VOOC ===== */
static struct oplus_gauge_chip *g_gauge_chip;

void oplus_gauge_init(struct oplus_gauge_chip *chip)
{
	g_gauge_chip = chip;
}
EXPORT_SYMBOL_GPL(oplus_gauge_init);

/**
 * oplus_gauge_deinit() - Unregister the main gauge provider.
 *
 * Called by gauge IC driver on remove to prevent g_gauge_chip from
 * becoming a dangling pointer.  After this call, all oplus_gauge_get_*
 * APIs fall back to conservative 4.19-style defaults.
 */
void oplus_gauge_deinit(struct oplus_gauge_chip *chip)
{
	if (g_gauge_chip == chip)
		g_gauge_chip = NULL;
}
EXPORT_SYMBOL_GPL(oplus_gauge_deinit);

int oplus_chg_get_ffc_status(void)
{
	return 0;
}
EXPORT_SYMBOL_GPL(oplus_chg_get_ffc_status);

bool oplus_vooc_get_fastchg_ing(void)
{
	return false;
}
EXPORT_SYMBOL_GPL(oplus_vooc_get_fastchg_ing);

/**
 * oplus_gauge_get_ops() - Return registered main-gauge operations.
 *
 * Return: gauge ops pointer when a gauge registered, otherwise NULL.
 */
static struct oplus_gauge_operations *oplus_gauge_get_ops(void)
{
	return g_gauge_chip ? g_gauge_chip->gauge_ops : NULL;
}

/**
 * oplus_gauge_check_chip_is_null() - Check whether the main gauge exists.
 *
 * Return: true when no gauge has registered, otherwise false.
 */
bool oplus_gauge_check_chip_is_null(void)
{
	return !g_gauge_chip;
}
EXPORT_SYMBOL_GPL(oplus_gauge_check_chip_is_null);

/**
 * oplus_gauge_get_batt_soc() - Read main battery capacity.
 *
 * Return: gauge-provided capacity percentage, or -1 when no gauge
 *         has registered (matches 4.19 fallback semantics).
 */
int oplus_gauge_get_batt_soc(void)
{
	struct oplus_gauge_operations *ops = oplus_gauge_get_ops();

	if (ops && ops->get_battery_soc)
		return ops->get_battery_soc();

	return -1;
}
EXPORT_SYMBOL_GPL(oplus_gauge_get_batt_soc);

/**
 * oplus_gauge_get_batt_mvolts() - Read main battery voltage.
 *
 * Return: gauge-provided voltage in millivolts, or 3800.
 */
int oplus_gauge_get_batt_mvolts(void)
{
	struct oplus_gauge_operations *ops = oplus_gauge_get_ops();

	if (ops && ops->get_battery_mvolts)
		return ops->get_battery_mvolts();

	return 3800;
}
EXPORT_SYMBOL_GPL(oplus_gauge_get_batt_mvolts);

/**
 * oplus_gauge_get_batt_temperature() - Read main battery temperature.
 *
 * Return: gauge-provided temperature in 0.1 degree Celsius units, or 250.
 */
int oplus_gauge_get_batt_temperature(void)
{
	struct oplus_gauge_operations *ops = oplus_gauge_get_ops();

	if (ops && ops->get_battery_temperature)
		return ops->get_battery_temperature();

	return 250;
}
EXPORT_SYMBOL_GPL(oplus_gauge_get_batt_temperature);

/**
 * oplus_gauge_get_batt_current() - Read average battery current.
 *
 * Return: gauge-provided current in milliamps, or 100 when no gauge
 *         has registered (matches 4.19 fallback semantics).
 */
int oplus_gauge_get_batt_current(void)
{
	struct oplus_gauge_operations *ops = oplus_gauge_get_ops();

	if (ops && ops->get_average_current)
		return ops->get_average_current();

	return 100;
}
EXPORT_SYMBOL_GPL(oplus_gauge_get_batt_current);

/**
 * oplus_gauge_get_batt_authenticate() - Read battery authentication state.
 *
 * Return: gauge-provided authentication state, or false when no gauge
 *         has registered (matches 4.19 fallback semantics).
 */
bool oplus_gauge_get_batt_authenticate(void)
{
	struct oplus_gauge_operations *ops = oplus_gauge_get_ops();

	if (ops && ops->get_battery_authenticate)
		return ops->get_battery_authenticate();

	return false;
}
EXPORT_SYMBOL_GPL(oplus_gauge_get_batt_authenticate);

/**
 * oplus_gauge_get_batt_soh() - Read battery state of health.
 *
 * Return: gauge-provided state of health percentage, or 0 when no gauge
 *         has registered (matches 4.19 fallback semantics).
 */
int oplus_gauge_get_batt_soh(void)
{
	struct oplus_gauge_operations *ops = oplus_gauge_get_ops();

	if (ops && ops->get_battery_soh)
		return ops->get_battery_soh();

	return 0;
}
EXPORT_SYMBOL_GPL(oplus_gauge_get_batt_soh);

/* ===== MOD REGISTRY ===== */

static LIST_HEAD(g_ocm_list);
static DEFINE_MUTEX(g_ocm_list_lock);

/* global event notifier — posted by framework events */
ATOMIC_NOTIFIER_HEAD(oplus_chg_event_notifier);
EXPORT_SYMBOL_GPL(oplus_chg_event_notifier);

ATOMIC_NOTIFIER_HEAD(oplus_chg_changed_notifier);
EXPORT_SYMBOL_GPL(oplus_chg_changed_notifier);

/* ===== Core registration implementation ===== */

static void oplus_chg_mod_register_common(struct device *parent,
					  const struct oplus_chg_mod_desc *desc,
					  const struct oplus_chg_mod_config *cfg,
					  struct oplus_chg_mod *ocm,
					  bool wakeup_source)
{
	ocm->desc = desc;
	ocm->drv_data = cfg ? cfg->drv_data : NULL;
	ocm->of_node = cfg ? cfg->of_node : NULL;
	atomic_set(&ocm->use_cnt, 1);
	spin_lock_init(&ocm->changed_lock);
	ocm->initialized = true;

	mutex_lock(&g_ocm_list_lock);
	list_add_tail(&ocm->list, &g_ocm_list);
	mutex_unlock(&g_ocm_list_lock);

	chg_info("mod=%s registered (type=%d wakeup=%d)\n",
		 desc->name, desc->type, wakeup_source);
}

static void oplus_chg_mod_unregister_common(struct oplus_chg_mod *ocm,
					    bool wakeup_source)
{
	mutex_lock(&g_ocm_list_lock);
	list_del(&ocm->list);
	mutex_unlock(&g_ocm_list_lock);

	chg_info("mod=%s unregistered\n", ocm->desc ? ocm->desc->name : "?");
	kfree(ocm);
}

struct oplus_chg_mod *oplus_chg_mod_register(struct device *parent,
	const struct oplus_chg_mod_desc *desc,
	const struct oplus_chg_mod_config *cfg)
{
	struct oplus_chg_mod *ocm;

	ocm = kzalloc(sizeof(*ocm), GFP_KERNEL);
	if (!ocm)
		return ERR_PTR(-ENOMEM);

	oplus_chg_mod_register_common(parent, desc, cfg, ocm, true);
	return ocm;
}
EXPORT_SYMBOL_GPL(oplus_chg_mod_register);

struct oplus_chg_mod *oplus_chg_mod_register_no_ws(struct device *parent,
	const struct oplus_chg_mod_desc *desc,
	const struct oplus_chg_mod_config *cfg)
{
	struct oplus_chg_mod *ocm;

	ocm = kzalloc(sizeof(*ocm), GFP_KERNEL);
	if (!ocm)
		return ERR_PTR(-ENOMEM);

	oplus_chg_mod_register_common(parent, desc, cfg, ocm, false);
	return ocm;
}
EXPORT_SYMBOL_GPL(oplus_chg_mod_register_no_ws);

struct oplus_chg_mod *devm_oplus_chg_mod_register(struct device *parent,
	const struct oplus_chg_mod_desc *desc,
	const struct oplus_chg_mod_config *cfg)
{
	struct oplus_chg_mod *ocm;

	ocm = oplus_chg_mod_register(parent, desc, cfg);
	if (IS_ERR(ocm))
		return ocm;

	/* devres-managed: auto-unregister on driver detach */
	devm_add_action(parent,
		(void (*)(void *))oplus_chg_mod_unregister, ocm);
	return ocm;
}
EXPORT_SYMBOL_GPL(devm_oplus_chg_mod_register);

struct oplus_chg_mod *devm_oplus_chg_mod_register_no_ws(struct device *parent,
	const struct oplus_chg_mod_desc *desc,
	const struct oplus_chg_mod_config *cfg)
{
	struct oplus_chg_mod *ocm;

	ocm = oplus_chg_mod_register_no_ws(parent, desc, cfg);
	if (IS_ERR(ocm))
		return ocm;

	devm_add_action(parent,
		(void (*)(void *))oplus_chg_mod_unregister, ocm);
	return ocm;
}
EXPORT_SYMBOL_GPL(devm_oplus_chg_mod_register_no_ws);

void oplus_chg_mod_unregister(struct oplus_chg_mod *ocm)
{
	if (!ocm)
		return;

	oplus_chg_mod_unregister_common(ocm, true);
}
EXPORT_SYMBOL_GPL(oplus_chg_mod_unregister);

/* ===== Mod lookup ===== */

struct oplus_chg_mod *oplus_chg_mod_get_by_name(const char *name)
{
	struct oplus_chg_mod *ocm, *found = NULL;

	mutex_lock(&g_ocm_list_lock);
	list_for_each_entry(ocm, &g_ocm_list, list) {
		if (ocm->desc && strcmp(ocm->desc->name, name) == 0) {
			atomic_inc(&ocm->use_cnt);
			found = ocm;
			break;
		}
	}
	mutex_unlock(&g_ocm_list_lock);

	return found;
}
EXPORT_SYMBOL_GPL(oplus_chg_mod_get_by_name);

void oplus_chg_mod_put(struct oplus_chg_mod *ocm)
{
	if (ocm)
		atomic_dec(&ocm->use_cnt);
}
EXPORT_SYMBOL_GPL(oplus_chg_mod_put);

void *oplus_chg_mod_get_drvdata(struct oplus_chg_mod *ocm)
{
	return ocm ? ocm->drv_data : NULL;
}
EXPORT_SYMBOL_GPL(oplus_chg_mod_get_drvdata);

/* ===== Property access ===== */

int oplus_chg_mod_get_property(struct oplus_chg_mod *ocm,
			       enum oplus_chg_mod_property ocm_prop,
			       union oplus_chg_mod_propval *val)
{
	if (!ocm)
		return -EINVAL;

	if (ocm->desc && ocm->desc->get_property)
		return ocm->desc->get_property(ocm, ocm_prop, val);

	/* Default: all properties return -ENODATA */
	return -ENODATA;
}
EXPORT_SYMBOL_GPL(oplus_chg_mod_get_property);

int oplus_chg_mod_set_property(struct oplus_chg_mod *ocm,
			       enum oplus_chg_mod_property ocm_prop,
			       const union oplus_chg_mod_propval *val)
{
	if (!ocm)
		return -EINVAL;

	if (ocm->desc && ocm->desc->set_property)
		return ocm->desc->set_property(ocm, ocm_prop, val);

	return -ENOSYS;
}
EXPORT_SYMBOL_GPL(oplus_chg_mod_set_property);

int oplus_chg_mod_property_is_writeable(struct oplus_chg_mod *ocm,
					enum oplus_chg_mod_property p)
{
	if (!ocm)
		return 0;

	if (ocm->desc && ocm->desc->property_is_writeable)
		return ocm->desc->property_is_writeable(ocm, p);

	return 0;
}
EXPORT_SYMBOL_GPL(oplus_chg_mod_property_is_writeable);

/* ===== Power-supply linkage stub ===== */

int oplus_chg_mod_powers(struct oplus_chg_mod *ocm, struct device *dev)
{
	if (!ocm || !dev)
		return -EINVAL;

	/* Stub — full power_supply registration deferred */
	return 0;
}
EXPORT_SYMBOL_GPL(oplus_chg_mod_powers);

/* ===== Notifier API stubs ===== */

static ATOMIC_NOTIFIER_HEAD(g_chg_changed_nh);
static ATOMIC_NOTIFIER_HEAD(g_chg_event_nh);

int oplus_chg_reg_changed_notifier(struct notifier_block *nb)
{
	return atomic_notifier_chain_register(&g_chg_changed_nh, nb);
}
EXPORT_SYMBOL_GPL(oplus_chg_reg_changed_notifier);

void oplus_chg_unreg_changed_notifier(struct notifier_block *nb)
{
	atomic_notifier_chain_unregister(&g_chg_changed_nh, nb);
}
EXPORT_SYMBOL_GPL(oplus_chg_unreg_changed_notifier);

int oplus_chg_reg_event_notifier(struct notifier_block *nb)
{
	return atomic_notifier_chain_register(&g_chg_event_nh, nb);
}
EXPORT_SYMBOL_GPL(oplus_chg_reg_event_notifier);

void oplus_chg_unreg_event_notifier(struct notifier_block *nb)
{
	atomic_notifier_chain_unregister(&g_chg_event_nh, nb);
}
EXPORT_SYMBOL_GPL(oplus_chg_unreg_event_notifier);

int oplus_chg_reg_mod_notifier(struct oplus_chg_mod *ocm,
			       struct notifier_block *nb)
{
	if (!ocm)
		return -EINVAL;

	return atomic_notifier_chain_register(
		&oplus_chg_changed_notifier, nb);
}
EXPORT_SYMBOL_GPL(oplus_chg_reg_mod_notifier);

void oplus_chg_unreg_mod_notifier(struct oplus_chg_mod *ocm,
				  struct notifier_block *nb)
{
	if (!ocm)
		return;

	atomic_notifier_chain_unregister(
		&oplus_chg_changed_notifier, nb);
}
EXPORT_SYMBOL_GPL(oplus_chg_unreg_mod_notifier);

/* ===== Event dispatch ===== */

void oplus_chg_mod_changed(struct oplus_chg_mod *ocm)
{
	if (!ocm || !ocm->initialized)
		return;

	atomic_notifier_call_chain(&oplus_chg_changed_notifier, 0, ocm);
}
EXPORT_SYMBOL_GPL(oplus_chg_mod_changed);

void oplus_chg_global_event(struct oplus_chg_mod *owner_ocm,
			    enum oplus_chg_event events)
{
	atomic_notifier_call_chain(&g_chg_event_nh, events, owner_ocm);
}
EXPORT_SYMBOL_GPL(oplus_chg_global_event);

int oplus_chg_mod_event(struct oplus_chg_mod *ocm_receive,
			struct oplus_chg_mod *ocm_send,
			enum oplus_chg_event events)
{
	int rc;

	rc = atomic_notifier_call_chain(&oplus_chg_event_notifier,
					events, ocm_send);
	return notifier_to_errno(rc);
}
EXPORT_SYMBOL_GPL(oplus_chg_mod_event);

int oplus_chg_anon_mod_event(struct oplus_chg_mod *ocm_receive,
			     enum oplus_chg_event events)
{
	return oplus_chg_mod_event(ocm_receive, NULL, events);
}
EXPORT_SYMBOL_GPL(oplus_chg_anon_mod_event);

MODULE_DESCRIPTION("OPLUS charging core (mod registration skeleton)");
MODULE_LICENSE("GPL v2");
