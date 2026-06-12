// SPDX-License-Identifier: GPL-2.0-only
/*
 * oplus_logbuf.c — Printk ring buffer in no-map reserved memory
 *
 * Registers a struct console that copies every printk byte into a
 * ring buffer located in a device-tree reserved-memory region.
 * Provides /proc/oplus_logbuf for reading the buffer after boot,
 * and oplus_logbuf_dump_to_kmsg() for dumping to dmesg on hung_task.
 *
 * Design goals:
 *   1. Zero dependency on filesystem, UART, pstore, or OEM debug.
 *   2. Works as soon as the reserved-memory node is mapped.
 *   3. Survives warm reset: magic check preserves prior-boot data.
 *   4. hung_task hook dumps the buffer to kmsg on detection.
 *
 * DTS binding (example):
 *   reserved-memory {
 *       oplus_logbuf_mem: logbuf@B0000000 {
 *           compatible = "oplus,logbuf";
 *           reg = <0x0 0xB0000000 0x0 0x400000>;
 *           no-map;
 *       };
 *   };
 *
 * Copyright (c) 2025, OPLUS SM8250 5.10 port
 */

#include <linux/console.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/oplus_logbuf.h>
#include <linux/printk.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>

static phys_addr_t logbuf_phys;
static size_t logbuf_size;

struct oplus_logbuf_hdr *oplus_logbuf;
EXPORT_SYMBOL_GPL(oplus_logbuf);

/* Protect ring buffer head/lines from concurrent console writes */
static DEFINE_RAW_SPINLOCK(logbuf_lock);

/**
 * oplus_logbuf_init() - Map the reserved-memory region and init the header
 * @phys: Physical base address (from DTS reg property)
 * @size: Region size in bytes
 *
 * On first boot (magic != OPLUS_LOGBUF_MAGIC): zero the header, set magic.
 * On warm boot (magic matches): validate existing data, preserve it.
 *
 * Return: 0 on success, negative errno on failure.
 */
int oplus_logbuf_init(phys_addr_t phys, size_t size)
{
	struct oplus_logbuf_hdr *hdr;
	size_t data_size;

	if (size < SZ_64K || size > SZ_16M) {
		pr_err("oplus_logbuf: invalid size %zu (must be 64K..16M)\n", size);
		return -EINVAL;
	}

	if (sizeof(*hdr) >= size) {
		pr_err("oplus_logbuf: region too small for header\n");
		return -EINVAL;
	}

	data_size = size - sizeof(*hdr);

	hdr = memremap(phys, size, MEMREMAP_WB);
	if (!hdr) {
		pr_err("oplus_logbuf: memremap(%pa, %zu) failed\n", &phys, size);
		return -ENOMEM;
	}

	if (hdr->magic != OPLUS_LOGBUF_MAGIC) {
		/* First boot or cold reset: initialise */
		memset(hdr, 0, sizeof(*hdr));
		hdr->magic = OPLUS_LOGBUF_MAGIC;
		hdr->size = data_size;
		hdr->head = 0;
		hdr->lines = 0;
		pr_info("oplus_logbuf: cold init at %pa, data %zu bytes\n",
			&phys, data_size);
	} else {
		/* Warm boot: validate and preserve */
		if (hdr->size != data_size) {
			pr_info("oplus_logbuf: size changed %u→%zu, reinit\n",
				hdr->size, data_size);
			hdr->size = data_size;
		}
		if (hdr->head >= data_size)
			hdr->head = 0;
		pr_info("oplus_logbuf: warm init at %pa, head=%u lines=%u\n",
			&phys, hdr->head, hdr->lines);
	}

	oplus_logbuf = hdr;
	logbuf_phys = phys;
	logbuf_size = size;

	return 0;
}

/* ─── console write callback ──────────────────────────────────── */

/**
 * oplus_logbuf_console_write() - Copy printk output into the ring buffer
 * @con:   Console structure (unused)
 * @s:     String to write
 * @count: Number of bytes
 *
 * Called for every character written to the kernel console.
 * Uses raw_spinlock to protect against concurrent writes from
 * multiple CPUs.  Safe in any context including NMI.
 */
static void oplus_logbuf_console_write(struct console *con,
				       const char *s, unsigned int count)
{
	struct oplus_logbuf_hdr *hdr = oplus_logbuf;
	unsigned long flags;
	unsigned int i;

	if (!hdr)
		return;

	raw_spin_lock_irqsave(&logbuf_lock, flags);
	for (i = 0; i < count; i++) {
		hdr->data[hdr->head] = s[i];
		hdr->head = (hdr->head + 1) % hdr->size;
		if (s[i] == '\n')
			hdr->lines++;
	}
	raw_spin_unlock_irqrestore(&logbuf_lock, flags);
}

static struct console oplus_logbuf_console = {
	.name	= "oplus_logbuf",
	.write	= oplus_logbuf_console_write,
	.flags	= CON_PRINTBUFFER | CON_ENABLED,
	.index	= -1,
};

/* ─── oplus_logbuf_dump_to_kmsg() ──────────────────────────────── */

/**
 * oplus_logbuf_dump_to_kmsg() - Dump the ring buffer to printk
 *
 * Linearises the ring buffer into a temporary buffer and prints it
 * via printk(KERN_EMERG ...).  Call this from hung_task detector
 * or panic path so the captured history appears in dmesg.
 *
 * Temporarily unregisters the logbuf console to prevent recursive
 * writes back into the ring buffer during the dump.
 *
 * Uses GFP_KERNEL — caller must be in process context.
 */
void oplus_logbuf_dump_to_kmsg(void)
{
	struct oplus_logbuf_hdr *hdr = oplus_logbuf;
	unsigned int total, pos, i;
	char *buf;

	if (!hdr || !hdr->size)
		return;

	total = hdr->size;

	buf = kmalloc(total + 1, GFP_KERNEL);
	if (!buf) {
		pr_emerg("oplus_logbuf: cannot alloc %u bytes for dump\n",
			 total);
		return;
	}

	/* Linearise: oldest byte is (head+1) % size */
	pos = (hdr->head + 1) % total;
	for (i = 0; i < total; i++) {
		buf[i] = hdr->data[pos];
		pos = (pos + 1) % total;
	}
	buf[total] = '\0';

	/*
	 * Unregister our console before printing to prevent the printk
	 * from writing back into the ring buffer (recursion).
	 */
	unregister_console(&oplus_logbuf_console);

	pr_emerg("=== oplus_logbuf begin (%u lines) ===\n", hdr->lines);
	printk(KERN_EMERG "%s", buf);
	pr_emerg("=== oplus_logbuf end ===\n");

	register_console(&oplus_logbuf_console);

	kfree(buf);
}
EXPORT_SYMBOL_GPL(oplus_logbuf_dump_to_kmsg);

/* ─── procfs: /proc/oplus_logbuf ───────────────────────────────── */

static int oplus_logbuf_proc_show(struct seq_file *m, void *v)
{
	struct oplus_logbuf_hdr *hdr = oplus_logbuf;
	unsigned int total, pos, i;
	char *buf;

	if (!hdr || !hdr->size) {
		seq_puts(m, "(logbuf not initialised)\n");
		return 0;
	}

	total = hdr->size;

	buf = kmalloc(total + 1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	pos = (hdr->head + 1) % total;
	for (i = 0; i < total; i++) {
		buf[i] = hdr->data[pos];
		pos = (pos + 1) % total;
	}
	buf[total] = '\0';

	seq_puts(m, buf);

	kfree(buf);
	return 0;
}

static int oplus_logbuf_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, oplus_logbuf_proc_show, NULL);
}

static const struct proc_ops oplus_logbuf_proc_ops = {
	.proc_open	= oplus_logbuf_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

/* ─── helper: look up DT reserved-memory node ──────────────────── */

static int __init oplus_logbuf_dt_init(void)
{
	struct device_node *np;
	struct resource res;
	resource_size_t size;
	int ret;

	np = of_find_compatible_node(NULL, NULL, "oplus,logbuf");
	if (!np) {
		pr_info("oplus_logbuf: no DT node with compatible=\"oplus,logbuf\","
			" skipping\n");
		return 0;
	}

	if (!of_device_is_available(np)) {
		pr_info("oplus_logbuf: DT node disabled, skipping\n");
		of_node_put(np);
		return 0;
	}

	ret = of_address_to_resource(np, 0, &res);
	of_node_put(np);

	if (ret) {
		pr_err("oplus_logbuf: failed to read reg from DT: %d\n", ret);
		return 0;
	}

	size = resource_size(&res);

	pr_info("oplus_logbuf: DT node found at %pa size %pa\n",
		&res.start, &size);

	return oplus_logbuf_init(res.start, size);
}

/* ─── module init ──────────────────────────────────────────────── */

static int __init oplus_logbuf_module_init(void)
{
	int ret;

	/*
	 * DT-backed init.  If the node is absent the logbuf remains NULL —
	 * the console write callback and dump are both safe with NULL.
	 */
	ret = oplus_logbuf_dt_init();
	if (ret)
		pr_err("oplus_logbuf: DT init failed: %d\n", ret);

	register_console(&oplus_logbuf_console);

	if (!proc_create("oplus_logbuf", 0444, NULL, &oplus_logbuf_proc_ops))
		pr_warn("oplus_logbuf: failed to create /proc/oplus_logbuf\n");

	return 0;
}

/*
 * Run early so the console is registered before most driver probe output,
 * but after memremap is available.
 */
subsys_initcall(oplus_logbuf_module_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Printk ring buffer in reserved memory for boot-hang debug");
