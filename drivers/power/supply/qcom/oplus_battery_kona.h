/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _OPLUS_BATTERY_KONA_H_
#define _OPLUS_BATTERY_KONA_H_

#include <linux/gpio/consumer.h>
#include <linux/pinctrl/consumer.h>

struct device;
struct smb_charger;

/**
 * struct oplus_kona_gpio - GPIO/pinctrl state for OPLUS Kona charger adapter.
 *
 * Ported from the 4.19 smb_charger struct GPIO fields (oplus_battery_msm8250.h)
 * with API migration: int gpio numbers replaced by struct gpio_desc *.
 *
 * Each GPIO category bundles its gpio_desc, pinctrl handle, and the relevant
 * pinctrl states (active/sleep/default).  Interrupt numbers are derived via
 * gpiod_to_irq() at init time.
 */
struct oplus_kona_gpio {
	/* --- CC detect (charger cable detect) --- */
	struct gpio_desc	*ccdetect_gpio;
	int			ccdetect_irq;
	struct pinctrl		*ccdetect_pinctrl;
	struct pinctrl_state	*ccdetect_active;
	struct pinctrl_state	*ccdetect_sleep;

	/* --- USB temperature ADC pinmux (3 separate GPIOs) --- */
	struct pinctrl		*usbtemp_gpio1_adc_pinctrl;
	struct pinctrl_state	*usbtemp_gpio1_default;
	struct pinctrl		*usbtemp_gpio8_adc_pinctrl;
	struct pinctrl_state	*usbtemp_gpio8_default;
	struct pinctrl		*usbtemp_gpio5_adc_pinctrl;
	struct pinctrl_state	*usbtemp_gpio5_default;

	/* --- Ship-mode ID --- */
	struct gpio_desc	*shipmode_id_gpio;
	struct pinctrl		*shipmode_id_pinctrl;
	struct pinctrl_state	*shipmode_id_active;

	/* --- Wired connector detect --- */
	struct gpio_desc	*wired_conn_gpio;
	int			wired_conn_irq;
	struct pinctrl		*wired_conn_pinctrl;
	struct pinctrl_state	*wired_conn_active;
	struct pinctrl_state	*wired_conn_sleep;

	/* --- OTG enable --- */
	struct gpio_desc	*otg_en_gpio;
	struct pinctrl		*otg_en_pinctrl;
	struct pinctrl_state	*otg_en_active;
	struct pinctrl_state	*otg_en_sleep;
	struct pinctrl_state	*otg_en_default;

	/* --- IDT enable (wireless charger) --- */
	struct gpio_desc	*idt_en_gpio;
	struct pinctrl		*idt_en_pinctrl;
	struct pinctrl_state	*idt_en_active;
	struct pinctrl_state	*idt_en_sleep;
	struct pinctrl_state	*idt_en_default;

	/* --- WRX enable (wireless receiver) --- */
	struct gpio_desc	*wrx_en_gpio;
	struct pinctrl		*wrx_en_pinctrl;
	struct pinctrl_state	*wrx_en_active;
	struct pinctrl_state	*wrx_en_sleep;
	struct pinctrl_state	*wrx_en_default;

	/* --- WRX OTG (wireless receiver OTG) --- */
	struct gpio_desc	*wrx_otg_gpio;
	struct pinctrl		*wrx_otg_pinctrl;
	struct pinctrl_state	*wrx_otg_active;
	struct pinctrl_state	*wrx_otg_sleep;
};

#if IS_ENABLED(CONFIG_OPLUS_CHARGER_IC_KONA)
int oplus_battery_kona_init(struct device *dev, struct smb_charger *chg);
#else
static inline int oplus_battery_kona_init(struct device *dev,
					  struct smb_charger *chg)
{
	return 0;
}
#endif

#endif /* _OPLUS_BATTERY_KONA_H_ */
