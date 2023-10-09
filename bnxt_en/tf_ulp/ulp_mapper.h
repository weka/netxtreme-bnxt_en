/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2019-2021 Broadcom
 * All rights reserved.
 */

#ifndef _ULP_MAPPER_H_
#define _ULP_MAPPER_H_

#include "tf_core.h"
#include "ulp_template_db_enum.h"
#include "ulp_template_struct.h"
#include "bnxt_tf_ulp.h"
#include "ulp_utils.h"
#include "ulp_gen_tbl.h"

#define ULP_IDENTS_INVALID ((u16)0xffff)

struct bnxt_ulp_mapper_glb_resource_entry {
	enum bnxt_ulp_resource_func	resource_func;
	u32			resource_type; /* TF_ enum type */
	u64			resource_hndl;
	bool				shared;
};

struct bnxt_ulp_mapper_data {
	struct bnxt_ulp_mapper_glb_resource_entry
		glb_res_tbl[TF_DIR_MAX][BNXT_ULP_GLB_RF_IDX_LAST];
	struct ulp_mapper_gen_tbl_list gen_tbl_list[BNXT_ULP_GEN_TBL_MAX_SZ];
};

/* Internal Structure for passing the arguments around */
struct bnxt_ulp_mapper_parms {
	enum bnxt_ulp_template_type		tmpl_type;
	u32				dev_id;
	u32				act_tid;
	struct bnxt_ulp_mapper_tbl_info		*atbls; /* action table */
	u32				num_atbls;
	u32				class_tid;
	struct bnxt_ulp_mapper_tbl_info		*ctbls; /* class table */
	u32				num_ctbls;
	struct ulp_tc_act_prop			*act_prop;
	struct ulp_tc_act_bitmap		*act_bitmap;
	struct ulp_tc_hdr_bitmap		*hdr_bitmap;
	struct ulp_tc_hdr_bitmap		*enc_hdr_bitmap;
	struct ulp_tc_hdr_field		*hdr_field;
	struct ulp_tc_hdr_field		*enc_field;
	struct ulp_tc_field_bitmap		*fld_bitmap;
	u64					*comp_fld;
	struct ulp_regfile			*regfile;
	struct bnxt_ulp_context			*ulp_ctx;
	u32				fid;
	enum bnxt_ulp_fdb_type			flow_type;
	struct bnxt_ulp_mapper_data		*mapper_data;
	struct bnxt_ulp_device_params		*device_params;
	u8					tun_idx;
	u32				app_priority;
	u64				shared_hndl;
	u32				flow_pattern_id;
	u32				act_pattern_id;
	u8					app_id;
	u16				port_id;
};

struct bnxt_ulp_mapper_create_parms {
	u32				app_priority;
	struct ulp_tc_hdr_bitmap	*hdr_bitmap;
	struct ulp_tc_hdr_bitmap	*enc_hdr_bitmap;
	struct ulp_tc_hdr_field		*hdr_field;
	struct ulp_tc_hdr_field		*enc_field;
	u64				*comp_fld;
	struct ulp_tc_act_bitmap	*act;
	struct ulp_tc_act_prop		*act_prop;
	struct ulp_tc_field_bitmap	*fld_bitmap;
	u32				class_tid;
	u32				act_tid;
	u16				func_id;
	u32				dir_attr;
	enum bnxt_ulp_fdb_type		flow_type;

	u32				flow_id;
	u8				tun_idx;
	u64				shared_hndl;

	void                            *tnl_key;
	void                            *neigh_key;

	/* support pattern based rejection */
	u32				flow_pattern_id;
	u32				act_pattern_id;
	u8				app_id;
	u16				port_id;
};

/* Function to initialize any dynamic mapper data. */
int
ulp_mapper_init(struct bnxt_ulp_context	*ulp_ctx);

/* Function to release all dynamic mapper data. */
void
ulp_mapper_deinit(struct bnxt_ulp_context *ulp_ctx);

/* Function to handle the mapping of the Flow to be compatible
 * with the underlying hardware.
 */
int
ulp_mapper_flow_create(struct bnxt_ulp_context	*ulp_ctx,
		       struct bnxt_ulp_mapper_create_parms *parms);

/* Function that frees all resources associated with the flow. */
int
ulp_mapper_flow_destroy(struct bnxt_ulp_context *ulp_ctx,
			enum bnxt_ulp_fdb_type flow_type,
			u32 fid);

/* Function that frees all resources and can be called on default or regular
 * flows
 */
int
ulp_mapper_resources_free(struct bnxt_ulp_context *ulp_ctx,
			  enum bnxt_ulp_fdb_type flow_type,
			  u32 fid);

int
ulp_mapper_get_shared_fid(struct bnxt_ulp_context *ulp,
			  u32 id,
			  u16 key,
			  u32 *fid);

#endif /* _ULP_MAPPER_H_ */
