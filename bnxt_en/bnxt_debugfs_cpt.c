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

static struct dentry *debugfs_dim_ring_init(struct dim *dim, int ring_idx,
					    struct dentry *dd)
{
	static char qname[16];

	snprintf(qname, 10, "%d", ring_idx);
	return debugfs_create_file(qname, 0600, dd,
				   dim, &debugfs_dim_fops);
}

void bnxt_debug_dev_init(struct bnxt *bp)
{
	const char *pname = pci_name(bp->pdev);
	struct dentry *pdevf;
	int i;

	bp->debugfs_pdev = debugfs_create_dir(pname, bnxt_debug_mnt);
	if (bp->debugfs_pdev) {
		pdevf = debugfs_create_dir("dim", bp->debugfs_pdev);
		if (!pdevf) {
			pr_err("failed to create debugfs entry %s/dim\n",
			       pname);
			return;
		}
		bp->debugfs_dim = pdevf;
		/* create files for each rx ring */
		for (i = 0; i < bp->cp_nr_rings; i++) {
			struct bnxt_cp_ring_info *cpr = &bp->bnapi[i]->cp_ring;

			if (cpr && bp->bnapi[i]->rx_ring) {
				pdevf = debugfs_dim_ring_init(&cpr->dim, i,
							      bp->debugfs_dim);
				if (!pdevf)
					pr_err("failed to create debugfs entry %s/dim/%d\n",
					       pname, i);
			}
		}
	} else {
		pr_err("failed to create debugfs entry %s\n", pname);
	}
}

void bnxt_debug_dev_exit(struct bnxt *bp)
{
	if (bp) {
		debugfs_remove_recursive(bp->debugfs_pdev);
		bp->debugfs_pdev = NULL;
	}
}

void bnxt_debug_init(void)
{
	bnxt_debug_mnt = debugfs_create_dir("bnxt_en", NULL);
	if (!bnxt_debug_mnt)
		pr_err("failed to init bnxt_en debugfs\n");
}

void bnxt_debug_exit(void)
{
	debugfs_remove_recursive(bnxt_debug_mnt);
}

#endif /* CONFIG_DEBUG_FS */
