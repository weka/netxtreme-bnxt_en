/* Broadcom NetXtreme-C/E network driver.
 *
 * Copyright (c) 2017-2018 Broadcom Limited
 * Copyright (c) 2018-2019 Broadcom Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation.
 */

#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/pci.h>
#include "bnxt_hsi.h"
#include "bnxt_compat.h"
#ifdef HAVE_DIM
#include <linux/dim.h>
#else
#include "bnxt_dim.h"
#endif
#include "bnxt.h"

#ifdef CONFIG_DEBUG_FS

static struct dentry *bnxt_debug_mnt;

static ssize_t debugfs_dim_read(struct file *filep,
				char __user *buffer,
				size_t count, loff_t *ppos)
{
	struct dim *dim = filep->private_data;
	int len;
	char *buf;

	if (*ppos)
		return 0;
	if (!dim)
		return -ENODEV;
	buf = kasprintf(GFP_KERNEL,
			"state = %d\n" \
			"profile_ix = %d\n" \
			"mode = %d\n" \
			"tune_state = %d\n" \
			"steps_right = %d\n" \
			"steps_left = %d\n" \
			"tired = %d\n",
			dim->state,
			dim->profile_ix,
			dim->mode,
			dim->tune_state,
			dim->steps_right,
			dim->steps_left,
			dim->tired);
	if (!buf)
		return -ENOMEM;
	if (count < strlen(buf)) {
		kfree(buf);
		return -ENOSPC;
	}
	len = simple_read_from_buffer(buffer, count, ppos, buf, strlen(buf));
	kfree(buf);
	return len;
}

static const struct file_operations debugfs_dim_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = debugfs_dim_read,
};

static void debugfs_dim_ring_init(struct dim *dim, int ring_idx,
				  struct dentry *dd)
{
	static char qname[16];

	snprintf(qname, 10, "%d", ring_idx);
	debugfs_create_file(qname, 0600, dd, dim, &debugfs_dim_fops);
}

static int dbr_enable_get(void *data, u64 *val)
{
	struct bnxt *bp = data;

	*val = bp->dbr.enable;
	return 0;
}

static int dbr_enable_set(void *data, u64 val)
{
	struct bnxt *bp = data;
	struct bnxt_dbr *dbr;
	int rc;

	dbr = &bp->dbr;

	if (val) {
		dbr->enable = 1;
		rc = bnxt_dbr_init(bp);
		if (rc) {
			netdev_err(bp->dev,
				   "Failed to initialize DB recovery\n");
			dbr->enable = 0;
			return rc;
		}
	} else {
		dbr->enable = 0;
		bnxt_dbr_exit(bp);
	}

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(dbr_enable_fops, dbr_enable_get, dbr_enable_set,
			 "%llu\n");

static ssize_t dbr_stats_read(struct file *filep, char __user *buffer,
size_t count, loff_t *ppos)
{
	struct bnxt_dbr_sw_stats *stat = filep->private_data;
	char *buf;
	int len;

	if (*ppos)
		return 0;

	buf = kasprintf(GFP_KERNEL, "nr_dbr = %u\n" "avg_dbr_us = %llu\n" \
			"max_dbr_us = %llu\n" "min_dbr_us = %llu\n",
			stat->nr_dbr, stat->avg_dbr_us, stat->max_dbr_us,
			stat->min_dbr_us);
	if (!buf)
		return -ENOMEM;

	if (count < strlen(buf)) {
		kfree(buf);
		return -ENOSPC;
	}

	len = simple_read_from_buffer(buffer, count, ppos, buf, strlen(buf));
	kfree(buf);

	return len;
}

static const struct file_operations dbr_stats_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = dbr_stats_read,
};

static int dbr_test_recover_enable_get(void *data, u64 *val)
{
	struct bnxt *bp = data;

	*val = bp->dbr.debug.recover_enable;
	return 0;
}

static int dbr_test_recover_enable_set(void *data, u64 val)
{
	struct bnxt_dbr_debug *debug;
	struct bnxt *bp = data;
	struct bnxt_dbr *dbr;

	dbr = &bp->dbr;
	debug = &dbr->debug;

	if (!dbr->enable && val) {
		netdev_err(bp->dev,
			   "Unable to run DB recovery test when DBR is disabled\n");
		return -EINVAL;
	}

	if (val) {
		debug->recover_enable = 1;
		if (dbr->wq)
			/* kick start the recovery work */
			if (queue_delayed_work(dbr->wq, &dbr->dwork,
					       msecs_to_jiffies(debug->recover_interval_ms)))
			    atomic_inc(&dbr->event_cnt);
	} else {
		debug->recover_enable = 0;
	}

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(dbr_test_recover_enable_fops,
			 dbr_test_recover_enable_get,
			 dbr_test_recover_enable_set,
			 "%llu\n");

void bnxt_debug_dev_init(struct bnxt *bp)
{
	const char *pname = pci_name(bp->pdev);
	struct bnxt_dbr_debug *debug;
	struct bnxt_dbr *dbr;
	struct dentry *dir;
	int i;

	bp->debugfs_pdev = debugfs_create_dir(pname, bnxt_debug_mnt);
	dir = debugfs_create_dir("dim", bp->debugfs_pdev);

	/* create files for each rx ring */
	for (i = 0; i < bp->cp_nr_rings; i++) {
		struct bnxt_cp_ring_info *cpr = &bp->bnapi[i]->cp_ring;

		if (cpr && bp->bnapi[i]->rx_ring)
			debugfs_dim_ring_init(&cpr->dim, i, dir);
	}

#define DBR_TEST_RECOVER_INTERVAL_MS 1000
#define DBR_TEST_DROP_RATIO          10
	dbr = &bp->dbr;
	debug = &bp->dbr.debug;

	debug->recover_interval_ms = DBR_TEST_RECOVER_INTERVAL_MS;
	debug->drop_ratio = DBR_TEST_DROP_RATIO;

	dir = debugfs_create_dir("dbr", bp->debugfs_pdev);
	debugfs_create_file("dbr_enable", 0644, dir, bp, &dbr_enable_fops);
	debugfs_create_file("dbr_stats", 0444, dir, &dbr->sw_stats,
			    &dbr_stats_fops);
#ifdef DBR_DBG_DROP_ENABLE
	debugfs_create_u8("dbr_test_drop_enable", 0644, dir,
			  &debug->drop_enable);
	debugfs_create_u32("dbr_test_drop_ratio", 0644, dir,
			   &debug->drop_ratio);
#endif
	debugfs_create_file("dbr_test_recover_enable", 0644, dir, bp,
			    &dbr_test_recover_enable_fops);
	debugfs_create_u32("dbr_test_recover_interval_ms", 0644, dir,
			   &debug->recover_interval_ms);
}

void bnxt_debug_dev_exit(struct bnxt *bp)
{
	if (bp) {
		struct bnxt_dbr_debug *debug = &bp->dbr.debug;

		memset(debug, 0, sizeof(*debug));
		debugfs_remove_recursive(bp->debugfs_pdev);
		bp->debugfs_pdev = NULL;
	}
}

void bnxt_debug_init(void)
{
	bnxt_debug_mnt = debugfs_create_dir("bnxt_en", NULL);
}

void bnxt_debug_exit(void)
{
	debugfs_remove_recursive(bnxt_debug_mnt);
}

#endif /* CONFIG_DEBUG_FS */
