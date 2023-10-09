/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2019-2021 Broadcom
 * All rights reserved.
 */

#ifndef _ULP_FLOW_DB_H_
#define _ULP_FLOW_DB_H_

#include "bnxt_tf_ulp.h"
#include "ulp_template_db_enum.h"
#include "ulp_mapper.h"

#define BNXT_FLOW_DB_DEFAULT_NUM_FLOWS		512
#define BNXT_FLOW_DB_DEFAULT_NUM_RESOURCES	8

/* Defines for the fdb flag */
#define ULP_FDB_FLAG_SHARED_SESSION	0x1

/**
 * Structure for the flow database resource information
 * The below structure is based on the below paritions
 * nxt_resource_idx = dir[31],resource_func_upper[30:28],nxt_resource_idx[27:0]
 * If resource_func is EM_TBL then use resource_em_handle.
 * Else the other part of the union is used and
 * resource_func is resource_func_upper[30:28] << 5 | resource_func_lower
 */
struct ulp_fdb_resource_info {
	/* Points to next resource in the chained list. */
	u32			nxt_resource_idx;
	union {
		u64		resource_em_handle;
		struct {
			u8		resource_func_lower;
			u8		resource_type;
			u8		resource_sub_type;
			u8		fdb_flags;
			u32		resource_hndl;
			u8		*key_data;
		};
	};
};

/* Structure for the flow database resource information. */
struct bnxt_ulp_flow_tbl {
	/* Flow tbl is the resource object list for each flow id. */
	struct ulp_fdb_resource_info	*flow_resources;

	/* Flow table stack to track free list of resources. */
	u32	*flow_tbl_stack;
	u32	head_index;
	u32	tail_index;

	/* Table to track the active flows. */
	u64	*active_reg_flows;
	u64	*active_dflt_flows;
	u32	num_flows;
	u32	num_resources;
};

/* Structure for the flow database resource information. */
struct bnxt_ulp_flow_db {
	struct bnxt_ulp_flow_tbl	flow_tbl;
	u16			*func_id_tbl;
	u32			func_id_tbl_size;
};

/* flow db resource params to add resources */
struct ulp_flow_db_res_params {
	enum tf_dir			direction;
	enum bnxt_ulp_resource_func	resource_func;
	u8				resource_type;
	u8				resource_sub_type;
	u8				fdb_flags;
	u8				critical_resource;
	u8				*key_data;
	u64				resource_hndl;
};

int	ulp_flow_db_init(struct bnxt_ulp_context *ulp_ctxt);

int	ulp_flow_db_deinit(struct bnxt_ulp_context *ulp_ctxt);

int
ulp_flow_db_fid_alloc(struct bnxt_ulp_context *ulp_ctxt,
		      enum bnxt_ulp_fdb_type flow_type,
		      u16 func_id,
		      u32 *fid);

int
ulp_flow_db_resource_add(struct bnxt_ulp_context *ulp_ctxt,
			 enum bnxt_ulp_fdb_type flow_type,
			 u32 fid,
			 struct ulp_flow_db_res_params *params);

int
ulp_flow_db_resource_del(struct bnxt_ulp_context *ulp_ctxt,
			 enum bnxt_ulp_fdb_type flow_type,
			 u32 fid,
			 struct ulp_flow_db_res_params *params);

int
ulp_flow_db_fid_free(struct bnxt_ulp_context *ulp_ctxt,
		     enum bnxt_ulp_fdb_type tbl_idx,
		     u32 fid);

int
ulp_flow_db_resource_get(struct bnxt_ulp_context *ulp_ctxt,
			 enum bnxt_ulp_fdb_type flow_type,
			 u32 fid,
			 u32 *nxt_idx,
			 struct ulp_flow_db_res_params *params);

int
ulp_flow_db_flush_flows(struct bnxt_ulp_context *ulp_ctx,
			u32 idx);

int
ulp_flow_db_function_flow_flush(struct bnxt_ulp_context *ulp_ctx,
				u16 func_id);

int
ulp_flow_db_session_flow_flush(struct bnxt_ulp_context *ulp_ctx);

int
ulp_flow_db_validate_flow_func(struct bnxt_ulp_context *ulp_ctx,
			       u32 flow_id,
			       u32 func_id);

int
ulp_default_flow_db_cfa_action_get(struct bnxt_ulp_context *ulp_ctx,
				   u32 flow_id,
				   u16 *cfa_action);

void ulp_flow_db_shared_session_set(struct ulp_flow_db_res_params *res,
				    enum bnxt_ulp_session_type s_type);

enum bnxt_ulp_session_type
ulp_flow_db_shared_session_get(struct ulp_flow_db_res_params *res);

#ifdef TC_BNXT_TRUFLOW_DEBUG
void
ulp_flow_db_debug_fid_dump(struct bnxt_ulp_context *ulp_ctx,
			   struct bnxt_ulp_flow_db *flow_db, u32 fid);

int	ulp_flow_db_debug_dump(struct bnxt_ulp_context *ulp_ctxt,
			       u32 flow_id);
#endif

#endif /* _ULP_FLOW_DB_H_ */
