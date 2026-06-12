/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020-2022 Oplus. All rights reserved.
 */

#ifndef _OPLUS_CHG_COMM_H
#define _OPLUS_CHG_COMM_H

#include <linux/kernel.h>

enum oplus_chg_comm_topic {
	MSG_TOPIC_DEFAULT,
	MSG_TOPIC_UART_TX,
	MSG_TOPIC_UART_RX,
	MSG_TOPIC_SPI_TX,
	MSG_TOPIC_SPI_RX,
};

struct oplus_chg_comm_config {
	int polling_mode;
	unsigned long timeout_ms;
	int retry_limit;
};

extern int oplus_chg_comm_register(void);
extern void oplus_chg_comm_unregister(void);

#endif /* _OPLUS_CHG_COMM_H */
