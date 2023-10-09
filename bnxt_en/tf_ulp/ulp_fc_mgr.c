// SPDX-License-Identifier: BSD-3-Clause
/* Copyright(c) 2019-2021 Broadcom
 * All rights reserved.
 */

#include "bnxt_compat.h"
#include "bnxt_hsi.h"
#include "bnxt.h"
#include "bnxt_tf_ulp.h"
#include "bnxt_tf_common.h"
#include "ulp_fc_mgr.h"
#include "ulp_flow_db.h"
#include "ulp_template_db_enum.h"
#include "ulp_template_struct.h"
#include "tf_tbl.h"

#if defined(CONFIG_BNXT_FLOWER_OFFLOAD) || defined(CONFIG_BNXT_CUSTOM_FLOWER_OFFLOAD)
static int
ulp_fc_mgr_shadow_mem_alloc(struct bnxt_ulp_context *ulp_ctx,
			    struct hw_fc_mem_info *parms, int size)
{
	/* Allocate memory*/
	if (!parms)
		return -EINVAL;

	parms->mem_va = kzalloc(L1_CACHE_ALIGN(size), GFP_KERNEL);
	if (!parms->mem_va)
		return -ENOMEM;

	parms->mem_pa = (void *)__pa(parms->mem_va);
	return 0;
}

static void
ulp_fc_mgr_shadow_mem_free(struct hw_fc_mem_info *parms)
{
	kfree(parms->mem_va);
}

/**
 * Allocate and Initialize all Flow Counter Manager resources for this ulp
 * context.
 *
 * @ctxt: The ulp context for the Flow Counter manager.
 *
 */
int
ulp_fc_mgr_init(struct bnxt_ulp_context *ctxt)
{
	u32 dev_id, sw_acc_cntr_tbl_sz, hw_fc_mem_info_sz;
	struct bnxt_ulp_device_params *dparms;
	struct bnxt_ulp_fc_info *ulp_fc_info;
	int i, rc;

	if (!ctxt)
		return -EINVAL;

	dev_id = bnxt_ulp_devid_get(ctxt->bp);

	dparms = bnxt_ulp_device_params_get(dev_id);
	if (!dparms) {
		netdev_dbg(ctxt->bp->dev, "Failed to device parms\n");
		return -EINVAL;
	}

	ulp_fc_info = kzalloc(sizeof(*ulp_fc_info), GFP_KERNEL);
	if (!ulp_fc_info)
		goto error;

	mutex_init(&ulp_fc_info->fc_lock);

	/* Add the FC info tbl to the ulp context. */
	bnxt_ulp_cntxt_ptr2_fc_info_set(ctxt, ulp_fc_info);

	ulp_fc_info->num_counters = dparms->flow_count_db_entries;
	if (!ulp_fc_info->num_counters) {
		/* No need for software counters, call fw directly */
		netdev_dbg(ctxt->bp->dev, "Sw flow counter support not enabled\n");
		return 0;
	}

	sw_acc_cntr_tbl_sz = sizeof(struct sw_acc_counter) *
				dparms->flow_count_db_entries;

	for (i = 0; i < TF_DIR_MAX; i++) {
		ulp_fc_info->sw_acc_tbl[i] = kzalloc(sw_acc_cntr_tbl_sz,
						     GFP_KERNEL);
		if (!ulp_fc_info->sw_acc_tbl[i])
			goto error;
	}

	hw_fc_mem_info_sz = sizeof(u64) * dparms->flow_count_db_entries;

	for (i = 0; i < TF_DIR_MAX; i++) {
		rc = ulp_fc_mgr_shadow_mem_alloc(ctxt,
						 &ulp_fc_info->shadow_hw_tbl[i],
						 hw_fc_mem_info_sz);
		if (rc)
			goto error;
	}

	ulp_fc_mgr_thread_start(ctxt);

	return 0;

error:
	ulp_fc_mgr_deinit(ctxt);

	return -ENOMEM;
}

/**
 * Release all resources in the Flow Counter Manager for this ulp context
 *
 * @ctxt: The ulp context for the Flow Counter manager
 *
 */
int
ulp_fc_mgr_deinit(struct bnxt_ulp_context *ctxt)
{
	struct bnxt_ulp_fc_info *ulp_fc_info;
	struct hw_fc_mem_info *shd_info;
	int i;

	ulp_fc_info = bnxt_ulp_cntxt_ptr2_fc_info_get(ctxt);

	if (!ulp_fc_info)
		return -EINVAL;

	ulp_fc_mgr_thread_cancel(ctxt);

	if (ulp_fc_info->num_counters) {
		for (i = 0; i < TF_DIR_MAX; i++)
			kfree(ulp_fc_info->sw_acc_tbl[i]);

		for (i = 0; i < TF_DIR_MAX; i++) {
			shd_info = &ulp_fc_info->shadow_hw_tbl[i];
			ulp_fc_mgr_shadow_mem_free(shd_info);
		}
	}

	kfree(ulp_fc_info);

	/* Safe to ignore on deinit */
	(void)bnxt_ulp_cntxt_ptr2_fc_info_set(ctxt, NULL);

	return 0;
}

/**
 * Check if the alarm thread that walks through the flows is started
 *
 * @ctxt: The ulp context for the flow counter manager
 *
 */
bool ulp_fc_mgr_thread_isstarted(struct bnxt_ulp_context *ctxt)
{
	struct bnxt_ulp_fc_info *ulp_fc_info;

	ulp_fc_info = bnxt_ulp_cntxt_ptr2_fc_info_get(ctxt);

	if (ulp_fc_info)
		return !!(ulp_fc_info->flags & ULP_FLAG_FC_THREAD);

	return false;
}

/**
 * Setup the Flow counter timer thread that will fetch/accumulate raw counter
 * data from the chip's internal flow counters
 *
 * @ctxt: The ulp context for the flow counter manager
 *
 */
void
ulp_fc_mgr_thread_start(struct bnxt_ulp_context *ctxt)
{
	struct delayed_work *work = &ctxt->cfg_data->fc_work;

	INIT_DELAYED_WORK(work, ulp_fc_mgr_alarm_cb);
	schedule_delayed_work(work, msecs_to_jiffies(1000));
}

/**
 * Cancel the alarm handler
 *
 * @ctxt: The ulp context for the flow counter manager
 *
 */
void ulp_fc_mgr_thread_cancel(struct bnxt_ulp_context *ctxt)
{
	cancel_delayed_work_sync(&ctxt->cfg_data->fc_work);
}

static int
ulp_tf_fc_tf_flow_stat_get(struct bnxt_ulp_context *ctxt,
			   struct ulp_flow_db_res_params *res,
			   u64 *packets, u64 *bytes)
{
	struct tf_get_tbl_entry_parms parms = { 0 };
	struct bnxt_ulp_device_params *dparms;
	struct bnxt *bp;
	u32 dev_id = 0;
	struct tf *tfp;
	u64 stats = 0;
	int rc = 0;

	tfp = bnxt_ulp_cntxt_tfp_get(ctxt,
				     ulp_flow_db_shared_session_get(res));
	if (!tfp)
		return -EINVAL;

	bp = tfp->bp;

	dev_id = bnxt_ulp_devid_get(bp);

	dparms = bnxt_ulp_device_params_get(dev_id);
	if (!dparms) {
		netdev_dbg(bp->dev, "Failed to device parms\n");
		return -EINVAL;
	}
	parms.dir = res->direction;
	parms.type = TF_TBL_TYPE_ACT_STATS_64;
	parms.idx = res->resource_hndl;
	parms.data_sz_in_bytes = sizeof(u64);
	parms.data = (u8 *)&stats;
	rc = tf_get_tbl_entry(tfp, &parms);
	if (rc) {
		netdev_dbg(bp->dev,
			   "Get failed for id:0x%x rc:%d\n",
			   parms.idx, rc);
		return rc;
	}

	*packets = FLOW_CNTR_PKTS(stats, dparms);
	*bytes = FLOW_CNTR_BYTES(stats, dparms);

	return rc;
}

static int ulp_get_single_flow_stat(struct bnxt_ulp_context *ctxt,
				    struct tf *tfp,
				    struct bnxt_ulp_fc_info *fc_info,
				    enum tf_dir dir,
				    u32 hw_cntr_id,
				    struct bnxt_ulp_device_params *dparms)
{
	struct sw_acc_counter *sw_acc_tbl_entry = NULL;
	struct tf_get_tbl_entry_parms parms = { 0 };
	struct bnxt *bp = tfp->bp;
	u32 sw_cntr_indx = 0;
	u64 stats = 0;
	int rc = 0;

	parms.dir = dir;
	parms.type = TF_TBL_TYPE_ACT_STATS_64;
	parms.idx = hw_cntr_id;
	/* TODO:
	 * Size of an entry needs to obtained from template
	 */
	parms.data_sz_in_bytes = sizeof(u64);
	parms.data = (u8 *)&stats;
	rc = tf_get_tbl_entry(tfp, &parms);
	if (rc) {
		netdev_dbg(bp->dev,
			   "Get failed for id:0x%x rc:%d\n",
			   parms.idx, rc);
		return rc;
	}

	/* TBD - Get PKT/BYTE COUNT SHIFT/MASK from Template */
	sw_cntr_indx = hw_cntr_id - fc_info->shadow_hw_tbl[dir].start_idx;
	sw_acc_tbl_entry = &fc_info->sw_acc_tbl[dir][sw_cntr_indx];
	/* Some applications may accumulate the flow counters while some
	 * may not. In cases where the application is accumulating the counters
	 * the PMD need not do the accumulation itself and viceversa to report
	 * the correct flow counters.
	 */
	sw_acc_tbl_entry->pkt_count += FLOW_CNTR_PKTS(stats, dparms);
	sw_acc_tbl_entry->byte_count += FLOW_CNTR_BYTES(stats, dparms);

	return rc;
}

/**
 * Alarm handler that will issue the TF-Core API to fetch
 * data from the chip's internal flow counters
 *
 * @ctxt: The ulp context for the flow counter manager
 *
 */

void
ulp_fc_mgr_alarm_cb(struct work_struct *work)
{
	u32 dev_id, hw_cntr_id = 0, num_entries = 0;
	struct bnxt_ulp_device_params *dparms;
	struct bnxt_ulp_fc_info *ulp_fc_info;
	struct delayed_work *fc_work = NULL;
	struct bnxt_ulp_data *cfg_data;
	struct bnxt_ulp_context *ctxt;
	struct bnxt *bp;
	struct tf *tfp;
	unsigned int j;
	enum tf_dir i;
	int rc = 0;

	cfg_data = container_of(work, struct bnxt_ulp_data, fc_work.work);
	fc_work = &cfg_data->fc_work;

	bnxt_ulp_cntxt_lock_acquire();
	ctxt = bnxt_ulp_cntxt_entry_lookup(cfg_data);
	if (!ctxt)
		goto err;
	if (!ctxt->cfg_data)
		goto err;

	bp = ctxt->bp;

	ulp_fc_info = bnxt_ulp_cntxt_ptr2_fc_info_get(ctxt);
	if (!ulp_fc_info)
		goto err;

	dev_id = bnxt_ulp_devid_get(bp);

	dparms = bnxt_ulp_device_params_get(dev_id);
	if (!dparms) {
		netdev_dbg(bp->dev, "Failed to device parms\n");
		goto err;
	}

	/* Take the fc_lock to ensure no flow is destroyed
	 * during the bulk get
	 */
	mutex_lock(&ulp_fc_info->fc_lock);

	if (!ulp_fc_info->num_entries) {
		mutex_unlock(&ulp_fc_info->fc_lock);
		goto err;
	}

	num_entries = dparms->flow_count_db_entries / 2;
	for (i = 0; i < TF_DIR_MAX; i++) {
		for (j = 0; j < num_entries; j++) {
			if (!ulp_fc_info->sw_acc_tbl[i][j].valid)
				continue;
			hw_cntr_id = ulp_fc_info->sw_acc_tbl[i][j].hw_cntr_id;
			tfp = bnxt_ulp_cntxt_tfp_get(ctxt,
						     ulp_fc_info->sw_acc_tbl[i][j].session_type);
			if (!tfp) {
				mutex_unlock(&ulp_fc_info->fc_lock);
				netdev_dbg(bp->dev,
					   "Failed to get the truflow pointer\n");
				goto err;
			}
			rc = ulp_get_single_flow_stat(ctxt, tfp, ulp_fc_info, i,
						      hw_cntr_id, dparms);
			if (rc)
				break;
		}
	}

	mutex_unlock(&ulp_fc_info->fc_lock);
err:
	bnxt_ulp_cntxt_lock_release();
	if (fc_work)
		schedule_delayed_work(fc_work, msecs_to_jiffies(1000));
}

/**
 * Set the starting index that indicates the first HW flow
 * counter ID
 *
 * @ctxt: The ulp context for the flow counter manager
 *
 * @dir: The direction of the flow
 *
 * @start_idx: The HW flow counter ID
 *
 */
bool ulp_fc_mgr_start_idx_isset(struct bnxt_ulp_context *ctxt, enum tf_dir dir)
{
	struct bnxt_ulp_fc_info *ulp_fc_info;

	ulp_fc_info = bnxt_ulp_cntxt_ptr2_fc_info_get(ctxt);

	if (ulp_fc_info)
		return ulp_fc_info->shadow_hw_tbl[dir].start_idx_is_set;

	return false;
}

/**
 * Set the starting index that indicates the first HW flow
 * counter ID
 *
 * @ctxt: The ulp context for the flow counter manager
 *
 * @dir: The direction of the flow
 *
 * @start_idx: The HW flow counter ID
 *
 */
int ulp_fc_mgr_start_idx_set(struct bnxt_ulp_context *ctxt, enum tf_dir dir,
			     u32 start_idx)
{
	struct bnxt_ulp_fc_info *ulp_fc_info;

	ulp_fc_info = bnxt_ulp_cntxt_ptr2_fc_info_get(ctxt);

	if (!ulp_fc_info)
		return -EIO;

	if (!ulp_fc_info->shadow_hw_tbl[dir].start_idx_is_set) {
		ulp_fc_info->shadow_hw_tbl[dir].start_idx = start_idx;
		ulp_fc_info->shadow_hw_tbl[dir].start_idx_is_set = true;
	}

	return 0;
}

/**
 * Set the corresponding SW accumulator table entry based on
 * the difference between this counter ID and the starting
 * counter ID. Also, keep track of num of active counter enabled
 * flows.
 *
 * @ctxt: The ulp context for the flow counter manager
 *
 * @dir: The direction of the flow
 *
 * @hw_cntr_id: The HW flow counter ID
 *
 */
int ulp_fc_mgr_cntr_set(struct bnxt_ulp_context *ctxt, enum tf_dir dir,
			u32 hw_cntr_id, enum bnxt_ulp_session_type session_type)
{
	struct bnxt_ulp_fc_info *ulp_fc_info;
	u32 sw_cntr_idx;

	ulp_fc_info = bnxt_ulp_cntxt_ptr2_fc_info_get(ctxt);
	if (!ulp_fc_info)
		return -EIO;

	if (!ulp_fc_info->num_counters)
		return 0;

	mutex_lock(&ulp_fc_info->fc_lock);
	sw_cntr_idx = hw_cntr_id - ulp_fc_info->shadow_hw_tbl[dir].start_idx;
	ulp_fc_info->sw_acc_tbl[dir][sw_cntr_idx].valid = true;
	ulp_fc_info->sw_acc_tbl[dir][sw_cntr_idx].hw_cntr_id = hw_cntr_id;
	ulp_fc_info->sw_acc_tbl[dir][sw_cntr_idx].session_type = session_type;
	ulp_fc_info->num_entries++;
	mutex_unlock(&ulp_fc_info->fc_lock);

	return 0;
}

/**
 * Reset the corresponding SW accumulator table entry based on
 * the difference between this counter ID and the starting
 * counter ID.
 *
 * @ctxt: The ulp context for the flow counter manager
 *
 * @dir: The direction of the flow
 *
 * @hw_cntr_id: The HW flow counter ID
 *
 */
int ulp_fc_mgr_cntr_reset(struct bnxt_ulp_context *ctxt, enum tf_dir dir,
			  u32 hw_cntr_id)
{
	struct bnxt_ulp_fc_info *ulp_fc_info;
	u32 sw_cntr_idx;

	ulp_fc_info = bnxt_ulp_cntxt_ptr2_fc_info_get(ctxt);
	if (!ulp_fc_info)
		return -EIO;

	if (!ulp_fc_info->num_counters)
		return 0;

	mutex_lock(&ulp_fc_info->fc_lock);
	sw_cntr_idx = hw_cntr_id - ulp_fc_info->shadow_hw_tbl[dir].start_idx;
	ulp_fc_info->sw_acc_tbl[dir][sw_cntr_idx].valid = false;
	ulp_fc_info->sw_acc_tbl[dir][sw_cntr_idx].hw_cntr_id = 0;
	ulp_fc_info->sw_acc_tbl[dir][sw_cntr_idx].pkt_count = 0;
	ulp_fc_info->sw_acc_tbl[dir][sw_cntr_idx].byte_count = 0;
	ulp_fc_info->sw_acc_tbl[dir][sw_cntr_idx].session_type = 0;
	ulp_fc_info->num_entries--;
	mutex_unlock(&ulp_fc_info->fc_lock);

	return 0;
}

/**
 * Fill packets & bytes with the values obtained and
 * accumulated locally.
 *
 * @ctxt: The ulp context for the flow counter manager
 *
 * @flow_id: The HW flow ID
 *
 * @packets:
 * @bytes:
 * @lastused:
 *
 */

int ulp_tf_fc_mgr_query_count_get(struct bnxt_ulp_context *ctxt,
				  u32 flow_id,
			       u64 *packets, u64 *bytes,
			       unsigned long *lastused)
{
	struct sw_acc_counter *sw_acc_tbl_entry;
	struct bnxt_ulp_fc_info *ulp_fc_info;
	struct ulp_flow_db_res_params params;
	u32 hw_cntr_id = 0, sw_cntr_idx = 0;
	bool found_cntr_resource = false;
	u32 nxt_resource_index = 0;
	enum tf_dir dir;
	int rc = 0;

	ulp_fc_info = bnxt_ulp_cntxt_ptr2_fc_info_get(ctxt);
	if (!ulp_fc_info)
		return -ENODEV;

	mutex_lock(&ctxt->cfg_data->flow_db_lock);
	do {
		rc = ulp_flow_db_resource_get(ctxt,
					      BNXT_ULP_FDB_TYPE_REGULAR,
					      flow_id,
					      &nxt_resource_index,
					      &params);
		if (params.resource_func ==
		     BNXT_ULP_RESOURCE_FUNC_INDEX_TABLE &&
		     (params.resource_sub_type ==
		      BNXT_ULP_RESOURCE_SUB_TYPE_INDEX_TABLE_INT_COUNT ||
		      params.resource_sub_type ==
		      BNXT_ULP_RESOURCE_SUB_TYPE_INDEX_TABLE_EXT_COUNT ||
		      params.resource_sub_type ==
		      BNXT_ULP_RESOURCE_SUB_TYPE_INDEX_TABLE_INT_COUNT_ACC)) {
			found_cntr_resource = true;
			break;
		}
	} while (!rc && nxt_resource_index);
	mutex_unlock(&ctxt->cfg_data->flow_db_lock);

	if (rc || !found_cntr_resource)
		return rc;

	dir = params.direction;
	hw_cntr_id = params.resource_hndl;
	if (params.resource_sub_type ==
			BNXT_ULP_RESOURCE_SUB_TYPE_INDEX_TABLE_INT_COUNT) {
		if (!ulp_fc_info->num_counters)
			return ulp_tf_fc_tf_flow_stat_get(ctxt, &params,
							  packets, bytes);

		/* TODO:
		 * Think about optimizing with try_lock later
		 */
		mutex_lock(&ulp_fc_info->fc_lock);
		sw_cntr_idx = hw_cntr_id -
			ulp_fc_info->shadow_hw_tbl[dir].start_idx;
		sw_acc_tbl_entry = &ulp_fc_info->sw_acc_tbl[dir][sw_cntr_idx];
		if (sw_acc_tbl_entry->pkt_count) {
			*packets = sw_acc_tbl_entry->pkt_count;
			*bytes = sw_acc_tbl_entry->byte_count;
			sw_acc_tbl_entry->pkt_count = 0;
			sw_acc_tbl_entry->byte_count = 0;
			*lastused = jiffies;
		}
		mutex_unlock(&ulp_fc_info->fc_lock);
	} else {
		/* TBD: Handle External counters */
		rc = -EINVAL;
	}

	return rc;
}
#endif /* CONFIG_BNXT_FLOWER_OFFLOAD */
