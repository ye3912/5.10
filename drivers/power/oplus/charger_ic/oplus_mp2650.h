/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2018-2020 Oplus. All rights reserved.
 *
 * MPS MP2650 charger IC minimal header for SM8250 5.10 GKI.
 */

#ifndef __OPLUS_MP2650_H__
#define __OPLUS_MP2650_H__

/* Register definitions ported from 4.19 — used by full driver in Phase 4. */
#define MP2650_REG_CHG_CURRENT		0x00
#define MP2650_REG_INPUT_CURRENT	0x01
#define MP2650_REG_CHG_VOLTAGE		0x02
#define MP2650_REG_STATUS		0x03
#define MP2650_REG_FAULT		0x04
#define MP2650_REG_ADC_VBUS		0x05
#define MP2650_REG_ADC_IBUS		0x06
#define MP2650_REG_ADC_VBAT		0x07
#define MP2650_REG_GPIO_CONTROL		0x10
#define MP2650_GPIO_OTG_EN		1
#define MP2650_GPIO_OTG_DIS		0

#endif /* __OPLUS_MP2650_H__ */
