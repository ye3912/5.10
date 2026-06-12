/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * oplus_logbuf.h - Printk ring buffer in reserved memory for boot-hang debug
 *
 * Provides a zero-dependency console ring buffer that survives warm reset,
 * plus a /proc/oplus_logbuf read interface and a hung_task dump hook.
 * No dependency on UART, pstore, filesystem, or OEM debug.
 *
 * Copyright (c) 2025, OPLUS SM8250 5.10 port
 */

#ifndef _OPLUS_LOGBUF_H
#define _OPLUS_LOGBUF_H

#include <linux/types.h>

#define OPLUS_LOGBUF_MAGIC  0x4F4C4201  /* 'OLB' + version 1 */

/**
 * struct oplus_logbuf_hdr - Header for the reserved-memory ring buffer
 * @magic:  Must equal OPLUS_LOGBUF_MAGIC (validates warm-boot data)
 * @head:   Write cursor (byte offset into data[])
 * @size:   Total size of data[] in bytes
 * @lines:  Monotonic line counter (incremented on '\n')
 * @data:   Ring buffer byte array
 */
struct oplus_logbuf_hdr {
	uint32_t magic;
	uint32_t head;
	uint32_t size;
	uint32_t lines;
	char data[];
};

#ifdef CONFIG_OPLUS_LOGBUF

extern struct oplus_logbuf_hdr *oplus_logbuf;

int oplus_logbuf_init(phys_addr_t phys, size_t size);
void oplus_logbuf_dump_to_kmsg(void);

#ifdef CONFIG_OPLUS_SCREENLOG
void oplus_screenlog_refresh(void);
#else
static inline void oplus_screenlog_refresh(void) { }
#endif

#else /* !CONFIG_OPLUS_LOGBUF */

#define oplus_logbuf ((struct oplus_logbuf_hdr *)NULL)

static inline int oplus_logbuf_init(phys_addr_t phys, size_t size)
{
	return -ENODEV;
}

static inline void oplus_logbuf_dump_to_kmsg(void) { }

#endif /* CONFIG_OPLUS_LOGBUF */

#endif /* _OPLUS_LOGBUF_H */
