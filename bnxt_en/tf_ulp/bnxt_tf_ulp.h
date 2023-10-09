/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2019-2021 Broadcom
 * All rights reserved.
 */

#ifndef _BNXT_ULP_H_
#define _BNXT_ULP_H_

#include <linux/stddef.h>

#include <linux/mutex.h>

#include "bnxt.h"

#include "tf_core.h"
#include "ulp_template_db_enum.h"
#include "bnxt_tf_common.h"

/* NAT defines to reuse existing inner L2 SMAC and DMAC */
#define BNXT_ULP_NAT_INNER_L2_HEADER_SMAC	0x2000
#define BNXT_ULP_NAT_OUTER_MOST_L2_HDR_SMAC	0x6000
#define BNXT_ULP_NAT_OUTER_MOST_L2_VLAN_TAGS	0xc00
#define BNXT_ULP_NAT_INNER_L2_HEADER_DMAC	0x100
#define BNXT_ULP_NAT_OUTER_MOST_L2_HDR_DMAC	0x300
#define BNXT_ULP_NAT_OUTER_MOST_FLAGS (BNXT_ULP_NAT_OUTER_MOST_L2_HDR_SMAC |\
					BNXT_ULP_NAT_OUTER_MOST_L2_VLAN_TAGS |\
					BNXT_ULP_NAT_OUTER_MOST_L2_HDR_DMAC)

/* defines for the ulp_flags */
#define BNXT_ULP_VF_REP_ENABLED		0x1
#define BNXT_ULP_SHARED_SESSION_ENABLED 0x2
#define BNXT_ULP_APP_DEV_UNSUPPORTED	0x4
#define BNXT_ULP_HIGH_AVAIL_ENABLED	0x8
#define BNXT_ULP_APP_UNICAST_ONLY	0x10
#define ULP_VF_REP_IS_ENABLED(flag)	((flag) & BNXT_ULP_VF_REP_ENABLED)
#define ULP_SHARED_SESSION_IS_ENABLED(flag) ((flag) &\
					     BNXT_ULP_SHARED_SESSION_ENABLED)
#define ULP_APP_DEV_UNSUPPORTED_ENABLED(flag)	((flag) &\
						 BNXT_ULP_APP_DEV_UNSUPPORTED)
#define ULP_HIGH_AVAIL_IS_ENABLED(flag)	((flag) & BNXT_ULP_HIGH_AVAIL_ENABLED)

enum bnxt_ulp_flow_mem_type {
	BNXT_ULP_FLOW_MEM_TYPE_INT = 0,
	BNXT_ULP_FLOW_MEM_TYPE_EXT = 1,
	BNXT_ULP_FLOW_MEM_TYPE_BOTH = 2,
	BNXT_ULP_FLOW_MEM_TYPE_LAST = 3
};

enum bnxt_session_type {
	BNXT_SESSION_TYPE_REGULAR = 0,
	BNXT_SESSION_TYPE_SHARED_COMMON,
	BNXT_SESSION_TYPE_SHARED_WC,
	BNXT_SESSION_TYPE_LAST
};

static inline struct tf *bnxt_get_tfp_session(struct bnxt *bp,
					      enum bnxt_session_type type)
{
	struct tf *tfp = bp->tfp;

	if (!tfp)
		return NULL;

	return (type >= BNXT_SESSION_TYPE_LAST) ?
		&tfp[BNXT_SESSION_TYPE_REGULAR] : &tfp[type];
}

struct bnxt_ulp_df_rule_info {
	u32			def_port_flow_id;
	u8				valid;
};

struct bnxt_ulp_vfr_rule_info {
	u32			vfr_flow_id;
	u16			parent_port_id;
	u8				valid;
};

struct bnxt_ulp_data {
	u32				tbl_scope_id;
	struct bnxt_ulp_mark_tbl	*mark_tbl;
	u32				ref_cnt;
	struct bnxt_ulp_flow_db		*flow_db;
	/* Serialize flow db operations */
	struct mutex			flow_db_lock;
	void				*mapper_data;
	void				*matcher_data;
	struct bnxt_ulp_port_db		*port_db;
	struct bnxt_ulp_fc_info		*fc_info;
	struct bnxt_ulp_ha_mgr_info	*ha_info;
	u32				ulp_flags;
#define	BNXT_TC_MAX_PORTS 1024
	struct bnxt_ulp_df_rule_info	df_rule_info[BNXT_TC_MAX_PORTS];
	struct bnxt_ulp_vfr_rule_info	vfr_rule_info[BNXT_TC_MAX_PORTS];
	enum bnxt_ulp_flow_mem_type	mem_type;
#define	BNXT_ULP_TUN_ENTRY_INVALID	-1
#define	BNXT_ULP_MAX_TUN_CACHE_ENTRIES	16
	bool				accum_stats;
	u8				app_id;
	u8				num_shared_clients;
	u8				wc_num_slices;
	struct delayed_work		fc_work;
};

#define BNXT_ULP_SESSION_MAX 3
struct bnxt_ulp_context {
	struct bnxt_ulp_data	*cfg_data;
	struct tf		*g_tfp[BNXT_ULP_SESSION_MAX];
	struct tf		*g_shared_tfp;
	struct bnxt		*bp;
};

struct bnxt_ulp_pci_info {
	u32	domain;
	u8		bus;
};

struct bnxt_ulp_session_state {
	struct hlist_node			next;
	bool					bnxt_ulp_init;
	/* Serialize session operations */
	u8					dsn[8];
	struct bnxt_ulp_data			*cfg_data;
	struct tf				*g_tfp[BNXT_ULP_SESSION_MAX];
	struct tf				g_shared_tfp;
	u32				session_opened[BNXT_ULP_SESSION_MAX];
};

/* ULP flow id structure */
struct tc_tf_flow {
	u32	flow_id;
};

struct ulp_tlv_param {
	enum bnxt_ulp_df_param_type type;
	u32 length;
	u8 value[16];
};

struct ulp_context_list_entry {
	struct hlist_node			next;
	struct bnxt_ulp_context			*ulp_ctx;
};

/* Allow the deletion of context only for the bnxt device that
 * created the session
 */
bool
ulp_ctx_deinit_allowed(struct bnxt_ulp_context *ulp_ctx);

/* Function to get the device id of the hardware. */
int
bnxt_ulp_devid_get(struct bnxt *bp);

/* Function to get whether or not ext mem is used for EM */
int
bnxt_ulp_cntxt_mem_type_get(struct bnxt_ulp_context *ulp_ctx,
			    enum bnxt_ulp_flow_mem_type *mem_type);

/* Function to set whether or not ext mem is used for EM */
int
bnxt_ulp_cntxt_mem_type_set(struct bnxt_ulp_context *ulp_ctx,
			    enum bnxt_ulp_flow_mem_type mem_type);

/* Function to set the table scope id of the EEM table. */
int
bnxt_ulp_cntxt_tbl_scope_id_set(struct bnxt_ulp_context *ulp_ctx,
				u32 tbl_scope_id);

/* Function to get the table scope id of the EEM table. */
int
bnxt_ulp_cntxt_tbl_scope_id_get(struct bnxt_ulp_context *ulp_ctx,
				u32 *tbl_scope_id);

/* Function to set the tfp session details in the ulp context. */
int
bnxt_ulp_cntxt_shared_tfp_set(struct bnxt_ulp_context *ulp, struct tf *tfp);

/* Function to get the tfp session details from ulp context. */
struct tf *
bnxt_ulp_cntxt_shared_tfp_get(struct bnxt_ulp_context *ulp);

/* Function to set the tfp session details in the ulp context. */
int
bnxt_ulp_cntxt_tfp_set(struct bnxt_ulp_context *ulp,
		       enum bnxt_ulp_session_type s_type, struct tf *tfp);

/* Function to get the tfp session details from ulp context. */
struct tf *
bnxt_ulp_cntxt_tfp_get(struct bnxt_ulp_context *ulp,
		       enum bnxt_ulp_session_type s_type);

/* Get the device table entry based on the device id. */
struct bnxt_ulp_device_params *
bnxt_ulp_device_params_get(u32 dev_id);

int
bnxt_ulp_cntxt_ptr2_mark_db_set(struct bnxt_ulp_context *ulp_ctx,
				struct bnxt_ulp_mark_tbl *mark_tbl);

struct bnxt_ulp_mark_tbl *
bnxt_ulp_cntxt_ptr2_mark_db_get(struct bnxt_ulp_context *ulp_ctx);

/* Function to set the flow database to the ulp context. */
int
bnxt_ulp_cntxt_ptr2_flow_db_set(struct bnxt_ulp_context	*ulp_ctx,
				struct bnxt_ulp_flow_db	*flow_db);

/* Function to get the flow database from the ulp context. */
struct bnxt_ulp_flow_db	*
bnxt_ulp_cntxt_ptr2_flow_db_get(struct bnxt_ulp_context	*ulp_ctx);

/* Function to get the ulp context from eth device. */
struct bnxt_ulp_context	*
bnxt_ulp_bp_ptr2_cntxt_get(struct bnxt *bp);

/* Function to add the ulp mapper data to the ulp context */
int
bnxt_ulp_cntxt_ptr2_mapper_data_set(struct bnxt_ulp_context *ulp_ctx,
				    void *mapper_data);

/* Function to get the ulp mapper data from the ulp context */
void *
bnxt_ulp_cntxt_ptr2_mapper_data_get(struct bnxt_ulp_context *ulp_ctx);

/* Function to add the ulp matcher data to the ulp context */
int
bnxt_ulp_cntxt_ptr2_matcher_data_set(struct bnxt_ulp_context *ulp_ctx,
				     void *matcher_data);

/* Function to get the ulp matcher data from the ulp context */
void *
bnxt_ulp_cntxt_ptr2_matcher_data_get(struct bnxt_ulp_context *ulp_ctx);

/* Function to set the port database to the ulp context. */
int
bnxt_ulp_cntxt_ptr2_port_db_set(struct bnxt_ulp_context	*ulp_ctx,
				struct bnxt_ulp_port_db	*port_db);

/* Function to get the port database from the ulp context. */
struct bnxt_ulp_port_db *
bnxt_ulp_cntxt_ptr2_port_db_get(struct bnxt_ulp_context	*ulp_ctx);

int bnxt_ulp_create_df_rules(struct bnxt *bp);
void bnxt_ulp_destroy_df_rules(struct bnxt *bp, bool global);

/* Function to create default flows. */
int
ulp_default_flow_create(struct bnxt *bp,
			struct ulp_tlv_param *param_list,
			u32 ulp_class_tid,
			u16 port_id,
			u32 *flow_id);

/* Function to destroy default flows. */
int
ulp_default_flow_destroy(struct bnxt *bp,
			 u32 flow_id);

int
bnxt_ulp_cntxt_ptr2_fc_info_set(struct bnxt_ulp_context *ulp_ctx,
				struct bnxt_ulp_fc_info *ulp_fc_info);

struct bnxt_ulp_fc_info *
bnxt_ulp_cntxt_ptr2_fc_info_get(struct bnxt_ulp_context *ulp_ctx);

int
bnxt_ulp_cntxt_ptr2_ulp_flags_get(struct bnxt_ulp_context *ulp_ctx,
				  u32 *flags);

int
bnxt_ulp_get_df_rule_info(u8 port_id, struct bnxt_ulp_context *ulp_ctx,
			  struct bnxt_ulp_df_rule_info *info);

struct bnxt_ulp_vfr_rule_info*
bnxt_ulp_cntxt_ptr2_ulp_vfr_info_get(struct bnxt_ulp_context *ulp_ctx,
				     u32 port_id);

struct bnxt_ulp_glb_resource_info *
bnxt_ulp_app_glb_resource_info_list_get(u32 *num_entries);

int
bnxt_ulp_cntxt_app_id_set(struct bnxt_ulp_context *ulp_ctx, u8 app_id);

int
bnxt_ulp_cntxt_app_id_get(struct bnxt_ulp_context *ulp_ctx, u8 *app_id);

bool
bnxt_ulp_cntxt_shared_session_enabled(struct bnxt_ulp_context *ulp_ctx);

struct bnxt_ulp_app_capabilities_info *
bnxt_ulp_app_cap_list_get(u32 *num_entries);

int
bnxt_ulp_cntxt_app_caps_init(struct bnxt_ulp_context *ulp_ctx,
			     u8 app_id, u32 dev_id);

struct bnxt_ulp_resource_resv_info *
bnxt_ulp_resource_resv_list_get(u32 *num_entries);

bool
bnxt_ulp_cntxt_ha_enabled(struct bnxt_ulp_context *ulp_ctx);

struct bnxt_ulp_context *
bnxt_ulp_cntxt_entry_lookup(void *cfg_data);

void
bnxt_ulp_cntxt_lock_acquire(void);

void
bnxt_ulp_cntxt_lock_release(void);

u8
bnxt_ulp_cntxt_num_shared_clients_get(struct bnxt_ulp_context *ulp_ctx);

int
bnxt_ulp_cntxt_ptr2_wc_num_slices_set(struct bnxt_ulp_context *ulp_ctx,
				      u8 wc_num_slices);
unsigned int
bnxt_ulp_cntxt_ptr2_wc_num_slices_get(struct bnxt_ulp_context *ulp_ctx);

/* Function to check if allowing multicast and broadcast flow offload. */
bool
bnxt_ulp_validate_bcast_mcast(struct bnxt *bp);

#endif /* _BNXT_ULP_H_ */
