// SPDX-License-Identifier: BSD-3-Clause
/* Copyright(c) 2019-2021 Broadcom
 * All rights reserved.
 */

#include "ulp_linux.h"
#include "bnxt_compat.h"
#include "bnxt_hsi.h"
#include "bnxt.h"
#include "bnxt_tf_common.h"
#include "ulp_utils.h"
#include "ulp_template_struct.h"
#include "ulp_mapper.h"
#include "ulp_flow_db.h"
#include "ulp_fc_mgr.h"

#if defined(CONFIG_BNXT_FLOWER_OFFLOAD) || defined(CONFIG_BNXT_CUSTOM_FLOWER_OFFLOAD)
#define ULP_FLOW_DB_RES_DIR_BIT		31
#define ULP_FLOW_DB_RES_DIR_MASK	0x80000000
#define ULP_FLOW_DB_RES_FUNC_BITS	28
#define ULP_FLOW_DB_RES_FUNC_MASK	0x70000000
#define ULP_FLOW_DB_RES_NXT_MASK	0x0FFFFFFF
#define ULP_FLOW_DB_RES_FUNC_UPPER	5
#define ULP_FLOW_DB_RES_FUNC_NEED_LOWER	0x80
#define ULP_FLOW_DB_RES_FUNC_LOWER_MASK	0x1F

/* Macro to copy the nxt_resource_idx */
#define ULP_FLOW_DB_RES_NXT_SET(dst, src)	{(dst) |= ((src) &\
					 ULP_FLOW_DB_RES_NXT_MASK); }
#define ULP_FLOW_DB_RES_NXT_RESET(dst)	((dst) &= ~(ULP_FLOW_DB_RES_NXT_MASK))

/**
 * Helper function to set the bit in the active flows
 * No validation is done in this function.
 *
 * @flow_db: Ptr to flow database
 * @flow_type: - specify default or regular
 * @idx: The index to bit to be set or reset.
 * @flag: 1 to set and 0 to reset.
 *
 * returns none
 */
static void
ulp_flow_db_active_flows_bit_set(struct bnxt_ulp_flow_db *flow_db,
				 enum bnxt_ulp_fdb_type flow_type,
				 u32 idx,
				 u32 flag)
{
	struct bnxt_ulp_flow_tbl *f_tbl = &flow_db->flow_tbl;
	u32 a_idx = idx / ULP_INDEX_BITMAP_SIZE;

	if (flag) {
		if (flow_type == BNXT_ULP_FDB_TYPE_REGULAR || flow_type ==
		    BNXT_ULP_FDB_TYPE_RID)
			ULP_INDEX_BITMAP_SET(f_tbl->active_reg_flows[a_idx],
					     idx);
		if (flow_type == BNXT_ULP_FDB_TYPE_DEFAULT || flow_type ==
		    BNXT_ULP_FDB_TYPE_RID)
			ULP_INDEX_BITMAP_SET(f_tbl->active_dflt_flows[a_idx],
					     idx);
	} else {
		if (flow_type == BNXT_ULP_FDB_TYPE_REGULAR || flow_type ==
		    BNXT_ULP_FDB_TYPE_RID)
			ULP_INDEX_BITMAP_RESET(f_tbl->active_reg_flows[a_idx],
					       idx);
		if (flow_type == BNXT_ULP_FDB_TYPE_DEFAULT || flow_type ==
		    BNXT_ULP_FDB_TYPE_RID)
			ULP_INDEX_BITMAP_RESET(f_tbl->active_dflt_flows[a_idx],
					       idx);
	}
}

/**
 * Helper function to check if given fid is active flow.
 * No validation being done in this function.
 *
 * @flow_db: Ptr to flow database
 * @flow_type: - specify default or regular
 * @idx: The index to bit to be set or reset.
 *
 * returns 1 on set or 0 if not set.
 */
static int
ulp_flow_db_active_flows_bit_is_set(struct bnxt_ulp_flow_db *flow_db,
				    enum bnxt_ulp_fdb_type flow_type,
				    u32 idx)
{
	struct bnxt_ulp_flow_tbl *f_tbl = &flow_db->flow_tbl;
	u32 a_idx = idx / ULP_INDEX_BITMAP_SIZE;
	u32 reg, dflt;

	reg = ULP_INDEX_BITMAP_GET(f_tbl->active_reg_flows[a_idx], idx);
	dflt = ULP_INDEX_BITMAP_GET(f_tbl->active_dflt_flows[a_idx], idx);

	switch (flow_type) {
	case BNXT_ULP_FDB_TYPE_REGULAR:
		return (reg && !dflt);
	case BNXT_ULP_FDB_TYPE_DEFAULT:
		return (!reg && dflt);
	case BNXT_ULP_FDB_TYPE_RID:
		return (reg && dflt);
	default:
		return 0;
	}
}

static inline enum tf_dir
ulp_flow_db_resource_dir_get(struct ulp_fdb_resource_info *res_info)
{
	return ((res_info->nxt_resource_idx & ULP_FLOW_DB_RES_DIR_MASK) >>
		ULP_FLOW_DB_RES_DIR_BIT);
}

static u8
ulp_flow_db_resource_func_get(struct ulp_fdb_resource_info *res_info)
{
	u8 func;

	func = (((res_info->nxt_resource_idx & ULP_FLOW_DB_RES_FUNC_MASK) >>
		 ULP_FLOW_DB_RES_FUNC_BITS) << ULP_FLOW_DB_RES_FUNC_UPPER);
	/* The resource func is split into upper and lower */
	if (func & ULP_FLOW_DB_RES_FUNC_NEED_LOWER)
		return (func | res_info->resource_func_lower);
	return func;
}

/**
 * Helper function to copy the resource params to resource info
 *  No validation being done in this function.
 *
 * @resource_info: Ptr to resource information
 * @params: The input params from the caller
 * returns none
 */
static void
ulp_flow_db_res_params_to_info(struct ulp_fdb_resource_info *resource_info,
			       struct ulp_flow_db_res_params *params)
{
	u32 resource_func;

	resource_info->nxt_resource_idx |= ((params->direction <<
				      ULP_FLOW_DB_RES_DIR_BIT) &
				     ULP_FLOW_DB_RES_DIR_MASK);
	resource_func = (params->resource_func >> ULP_FLOW_DB_RES_FUNC_UPPER);
	resource_info->nxt_resource_idx |= ((resource_func <<
					     ULP_FLOW_DB_RES_FUNC_BITS) &
					    ULP_FLOW_DB_RES_FUNC_MASK);

	if (params->resource_func & ULP_FLOW_DB_RES_FUNC_NEED_LOWER) {
		/* Break the resource func into two parts */
		resource_func = (params->resource_func &
				 ULP_FLOW_DB_RES_FUNC_LOWER_MASK);
		resource_info->resource_func_lower = resource_func;
	}

	/* Store the handle as 64bit only for EM table entries */
	if (params->resource_func != BNXT_ULP_RESOURCE_FUNC_EM_TABLE) {
		resource_info->resource_hndl = (u32)params->resource_hndl;
		resource_info->key_data = params->key_data;
		resource_info->resource_type = params->resource_type;
		resource_info->resource_sub_type = params->resource_sub_type;
		resource_info->fdb_flags = params->fdb_flags;
	} else {
		resource_info->resource_em_handle = params->resource_hndl;
	}
}

/**
 * Helper function to copy the resource params to resource info
 *  No validation being done in this function.
 *
 * @resource_info: Ptr to resource information
 * @params: The output params to the caller
 *
 * returns none
 */
static void
ulp_flow_db_res_info_to_params(struct ulp_fdb_resource_info *resource_info,
			       struct ulp_flow_db_res_params *params)
{
	memset(params, 0, sizeof(struct ulp_flow_db_res_params));

	/* use the helper function to get the resource func */
	params->direction = ulp_flow_db_resource_dir_get(resource_info);
	params->resource_func = ulp_flow_db_resource_func_get(resource_info);

	if (params->resource_func == BNXT_ULP_RESOURCE_FUNC_EM_TABLE) {
		params->resource_hndl = resource_info->resource_em_handle;
	} else if (params->resource_func & ULP_FLOW_DB_RES_FUNC_NEED_LOWER) {
		params->resource_hndl = resource_info->resource_hndl;
		params->key_data = resource_info->key_data;
		params->resource_type = resource_info->resource_type;
		params->resource_sub_type = resource_info->resource_sub_type;
		params->fdb_flags = resource_info->fdb_flags;
	}
}

/**
 * Helper function to allocate the flow table and initialize
 * the stack for allocation operations.
 *
 * @flow_db: Ptr to flow database structure
 *
 * Returns 0 on success or negative number on failure.
 */
static int
ulp_flow_db_alloc_resource(struct bnxt_ulp_context *ulp_ctxt,
			   struct bnxt_ulp_flow_db *flow_db)
{
	struct bnxt_ulp_flow_tbl *flow_tbl;
	u32 idx = 0;
	u32 size;

	flow_tbl = &flow_db->flow_tbl;

	size = sizeof(struct ulp_fdb_resource_info) * flow_tbl->num_resources;
	flow_tbl->flow_resources = vzalloc(size);

	if (!flow_tbl->flow_resources)
		return -ENOMEM;

	size = sizeof(u32) * flow_tbl->num_resources;
	flow_tbl->flow_tbl_stack = vzalloc(size);
	if (!flow_tbl->flow_tbl_stack)
		return -ENOMEM;

	size = (flow_tbl->num_flows / sizeof(u64)) + 1;
	size = ULP_BYTE_ROUND_OFF_8(size);
	flow_tbl->active_reg_flows = vzalloc(size);
	if (!flow_tbl->active_reg_flows)
		return -ENOMEM;

	flow_tbl->active_dflt_flows = vzalloc(size);
	if (!flow_tbl->active_dflt_flows)
		return -ENOMEM;

	/* Initialize the stack table. */
	for (idx = 0; idx < flow_tbl->num_resources; idx++)
		flow_tbl->flow_tbl_stack[idx] = idx;

	/* Ignore the first element in the list. */
	flow_tbl->head_index = 1;
	/* Tail points to the last entry in the list. */
	flow_tbl->tail_index = flow_tbl->num_resources - 1;
	return 0;
}

/**
 * Helper function to deallocate the flow table.
 *
 * @flow_db: Ptr to flow database structure
 *
 * Returns none.
 */
static void
ulp_flow_db_dealloc_resource(struct bnxt_ulp_flow_db *flow_db)
{
	struct bnxt_ulp_flow_tbl *flow_tbl = &flow_db->flow_tbl;

	/* Free all the allocated tables in the flow table. */
	vfree(flow_tbl->active_reg_flows);
	flow_tbl->active_reg_flows = NULL;

	vfree(flow_tbl->active_dflt_flows);
	flow_tbl->active_dflt_flows = NULL;

	vfree(flow_tbl->flow_tbl_stack);
	flow_tbl->flow_tbl_stack = NULL;

	vfree(flow_tbl->flow_resources);
	flow_tbl->flow_resources = NULL;
}

/**
 * Helper function to add function id to the flow table
 *
 * @flow_db: Ptr to flow table
 * @flow_id: The flow id of the flow
 * @func_id: The func_id to be set, for reset pass zero
 *
 * returns none
 */
static void
ulp_flow_db_func_id_set(struct bnxt_ulp_context *ulp_ctxt,
			struct bnxt_ulp_flow_db *flow_db,
			u32 flow_id,
			u32 func_id)
{
	/* set the function id in the function table */
	if (flow_id < flow_db->func_id_tbl_size)
		flow_db->func_id_tbl[flow_id] = func_id;
	else /* This should never happen */
		netdev_dbg(ulp_ctxt->bp->dev, "Invalid flow id, flowdb corrupt\n");
}

/**
 * Initialize the flow database. Memory is allocated in this
 * call and assigned to the flow database.
 *
 * @ulp_ctxt: Ptr to ulp context
 *
 * Returns 0 on success or negative number on failure.
 */
int
ulp_flow_db_init(struct bnxt_ulp_context *ulp_ctxt)
{
	struct bnxt_ulp_device_params *dparms;
	struct bnxt_ulp_flow_tbl *flow_tbl;
	enum bnxt_ulp_flow_mem_type mtype;
	struct bnxt_ulp_flow_db *flow_db;
	u32 dev_id, num_flows;

	dev_id = bnxt_ulp_devid_get(ulp_ctxt->bp);

	dparms = bnxt_ulp_device_params_get(dev_id);
	if (!dparms) {
		netdev_dbg(ulp_ctxt->bp->dev, "could not fetch the device params\n");
		return -ENODEV;
	}

	flow_db = vzalloc(sizeof(*flow_db));
	if (!flow_db)
		return -ENOMEM;

	/* Attach the flow database to the ulp context. */
	bnxt_ulp_cntxt_ptr2_flow_db_set(ulp_ctxt, flow_db);

	/* Determine the number of flows based on EM type */
	if (bnxt_ulp_cntxt_mem_type_get(ulp_ctxt, &mtype))
		goto error_free;

	if (mtype == BNXT_ULP_FLOW_MEM_TYPE_INT)
		num_flows = dparms->int_flow_db_num_entries;
	else
		num_flows = dparms->ext_flow_db_num_entries;

	/* Populate the regular flow table limits. */
	flow_tbl = &flow_db->flow_tbl;
	flow_tbl->num_flows = num_flows + 1;
	flow_tbl->num_resources = ((num_flows + 1) *
				   dparms->num_resources_per_flow);

	/* Include the default flow table limits. */
	flow_tbl->num_flows += (BNXT_FLOW_DB_DEFAULT_NUM_FLOWS + 1);
	flow_tbl->num_resources += ((BNXT_FLOW_DB_DEFAULT_NUM_FLOWS + 1) *
				    BNXT_FLOW_DB_DEFAULT_NUM_RESOURCES);

	/* Allocate the resource for the flow table. */
	if (ulp_flow_db_alloc_resource(ulp_ctxt, flow_db))
		goto error_free;

	/* add 1 since we are not using index 0 for flow id */
	flow_db->func_id_tbl_size = flow_tbl->num_flows + 1;
	/* Allocate the function Id table */
	flow_db->func_id_tbl = vzalloc(flow_db->func_id_tbl_size * sizeof(u16));
	if (!flow_db->func_id_tbl)
		goto error_free;

	/* All good so return. */
	netdev_dbg(ulp_ctxt->bp->dev, "FlowDB initialized with %d flows.\n",
		   flow_tbl->num_flows);
	return 0;
error_free:
	ulp_flow_db_deinit(ulp_ctxt);
	return -ENOMEM;
}

/**
 * Deinitialize the flow database. Memory is deallocated in
 * this call and all flows should have been purged before this
 * call.
 *
 * @ulp_ctxt: Ptr to ulp context
 *
 * Returns 0 on success.
 */
int
ulp_flow_db_deinit(struct bnxt_ulp_context *ulp_ctxt)
{
	struct bnxt_ulp_flow_db *flow_db;

	flow_db = bnxt_ulp_cntxt_ptr2_flow_db_get(ulp_ctxt);
	if (!flow_db)
		return -EINVAL;

	/* Detach the flow database from the ulp context. */
	bnxt_ulp_cntxt_ptr2_flow_db_set(ulp_ctxt, NULL);

	/* Free up all the memory. */
	ulp_flow_db_dealloc_resource(flow_db);
	vfree(flow_db->func_id_tbl);
	vfree(flow_db);

	return 0;
}

/**
 * Allocate the flow database entry
 *
 * @ulp_ctxt: Ptr to ulp_context
 * @flow_type: - specify default or regular
 * @func_id:.function id of the ingress port
 * @fid: The index to the flow entry
 *
 * returns 0 on success and negative on failure.
 */
int
ulp_flow_db_fid_alloc(struct bnxt_ulp_context *ulp_ctxt,
		      enum bnxt_ulp_fdb_type flow_type,
		      u16 func_id,
		      u32 *fid)
{
	struct bnxt_ulp_flow_tbl *flow_tbl;
	struct bnxt_ulp_flow_db *flow_db;

	*fid = 0; /* Initialize fid to invalid value */
	flow_db = bnxt_ulp_cntxt_ptr2_flow_db_get(ulp_ctxt);
	if (!flow_db) {
		netdev_dbg(ulp_ctxt->bp->dev, "Invalid Arguments\n");
		return -EINVAL;
	}

	if (flow_type >= BNXT_ULP_FDB_TYPE_LAST) {
		netdev_dbg(ulp_ctxt->bp->dev, "Invalid flow type\n");
		return -EINVAL;
	}

	flow_tbl = &flow_db->flow_tbl;
	/* check for max flows */
	if (flow_tbl->num_flows <= flow_tbl->head_index) {
		netdev_dbg(ulp_ctxt->bp->dev, "Flow database has reached max flows\n");
		return -ENOSPC;
	}

	if (flow_tbl->tail_index <= (flow_tbl->head_index + 1)) {
		netdev_dbg(ulp_ctxt->bp->dev, "Flow database has reached max resources\n");
		return -ENOSPC;
	}
	*fid = flow_tbl->flow_tbl_stack[flow_tbl->head_index];
	flow_tbl->head_index++;

	/* Set the flow type */
	ulp_flow_db_active_flows_bit_set(flow_db, flow_type, *fid, 1);

	/* function id update is only valid for regular flow table */
	if (flow_type == BNXT_ULP_FDB_TYPE_REGULAR)
		ulp_flow_db_func_id_set(ulp_ctxt, flow_db, *fid, func_id);

	netdev_dbg(ulp_ctxt->bp->dev, "flow_id = %u:%u allocated\n", flow_type, *fid);
	/* return success */
	return 0;
}

/**
 * Allocate the flow database entry.
 * The params->critical_resource has to be set to 0 to allocate a new resource.
 *
 * @ulp_ctxt: Ptr to ulp_context
 * @flow_type: Specify it is regular or default flow
 * @fid: The index to the flow entry
 * @params: The contents to be copied into resource
 *
 * returns 0 on success and negative on failure.
 */
int
ulp_flow_db_resource_add(struct bnxt_ulp_context *ulp_ctxt,
			 enum bnxt_ulp_fdb_type flow_type,
			 u32 fid,
			 struct ulp_flow_db_res_params *params)
{
	struct ulp_fdb_resource_info *resource, *fid_resource;
	struct bnxt_ulp_fc_info *ulp_fc_info;
	struct bnxt_ulp_flow_tbl *flow_tbl;
	struct bnxt_ulp_flow_db *flow_db;
	u32 idx;

	flow_db = bnxt_ulp_cntxt_ptr2_flow_db_get(ulp_ctxt);
	if (!flow_db) {
		netdev_dbg(ulp_ctxt->bp->dev, "Invalid Arguments\n");
		return -EINVAL;
	}

	if (flow_type >= BNXT_ULP_FDB_TYPE_LAST) {
		netdev_dbg(ulp_ctxt->bp->dev, "Invalid flow type\n");
		return -EINVAL;
	}

	flow_tbl = &flow_db->flow_tbl;
	/* check for max flows */
	if (fid >= flow_tbl->num_flows || !fid) {
		netdev_dbg(ulp_ctxt->bp->dev,
			   "Invalid flow index fid %d num_flows %d\n",
			   fid, flow_tbl->num_flows);
		return -EINVAL;
	}

	/* check if the flow is active or not */
	if (!ulp_flow_db_active_flows_bit_is_set(flow_db, flow_type, fid)) {
		netdev_dbg(ulp_ctxt->bp->dev, "flow does not exist %x:%x\n", flow_type, fid);
		return -EINVAL;
	}

	/* check for max resource */
	if ((flow_tbl->head_index + 1) >= flow_tbl->tail_index) {
		netdev_dbg(ulp_ctxt->bp->dev, "Flow db has reached max resources\n");
		return -ENOSPC;
	}
	fid_resource = &flow_tbl->flow_resources[fid];

	if (params->critical_resource && fid_resource->resource_em_handle) {
		netdev_dbg(ulp_ctxt->bp->dev, "Ignore multiple critical resources\n");
		/* Ignore the multiple critical resources */
		params->critical_resource = BNXT_ULP_CRITICAL_RESOURCE_NO;
	}

	if (!params->critical_resource) {
		/* Not the critical_resource so allocate a resource */
		idx = flow_tbl->flow_tbl_stack[flow_tbl->tail_index];
		resource = &flow_tbl->flow_resources[idx];
		flow_tbl->tail_index--;

		/* Update the chain list of resource*/
		ULP_FLOW_DB_RES_NXT_SET(resource->nxt_resource_idx,
					fid_resource->nxt_resource_idx);
		/* update the contents */
		ulp_flow_db_res_params_to_info(resource, params);
		ULP_FLOW_DB_RES_NXT_RESET(fid_resource->nxt_resource_idx);
		ULP_FLOW_DB_RES_NXT_SET(fid_resource->nxt_resource_idx,
					idx);
	} else {
		/* critical resource. Just update the fid resource */
		ulp_flow_db_res_params_to_info(fid_resource, params);
	}

	ulp_fc_info = bnxt_ulp_cntxt_ptr2_fc_info_get(ulp_ctxt);
	if (params->resource_type == TF_TBL_TYPE_ACT_STATS_64 &&
	    params->resource_sub_type ==
	    BNXT_ULP_RESOURCE_SUB_TYPE_INDEX_TABLE_INT_COUNT &&
	    ulp_fc_info && ulp_fc_info->num_counters) {
		/* Store the first HW counter ID for this table */
		if (!ulp_fc_mgr_start_idx_isset(ulp_ctxt, params->direction))
			ulp_fc_mgr_start_idx_set(ulp_ctxt, params->direction,
						 params->resource_hndl);

		ulp_fc_mgr_cntr_set(ulp_ctxt, params->direction,
				    params->resource_hndl,
				    ulp_flow_db_shared_session_get(params));

	}

	/* all good, return success */
	return 0;
}

/**
 * Free the flow database entry.
 * The params->critical_resource has to be set to 1 to free the first resource.
 *
 * @ulp_ctxt: Ptr to ulp_context
 * @flow_type: Specify it is regular or default flow
 * @fid: The index to the flow entry
 * @params: The contents to be copied into params.
 * Onlythe critical_resource needs to be set by the caller.
 *
 * Returns 0 on success and negative on failure.
 */
int
ulp_flow_db_resource_del(struct bnxt_ulp_context *ulp_ctxt,
			 enum bnxt_ulp_fdb_type flow_type,
			 u32 fid,
			 struct ulp_flow_db_res_params *params)
{
	struct ulp_fdb_resource_info *nxt_resource, *fid_resource;
	struct bnxt_ulp_flow_tbl *flow_tbl;
	struct bnxt_ulp_flow_db *flow_db;
	u32 nxt_idx = 0;

	flow_db = bnxt_ulp_cntxt_ptr2_flow_db_get(ulp_ctxt);
	if (!flow_db) {
		netdev_dbg(ulp_ctxt->bp->dev, "Invalid Arguments\n");
		return -EINVAL;
	}

	if (flow_type >= BNXT_ULP_FDB_TYPE_LAST) {
		netdev_dbg(ulp_ctxt->bp->dev, "Invalid flow type\n");
		return -EINVAL;
	}

	flow_tbl = &flow_db->flow_tbl;
	/* check for max flows */
	if (fid >= flow_tbl->num_flows || !fid) {
		netdev_dbg(ulp_ctxt->bp->dev,
			   "Invalid flow index fid %d num_flows %d\n",
			   fid, flow_tbl->num_flows);
		return -EINVAL;
	}

	/* check if the flow is active or not */
	if (!ulp_flow_db_active_flows_bit_is_set(flow_db, flow_type, fid)) {
		netdev_dbg(ulp_ctxt->bp->dev, "flow does not exist %x:%x\n", flow_type, fid);
		return -EINVAL;
	}

	fid_resource = &flow_tbl->flow_resources[fid];
	if (!params->critical_resource) {
		/* Not the critical resource so free the resource */
		ULP_FLOW_DB_RES_NXT_SET(nxt_idx,
					fid_resource->nxt_resource_idx);
		if (!nxt_idx) {
			/* reached end of resources */
			return -ENOENT;
		}
		nxt_resource = &flow_tbl->flow_resources[nxt_idx];

		/* connect the fid resource to the next resource */
		ULP_FLOW_DB_RES_NXT_RESET(fid_resource->nxt_resource_idx);
		ULP_FLOW_DB_RES_NXT_SET(fid_resource->nxt_resource_idx,
					nxt_resource->nxt_resource_idx);

		/* update the contents to be given to caller */
		ulp_flow_db_res_info_to_params(nxt_resource, params);

		/* Delete the nxt_resource */
		memset(nxt_resource, 0, sizeof(struct ulp_fdb_resource_info));

		/* add it to the free list */
		flow_tbl->tail_index++;
		if (flow_tbl->tail_index >= flow_tbl->num_resources) {
			netdev_dbg(ulp_ctxt->bp->dev, "FlowDB:Tail reached max\n");
			return -ENOENT;
		}
		flow_tbl->flow_tbl_stack[flow_tbl->tail_index] = nxt_idx;

	} else {
		/* Critical resource. copy the contents and exit */
		ulp_flow_db_res_info_to_params(fid_resource, params);
		ULP_FLOW_DB_RES_NXT_SET(nxt_idx,
					fid_resource->nxt_resource_idx);
		memset(fid_resource, 0, sizeof(struct ulp_fdb_resource_info));
		ULP_FLOW_DB_RES_NXT_SET(fid_resource->nxt_resource_idx,
					nxt_idx);
	}

	/* Now that the HW Flow counter resource is deleted, reset it's
	 * corresponding slot in the SW accumulation table in the Flow Counter
	 * manager
	 */
	if (params->resource_type == TF_TBL_TYPE_ACT_STATS_64 &&
	    params->resource_sub_type ==
	    BNXT_ULP_RESOURCE_SUB_TYPE_INDEX_TABLE_INT_COUNT) {
		ulp_fc_mgr_cntr_reset(ulp_ctxt, params->direction,
				      params->resource_hndl);
	}

	/* all good, return success */
	return 0;
}

/**
 * Free the flow database entry
 *
 * @ulp_ctxt: Ptr to ulp_context
 * @flow_type: - specify default or regular
 * @fid: The index to the flow entry
 *
 * returns 0 on success and negative on failure.
 */
int
ulp_flow_db_fid_free(struct bnxt_ulp_context *ulp_ctxt,
		     enum bnxt_ulp_fdb_type flow_type,
		     u32 fid)
{
	struct bnxt_ulp_flow_tbl *flow_tbl;
	struct bnxt_ulp_flow_db *flow_db;

	flow_db = bnxt_ulp_cntxt_ptr2_flow_db_get(ulp_ctxt);
	if (!flow_db) {
		netdev_dbg(ulp_ctxt->bp->dev, "Invalid Arguments\n");
		return -EINVAL;
	}

	if (flow_type >= BNXT_ULP_FDB_TYPE_LAST) {
		netdev_dbg(ulp_ctxt->bp->dev, "Invalid flow type\n");
		return -EINVAL;
	}

	flow_tbl = &flow_db->flow_tbl;

	/* check for limits of fid */
	if (fid >= flow_tbl->num_flows || !fid) {
		netdev_dbg(ulp_ctxt->bp->dev,
			   "Invalid flow index fid %d num_flows %d\n",
			   fid, flow_tbl->num_flows);
		return -EINVAL;
	}

	/* check if the flow is active or not */
	if (!ulp_flow_db_active_flows_bit_is_set(flow_db, flow_type, fid)) {
		netdev_dbg(ulp_ctxt->bp->dev, "flow does not exist %x:%x\n", flow_type, fid);
		return -EINVAL;
	}
	flow_tbl->head_index--;
	if (!flow_tbl->head_index) {
		netdev_dbg(ulp_ctxt->bp->dev, "FlowDB: Head Ptr is zero\n");
		return -ENOENT;
	}

	flow_tbl->flow_tbl_stack[flow_tbl->head_index] = fid;

	/* Clear the flows bitmap */
	ulp_flow_db_active_flows_bit_set(flow_db, flow_type, fid, 0);

	if (flow_type == BNXT_ULP_FDB_TYPE_REGULAR)
		ulp_flow_db_func_id_set(ulp_ctxt, flow_db, fid, 0);

	netdev_dbg(ulp_ctxt->bp->dev, "flow_id = %u:%u freed\n", flow_type, fid);
	/* all good, return success */
	return 0;
}

/**
 *Get the flow database entry details
 *
 * @ulp_ctxt: Ptr to ulp_context
 * @flow_type: - specify default or regular
 * @fid: The index to the flow entry
 * @nxt_idx: the index to the next entry
 * @params: The contents to be copied into params.
 *
 * returns 0 on success and negative on failure.
 */
int
ulp_flow_db_resource_get(struct bnxt_ulp_context *ulp_ctxt,
			 enum bnxt_ulp_fdb_type flow_type,
			 u32 fid,
			 u32 *nxt_idx,
			 struct ulp_flow_db_res_params *params)
{
	struct ulp_fdb_resource_info *nxt_resource, *fid_resource;
	struct bnxt_ulp_flow_tbl *flow_tbl;
	struct bnxt_ulp_flow_db *flow_db;

	flow_db = bnxt_ulp_cntxt_ptr2_flow_db_get(ulp_ctxt);
	if (!flow_db) {
		netdev_dbg(ulp_ctxt->bp->dev, "Invalid Arguments\n");
		return -EINVAL;
	}

	if (flow_type >= BNXT_ULP_FDB_TYPE_LAST) {
		netdev_dbg(ulp_ctxt->bp->dev, "Invalid flow type\n");
		return -EINVAL;
	}

	flow_tbl = &flow_db->flow_tbl;

	/* check for limits of fid */
	if (fid >= flow_tbl->num_flows || !fid) {
		netdev_dbg(ulp_ctxt->bp->dev,
			   "Invalid flow index fid %d num_flows %d\n",
			   fid, flow_tbl->num_flows);
		return -EINVAL;
	}

	/* check if the flow is active or not */
	if (!ulp_flow_db_active_flows_bit_is_set(flow_db, flow_type, fid)) {
		netdev_dbg(ulp_ctxt->bp->dev, "flow does not exist\n");
		return -EINVAL;
	}

	if (!*nxt_idx) {
		fid_resource = &flow_tbl->flow_resources[fid];
		ulp_flow_db_res_info_to_params(fid_resource, params);
		ULP_FLOW_DB_RES_NXT_SET(*nxt_idx,
					fid_resource->nxt_resource_idx);
	} else {
		nxt_resource = &flow_tbl->flow_resources[*nxt_idx];
		ulp_flow_db_res_info_to_params(nxt_resource, params);
		*nxt_idx = 0;
		ULP_FLOW_DB_RES_NXT_SET(*nxt_idx,
					nxt_resource->nxt_resource_idx);
	}

	/* all good, return success */
	return 0;
}

/**
 * Get the flow database entry iteratively
 *
 * @flow_tbl: Ptr to flow table
 * @flow_type: - specify default or regular
 * @fid: The index to the flow entry
 *
 * returns 0 on success and negative on failure.
 */
static int
ulp_flow_db_next_entry_get(struct bnxt_ulp_context *ulp_ctxt,
			   struct bnxt_ulp_flow_db *flow_db,
			   enum bnxt_ulp_fdb_type flow_type,
			   u32 *fid)
{
	struct bnxt_ulp_flow_tbl *flowtbl = &flow_db->flow_tbl;
	u32 idx, s_idx, mod_fid;
	u64 *active_flows;
	u32 lfid = *fid;
	u64 bs;

	if (flow_type == BNXT_ULP_FDB_TYPE_REGULAR)
		active_flows = flowtbl->active_reg_flows;
	else if (flow_type == BNXT_ULP_FDB_TYPE_DEFAULT)
		active_flows = flowtbl->active_dflt_flows;
	else
		return -EINVAL;

	do {
		/* increment the flow id to find the next valid flow id */
		lfid++;
		if (lfid >= flowtbl->num_flows)
			return -ENOENT;
		idx = lfid / ULP_INDEX_BITMAP_SIZE;
		mod_fid = lfid % ULP_INDEX_BITMAP_SIZE;
		s_idx = idx;
		while (!(bs = active_flows[idx])) {
			idx++;
			if ((idx * ULP_INDEX_BITMAP_SIZE) >= flowtbl->num_flows)
				return -ENOENT;
		}
		/* remove the previous bits in the bitset bs to find the
		 * next non zero bit in the bitset. This needs to be done
		 * only if the idx is same as he one you started.
		 */
		if (s_idx == idx)
			bs &= (-1UL >> mod_fid);
		lfid = (idx * ULP_INDEX_BITMAP_SIZE) + __builtin_clzl(bs);
		if (*fid >= lfid)
			return -ENOENT;
	} while (!ulp_flow_db_active_flows_bit_is_set(flow_db, flow_type,
						      lfid));

	/* all good, return success */
	*fid = lfid;
	return 0;
}

/**
 * Flush all flows in the flow database.
 *
 * @ulp_ctxt: Ptr to ulp context
 * @flow_type: - specify default or regular
 *
 * returns 0 on success or negative number on failure
 */
int
ulp_flow_db_flush_flows(struct bnxt_ulp_context *ulp_ctx,
			enum bnxt_ulp_fdb_type flow_type)
{
	struct bnxt_ulp_flow_db *flow_db;
	u32 fid = 0;

	if (!ulp_ctx)
		return -EINVAL;

	flow_db = bnxt_ulp_cntxt_ptr2_flow_db_get(ulp_ctx);
	if (!flow_db) {
		netdev_dbg(ulp_ctx->bp->dev, "Flow database not found\n");
		return -EINVAL;
	}

	mutex_lock(&ulp_ctx->cfg_data->flow_db_lock);
	while (!ulp_flow_db_next_entry_get(ulp_ctx, flow_db, flow_type, &fid))
		ulp_mapper_resources_free(ulp_ctx, flow_type, fid);
	mutex_unlock(&ulp_ctx->cfg_data->flow_db_lock);

	return 0;
}

/**
 * Flush all flows in the flow database that belong to a device function.
 *
 * @ulp_ctxt: Ptr to ulp context
 * @func_id: - The port function id
 *
 * returns 0 on success or negative number on failure
 */
int
ulp_flow_db_function_flow_flush(struct bnxt_ulp_context *ulp_ctx,
				u16 func_id)
{
	struct bnxt_ulp_flow_db *flow_db;
	u32 flow_id = 0;

	if (!ulp_ctx || !func_id)
		return -EINVAL;

	flow_db = bnxt_ulp_cntxt_ptr2_flow_db_get(ulp_ctx);
	if (!flow_db) {
		netdev_dbg(ulp_ctx->bp->dev, "Flow database not found\n");
		return -EINVAL;
	}

	mutex_lock(&ulp_ctx->cfg_data->flow_db_lock);
	while (!ulp_flow_db_next_entry_get(ulp_ctx, flow_db, BNXT_ULP_FDB_TYPE_REGULAR,
					   &flow_id)) {
		if (flow_db->func_id_tbl[flow_id] == func_id)
			ulp_mapper_resources_free(ulp_ctx,
						  BNXT_ULP_FDB_TYPE_REGULAR,
						  flow_id);
	}
	mutex_unlock(&ulp_ctx->cfg_data->flow_db_lock);

	return 0;
}

/**
 * Flush all flows in the flow database that are associated with the session.
 *
 * @ulp_ctxt: Ptr to ulp context
 *
 * returns 0 on success or negative number on failure
 */
int
ulp_flow_db_session_flow_flush(struct bnxt_ulp_context *ulp_ctx)
{
	/* TBD: Tf core implementation of FW session flush shall change this
	 * implementation.
	 */
	return ulp_flow_db_flush_flows(ulp_ctx, BNXT_ULP_FDB_TYPE_REGULAR);
}

/**
 * Check that flow id matches the function id or not
 *
 * @ulp_ctxt: Ptr to ulp context
 * @flow_db: Ptr to flow table
 * @func_id: The func_id to be set, for reset pass zero.
 *
 * returns 0 on success else errors
 */
int
ulp_flow_db_validate_flow_func(struct bnxt_ulp_context *ulp_ctxt,
			       u32 flow_id,
			       u32 func_id)
{
	struct bnxt_ulp_flow_db *flow_db;

	flow_db = bnxt_ulp_cntxt_ptr2_flow_db_get(ulp_ctxt);
	if (!flow_db) {
		netdev_dbg(ulp_ctxt->bp->dev, "Flow database not found\n");
		return -EINVAL;
	}

	/* check if the flow is active or not */
	if (!ulp_flow_db_active_flows_bit_is_set(flow_db, BNXT_ULP_FDB_TYPE_REGULAR, flow_id)) {
		netdev_dbg(ulp_ctxt->bp->dev, "Flow does not exist %x:%x\n",
			   BNXT_ULP_FDB_TYPE_REGULAR, flow_id);
		return -ENOENT;
	}

	/* check the function id in the function table */
	if (flow_id < flow_db->func_id_tbl_size && func_id &&
	    flow_db->func_id_tbl[flow_id] != func_id) {
		netdev_dbg(ulp_ctxt->bp->dev,
			   "Function id %x does not own flow %x:%x\n",
			   func_id, BNXT_ULP_FDB_TYPE_REGULAR, flow_id);
		return -EINVAL;
	}

	return 0;
}

/**
 * Internal api to traverse the resource list within a flow
 * and match a resource based on resource func and resource
 * sub type. This api should be used only for resources that
 * are unique and do not have multiple instances of resource
 * func and sub type combination since it will return only
 * the first match.
 */
static int
ulp_flow_db_resource_params_get(struct bnxt_ulp_context *ulp_ctxt,
				enum bnxt_ulp_fdb_type flow_type,
				u32 flow_id,
				u32 resource_func,
				u32 res_subtype,
				struct ulp_flow_db_res_params *params)
{
	struct ulp_fdb_resource_info *fid_res;
	struct bnxt_ulp_flow_tbl *flow_tbl;
	struct bnxt_ulp_flow_db *flow_db;
	u32 res_id;

	flow_db = bnxt_ulp_cntxt_ptr2_flow_db_get(ulp_ctxt);
	if (!flow_db) {
		netdev_dbg(ulp_ctxt->bp->dev, "Flow database not found\n");
		return -EINVAL;
	}

	if (!params) {
		netdev_dbg(ulp_ctxt->bp->dev, "invalid argument\n");
		return -EINVAL;
	}

	if (flow_type >= BNXT_ULP_FDB_TYPE_LAST) {
		netdev_dbg(ulp_ctxt->bp->dev, "Invalid flow type\n");
		return -EINVAL;
	}

	flow_tbl = &flow_db->flow_tbl;

	/* check for limits of fid */
	if (flow_id >= flow_tbl->num_flows || !flow_id) {
		netdev_dbg(ulp_ctxt->bp->dev,
			   "Invalid flow index fid %d num_flows %d\n",
			   flow_id, flow_tbl->num_flows);
		return -EINVAL;
	}

	/* check if the flow is active or not */
	if (!ulp_flow_db_active_flows_bit_is_set(flow_db, flow_type, flow_id)) {
		netdev_dbg(ulp_ctxt->bp->dev, "flow does not exist\n");
		return -EINVAL;
	}
	/* Iterate the resource to get the resource handle */
	res_id =  flow_id;
	memset(params, 0, sizeof(struct ulp_flow_db_res_params));
	while (res_id) {
		fid_res = &flow_tbl->flow_resources[res_id];
		if (ulp_flow_db_resource_func_get(fid_res) == resource_func) {
			if (resource_func & ULP_FLOW_DB_RES_FUNC_NEED_LOWER) {
				if (res_subtype == fid_res->resource_sub_type) {
					ulp_flow_db_res_info_to_params(fid_res,
								       params);
					return 0;
				}

			} else if (resource_func ==
				   BNXT_ULP_RESOURCE_FUNC_EM_TABLE) {
				ulp_flow_db_res_info_to_params(fid_res,
							       params);
				return 0;
			}
		}
		res_id = 0;
		ULP_FLOW_DB_RES_NXT_SET(res_id, fid_res->nxt_resource_idx);
	}
	return -ENOENT;
}

/**
 * Api to get the cfa action pointer from a flow.
 *
 * @ulp_ctxt: Ptr to ulp context
 * @flow_id: flow id
 * @cfa_action: The resource handle stored in the flow database
 *
 * returns 0 on success
 */
int
ulp_default_flow_db_cfa_action_get(struct bnxt_ulp_context *ulp_ctxt,
				   u32 flow_id,
				   u16 *cfa_action)
{
	u8 sub_typ = BNXT_ULP_RESOURCE_SUB_TYPE_INDEX_TABLE_VFR_CFA_ACTION;
	struct ulp_flow_db_res_params params;
	int rc;

	rc = ulp_flow_db_resource_params_get(ulp_ctxt,
					     BNXT_ULP_FDB_TYPE_DEFAULT,
					     flow_id,
					     BNXT_ULP_RESOURCE_FUNC_INDEX_TABLE,
					     sub_typ, &params);
	if (rc) {
		netdev_dbg(ulp_ctxt->bp->dev,
			   "CFA Action ptr not found for flow id %u\n",
			   flow_id);
		return -ENOENT;
	}
	*cfa_action = params.resource_hndl;
	return 0;
}

/* Set the shared bit for the flow db entry
 * @res: Ptr to fdb entry
 * @shared: shared flag
 *
 * returns none
 */
void ulp_flow_db_shared_session_set(struct ulp_flow_db_res_params *res,
				    enum bnxt_ulp_session_type s_type)
{
	if (res && (s_type & BNXT_ULP_SESSION_TYPE_SHARED))
		res->fdb_flags |= ULP_FDB_FLAG_SHARED_SESSION;
}

enum bnxt_ulp_session_type
ulp_flow_db_shared_session_get(struct ulp_flow_db_res_params *res)
{
	enum bnxt_ulp_session_type stype = BNXT_ULP_SESSION_TYPE_DEFAULT;

	if (res && (res->fdb_flags & ULP_FDB_FLAG_SHARED_SESSION))
		stype = BNXT_ULP_SESSION_TYPE_SHARED;

	return stype;
}

#ifdef TC_BNXT_TRUFLOW_DEBUG

/**
 * Dump the entry details
 *
 * @ulp_ctxt: Ptr to ulp_context
 *
 * returns none
 */
static void ulp_flow_db_res_dump(struct bnxt_ulp_context *ulp_ctxt,
				 struct ulp_fdb_resource_info *r,
				 u32 *nxt_res)
{
	u8 res_func = ulp_flow_db_resource_func_get(r);

	netdev_dbg(ulp_ctxt->bp->dev, "Resource func = %x, nxt_resource_idx = %x\n",
		   res_func, (ULP_FLOW_DB_RES_NXT_MASK & r->nxt_resource_idx));
	if (res_func == BNXT_ULP_RESOURCE_FUNC_EM_TABLE)
		netdev_dbg(ulp_ctxt->bp->dev, "EM Handle = %llu\n", r->resource_em_handle);
	else
		netdev_dbg(ulp_ctxt->bp->dev, "Handle = 0x%08x\n", r->resource_hndl);

	*nxt_res = 0;
	ULP_FLOW_DB_RES_NXT_SET(*nxt_res,
				r->nxt_resource_idx);
}

/**
 * Dump the flow entry details
 *
 * @flow_db: Ptr to flow db
 * @fid: flow id
 *
 * returns none
 */
void
ulp_flow_db_debug_fid_dump(struct bnxt_ulp_context *ulp_ctxt,
			   struct bnxt_ulp_flow_db *flow_db, u32 fid)
{
	struct bnxt_ulp_flow_tbl *flow_tbl;
	struct ulp_fdb_resource_info *r;
	u32 def_flag = 0, reg_flag = 0;
	u32 nxt_res = 0;

	flow_tbl = &flow_db->flow_tbl;
	if (ulp_flow_db_active_flows_bit_is_set(flow_db,
						BNXT_ULP_FDB_TYPE_REGULAR, fid))
		reg_flag = 1;
	if (ulp_flow_db_active_flows_bit_is_set(flow_db,
						BNXT_ULP_FDB_TYPE_DEFAULT, fid))
		def_flag = 1;

	if (reg_flag && def_flag) {
		netdev_dbg(ulp_ctxt->bp->dev, "RID = %u\n", fid);
	} else if (reg_flag) {
		netdev_dbg(ulp_ctxt->bp->dev, "Regular fid = %u and func id = %u\n",
			   fid, flow_db->func_id_tbl[fid]);
	} else if (def_flag) {
		netdev_dbg(ulp_ctxt->bp->dev, "Default fid = %u\n", fid);
	} else {
		return;
	}
	/* iterate the resource */
	nxt_res = fid;
	do {
		r = &flow_tbl->flow_resources[nxt_res];
		ulp_flow_db_res_dump(ulp_ctxt, r, &nxt_res);
	} while (nxt_res);
}

/**
 * Dump the flow database entry details
 *
 * @ulp_ctxt: Ptr to ulp_context
 * @flow_id: if zero then all fids are dumped.
 *
 * returns none
 */
int ulp_flow_db_debug_dump(struct bnxt_ulp_context *ulp_ctxt, u32 flow_id)
{
	struct bnxt_ulp_flow_tbl *flow_tbl;
	struct bnxt_ulp_flow_db *flow_db;
	u32 fid;

	if (!ulp_ctxt || !ulp_ctxt->cfg_data)
		return -EINVAL;

	flow_db = bnxt_ulp_cntxt_ptr2_flow_db_get(ulp_ctxt);
	if (!flow_db) {
		netdev_dbg(ulp_ctxt->bp->dev, "Invalid Arguments\n");
		return -EINVAL;
	}

	flow_tbl = &flow_db->flow_tbl;
	if (flow_id) {
		ulp_flow_db_debug_fid_dump(ulp_ctxt, flow_db, flow_id);
		return 0;
	}

	netdev_dbg(ulp_ctxt->bp->dev, "Dump flows = %u:%u\n",
		   flow_tbl->num_flows,
		   flow_tbl->num_resources);
	netdev_dbg(ulp_ctxt->bp->dev, "Head_index = %u, Tail_index = %u\n",
		   flow_tbl->head_index, flow_tbl->tail_index);
	for (fid = 1; fid < flow_tbl->num_flows; fid++)
		ulp_flow_db_debug_fid_dump(ulp_ctxt, flow_db, fid);
	netdev_dbg(ulp_ctxt->bp->dev, "Done.\n");
	return 0;
}

#else /* TC_BNXT_TRUFLOW_DEBUG */

void ulp_flow_db_debug_fid_dump(struct bnxt_ulp_context *ulp_ctxt,
				struct bnxt_ulp_flow_db *flow_db, u32 fid)
{
}

int ulp_flow_db_debug_dump(struct bnxt_ulp_context *ulp_ctxt, u32 flow_id)
{
	return 0;
}

#endif	/* TC_BNXT_TRUFLOW_DEBUG */
#endif /* CONFIG_BNXT_FLOWER_OFFLOAD */
