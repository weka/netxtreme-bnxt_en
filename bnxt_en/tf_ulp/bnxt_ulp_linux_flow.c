// SPDX-License-Identifier: BSD-3-Clause
/* Copyright(c) 2019-2021 Broadcom
 * All rights reserved.
 */

#include "bnxt_compat.h"
#include "bnxt_hsi.h"
#include "bnxt.h"
#include "bnxt_tf_common.h"
#include "bnxt_ulp_flow.h"
#include "ulp_tc_parser.h"
#include "ulp_matcher.h"
#include "ulp_flow_db.h"
#include "ulp_mapper.h"
#include "ulp_fc_mgr.h"
#include "ulp_port_db.h"
#include "ulp_template_debug_proto.h"

#ifdef CONFIG_BNXT_FLOWER_OFFLOAD
static inline void bnxt_ulp_set_dir_attributes(struct bnxt *bp,
					       struct ulp_tc_parser_params
					       *params, u16 src_fid)
{
	/* Set the flow attributes.
	 * TBD: This logic might need some port-process fixing for the
	 * vxlan-decap case.
	 */
	if (bp->pf.fw_fid == src_fid)
		params->dir_attr |= BNXT_ULP_FLOW_ATTR_INGRESS;
	else
		params->dir_attr |= BNXT_ULP_FLOW_ATTR_EGRESS;
}

void bnxt_ulp_init_mapper_params(struct bnxt_ulp_mapper_create_parms
				 *mapper_cparms, struct ulp_tc_parser_params
				 *params, enum bnxt_ulp_fdb_type flow_type)
{
	memset(mapper_cparms, 0, sizeof(*mapper_cparms));

	mapper_cparms->flow_type = flow_type;
	mapper_cparms->app_priority = params->priority;
	mapper_cparms->dir_attr = params->dir_attr;
	mapper_cparms->class_tid = params->class_id;
	mapper_cparms->act_tid = params->act_tmpl;
	mapper_cparms->func_id = params->func_id;
	mapper_cparms->hdr_bitmap = &params->hdr_bitmap;
	mapper_cparms->enc_hdr_bitmap = &params->enc_hdr_bitmap;
	mapper_cparms->hdr_field = params->hdr_field;
	mapper_cparms->enc_field = params->enc_field;
	mapper_cparms->comp_fld = params->comp_fld;
	mapper_cparms->act = &params->act_bitmap;
	mapper_cparms->act_prop = &params->act_prop;
	mapper_cparms->flow_id = params->fid;
	mapper_cparms->fld_bitmap = &params->fld_bitmap;
	mapper_cparms->flow_pattern_id = params->flow_pattern_id;
	mapper_cparms->act_pattern_id = params->act_pattern_id;
	mapper_cparms->app_id = params->app_id;
	mapper_cparms->port_id = params->port_id;

	/* update the signature fields into the computed field list */
	ULP_COMP_FLD_IDX_WR(params, BNXT_ULP_CF_IDX_HDR_SIG_ID,
			    params->class_info_idx);
	ULP_COMP_FLD_IDX_WR(params, BNXT_ULP_CF_IDX_FLOW_SIG_ID,
			    params->flow_sig_id);
}

int
bnxt_ulp_alloc_mapper_encap_cparams(struct bnxt_ulp_mapper_create_parms **cparms_dyn,
				    struct bnxt_ulp_mapper_create_parms *cparms)
{
	struct bnxt_ulp_mapper_create_parms *parms = NULL;

	parms = vzalloc(sizeof(*parms));
	if (!parms)
		goto err;
	memcpy(parms, cparms, sizeof(*parms));

	parms->hdr_bitmap = vzalloc(sizeof(*parms->hdr_bitmap));
	if (!parms->hdr_bitmap)
		goto err_cparm;

	parms->enc_hdr_bitmap = vzalloc(sizeof(*parms->enc_hdr_bitmap));
	if (!parms->enc_hdr_bitmap)
		goto err_hdr_bitmap;

	parms->hdr_field =  vzalloc(sizeof(*parms->hdr_field) * BNXT_ULP_PROTO_HDR_MAX);
	if (!parms->hdr_field)
		goto err_enc_hdr_bitmap;

	parms->enc_field =  vzalloc(sizeof(*parms->enc_field) * BNXT_ULP_PROTO_HDR_ENCAP_MAX);
	if (!parms->enc_field)
		goto err_hdr_field;

	parms->comp_fld = vzalloc(sizeof(*parms->comp_fld) * BNXT_ULP_CF_IDX_LAST);
	if (!parms->comp_fld)
		goto err_enc_field;

	parms->act = vzalloc(sizeof(*parms->act));
	if (!parms->act)
		goto err_comp_fld;

	parms->act_prop = vzalloc(sizeof(*parms->act_prop));
	if (!parms->act_prop)
		goto err_act;

	parms->fld_bitmap = vzalloc(sizeof(*parms->fld_bitmap));
	if (!parms->fld_bitmap)
		goto err_act_prop;

	memcpy(parms->hdr_bitmap, cparms->hdr_bitmap, sizeof(*parms->hdr_bitmap));
	memcpy(parms->enc_hdr_bitmap, cparms->enc_hdr_bitmap,
	       sizeof(*parms->enc_hdr_bitmap));
	memcpy(parms->hdr_field, cparms->hdr_field,
	       sizeof(*parms->hdr_field) * BNXT_ULP_PROTO_HDR_MAX);
	memcpy(parms->enc_field, cparms->enc_field,
	       sizeof(*parms->enc_field) * BNXT_ULP_PROTO_HDR_ENCAP_MAX);
	memcpy(parms->comp_fld, cparms->comp_fld,
	       sizeof(*parms->comp_fld) * BNXT_ULP_CF_IDX_LAST);
	memcpy(parms->act, cparms->act, sizeof(*parms->act));
	memcpy(parms->act_prop, cparms->act_prop, sizeof(*parms->act_prop));
	memcpy(parms->fld_bitmap, cparms->fld_bitmap, sizeof(*parms->fld_bitmap));

	*cparms_dyn = parms;
	return 0;

err_act_prop:
	vfree(parms->act_prop);
err_act:
	vfree(parms->act);
err_comp_fld:
	vfree(parms->comp_fld);
err_enc_field:
	vfree(parms->enc_field);
err_hdr_field:
	vfree(parms->hdr_field);
err_enc_hdr_bitmap:
	vfree(parms->enc_hdr_bitmap);
err_hdr_bitmap:
	vfree(parms->hdr_bitmap);
err_cparm:
	vfree(parms);
err:
	return -ENOMEM;
}

void bnxt_ulp_free_mapper_encap_cparams(void *mapper_cparms)
{
	struct bnxt_ulp_mapper_create_parms *parms = mapper_cparms;

	vfree(parms->act_prop);
	vfree(parms->act);
	vfree(parms->comp_fld);
	vfree(parms->enc_field);
	vfree(parms->hdr_field);
	vfree(parms->enc_hdr_bitmap);
	vfree(parms->hdr_bitmap);
	vfree(parms);
}

/* Function to create the ulp flow. */
int bnxt_ulp_flow_create(struct bnxt *bp, u16 src_fid,
			 struct flow_cls_offload *tc_flow_cmd,
			 struct bnxt_ulp_flow_info *flow_info)
{
	struct bnxt_ulp_mapper_create_parms *mapper_encap_cparms = NULL;
	struct bnxt_ulp_mapper_create_parms mapper_cparms = { 0 };
	struct ulp_tc_parser_params *params = NULL;
	struct bnxt_ulp_context *ulp_ctx;
	int rc, ret = BNXT_TF_RC_ERROR;
	u32 chain_index;
	u16 func_id;
	u32 fid;

	ulp_ctx = bnxt_ulp_bp_ptr2_cntxt_get(bp);
	if (!ulp_ctx) {
		netdev_dbg(bp->dev, "ULP context is not initialized\n");
		goto flow_error;
	}

	/* Initialize the parser params */
	params = vzalloc(sizeof(*params));
	params->ulp_ctx = ulp_ctx;

	if (bnxt_ulp_cntxt_app_id_get(params->ulp_ctx, &params->app_id)) {
		netdev_dbg(bp->dev, "failed to get the app id\n");
		goto flow_error;
	}

	/* Set the flow attributes */
	bnxt_ulp_set_dir_attributes(bp, params, src_fid);

	/* copy the device port id and direction for further processing */
	ULP_COMP_FLD_IDX_WR(params, BNXT_ULP_CF_IDX_INCOMING_IF, src_fid);
	ULP_COMP_FLD_IDX_WR(params, BNXT_ULP_CF_IDX_DEV_PORT_ID, src_fid);
	ULP_COMP_FLD_IDX_WR(params, BNXT_ULP_CF_IDX_SVIF_FLAG,
			    BNXT_ULP_INVALID_SVIF_VAL);

	/* Get the function id */
	if (ulp_port_db_port_func_id_get(ulp_ctx, src_fid, &func_id)) {
		netdev_dbg(bp->dev, "conversion of port to func id failed\n");
		goto flow_error;
	}

	/* Protect flow creation */
	mutex_lock(&ulp_ctx->cfg_data->flow_db_lock);

	/* Allocate a Flow ID to attach all resources for the flow.
	 * Once allocated, all errors have to walk the list of resources and
	 * free each of them.
	 */
	rc = ulp_flow_db_fid_alloc(ulp_ctx, BNXT_ULP_FDB_TYPE_REGULAR,
				   func_id, &fid);
	if (rc) {
		netdev_dbg(bp->dev, "Unable to allocate flow table entry\n");
		goto release_lock;
	}

	/* Parse the rte flow pattern */
	ret = bnxt_ulp_tc_parser_hdr_parse(bp, tc_flow_cmd, params);
	if (ret != BNXT_TF_RC_SUCCESS)
		goto free_fid;

	/* Parse the rte flow action */
	ret = bnxt_ulp_tc_parser_act_parse(bp, tc_flow_cmd, params);
	if (ret != BNXT_TF_RC_SUCCESS)
		goto free_fid;

	params->fid = fid;
	params->func_id = func_id;
	params->priority = tc_flow_cmd->common.prio;
	chain_index = tc_flow_cmd->common.chain_index;
	if (chain_index) {
		if (chain_index > 4095) {
			netdev_dbg(bp->dev, "%s: Invalid match chain: %u\n",
				   __func__, chain_index);
			return BNXT_TF_RC_ERROR;
		}
		ULP_COMP_FLD_IDX_WR(params, BNXT_ULP_CF_IDX_CHAIN_ID_METADATA,
				    (ULP_THOR_SYM_CHAIN_META_VAL +
				     (0xfff & chain_index)));
		netdev_dbg(bp->dev, "%s: Chain metadata: 0x%x chain: %u\n",
			   __func__,
			   (chain_index + ULP_THOR_SYM_CHAIN_META_VAL),
			   chain_index);
	}
	params->match_chain_id = chain_index;

	netdev_dbg(bp->dev, "Flow prio: %u chain: %u\n",
		   params->priority, params->match_chain_id);

	params->port_id = src_fid;
	/* Perform the rte flow post process */
	ret = bnxt_ulp_tc_parser_post_process(params);
	if (ret == BNXT_TF_RC_ERROR)
		goto free_fid;
	else if (ret == BNXT_TF_RC_FID)
		goto return_fid;

	/* Dump the rte flow pattern */
	ulp_parser_hdr_info_dump(params);
	/* Dump the rte flow action */
	ulp_parser_act_info_dump(params);

	ret = ulp_matcher_pattern_match(params, &params->class_id);
	if (ret != BNXT_TF_RC_SUCCESS)
		goto free_fid;

	ret = ulp_matcher_action_match(params, &params->act_tmpl);
	if (ret != BNXT_TF_RC_SUCCESS)
		goto free_fid;

	bnxt_ulp_init_mapper_params(&mapper_cparms, params,
				    BNXT_ULP_FDB_TYPE_REGULAR);
	/* Call the ulp mapper to create the flow in the hardware. */
	ret = ulp_mapper_flow_create(ulp_ctx, &mapper_cparms);
	if (ret)
		goto free_fid;

	if (params->tnl_key) {
		ret = bnxt_ulp_alloc_mapper_encap_cparams(&mapper_encap_cparms,
							  &mapper_cparms);
		if (ret)
			goto mapper_destroy;
	}

return_fid:
	flow_info->flow_id = fid;
	if (params->tnl_key) {
		flow_info->cparms = mapper_encap_cparms;
		ether_addr_copy(flow_info->tnl_dmac, params->tnl_dmac);
		ether_addr_copy(flow_info->tnl_smac, params->tnl_smac);
		flow_info->tnl_ether_type = params->tnl_ether_type;
		flow_info->encap_key = params->tnl_key;
		flow_info->neigh_key = params->neigh_key;
	}
	vfree(params);
	mutex_unlock(&ulp_ctx->cfg_data->flow_db_lock);

	return 0;

mapper_destroy:
	ulp_mapper_flow_destroy(ulp_ctx, mapper_cparms.flow_type,
				mapper_cparms.flow_id);
free_fid:
	vfree(params->tnl_key);
	vfree(params->neigh_key);
	ulp_flow_db_fid_free(ulp_ctx, BNXT_ULP_FDB_TYPE_REGULAR, fid);
release_lock:
	mutex_unlock(&ulp_ctx->cfg_data->flow_db_lock);
flow_error:
	vfree(params);
	if (ret == -ENOSPC)
		return ret;
	else
		return (ret == BNXT_TF_RC_PARSE_ERR_NOTSUPP) ? -EOPNOTSUPP : -EIO;
}

/* Function to destroy the ulp flow. */
int bnxt_ulp_flow_destroy(struct bnxt *bp, u32 flow_id, u16 src_fid)
{
	struct bnxt_ulp_context *ulp_ctx;
	u16 func_id;
	int ret;

	ulp_ctx = bnxt_ulp_bp_ptr2_cntxt_get(bp);
	if (!ulp_ctx) {
		netdev_dbg(bp->dev, "ULP context is not initialized\n");
		return -ENOENT;
	}

	if (ulp_port_db_port_func_id_get(ulp_ctx, src_fid, &func_id)) {
		netdev_dbg(bp->dev, "Conversion of port to func id failed\n");
		return -EINVAL;
	}

	ret = ulp_flow_db_validate_flow_func(ulp_ctx, flow_id, func_id);
	if (ret)
		return ret;

	mutex_lock(&ulp_ctx->cfg_data->flow_db_lock);
	ret = ulp_mapper_flow_destroy(ulp_ctx, BNXT_ULP_FDB_TYPE_REGULAR,
				      flow_id);
	mutex_unlock(&ulp_ctx->cfg_data->flow_db_lock);

	return ret;
}

void bnxt_ulp_flow_query_count(struct bnxt *bp, u32 flow_id, u64 *packets,
			       u64 *bytes, unsigned long *lastused)
{
	ulp_tf_fc_mgr_query_count_get(bp->ulp_ctx, flow_id, packets, bytes,
				      lastused);
}

int
bnxt_ulp_update_flow_encap_record(struct bnxt *bp, u8 *tnl_dmac, void *mapper_cparms,
				  u32 *flow_id)
{
	struct bnxt_ulp_mapper_create_parms *cparms = mapper_cparms;
	struct bnxt_ulp_context *ulp_ctx = bp->ulp_ctx;
	struct ulp_tc_hdr_field *field;
	int ret;
	u32 local_flow_id;

	if (!cparms) {
		netdev_dbg(bp->dev, "Function %s: cparms pointer is NULL\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&ulp_ctx->cfg_data->flow_db_lock);
	ret = ulp_mapper_flow_destroy(bp->ulp_ctx, BNXT_ULP_FDB_TYPE_REGULAR,
				      *flow_id);
	if (ret)
		goto err;

	netdev_dbg(bp->dev, "Function %s: flow destroy successful\n", __func__);
	field = &cparms->enc_field[BNXT_ULP_ENC_FIELD_ETH_DMAC];
	memcpy(field->spec, tnl_dmac, ETH_ALEN);
	ret = ulp_flow_db_fid_alloc(ulp_ctx, BNXT_ULP_FDB_TYPE_REGULAR,
				    cparms->func_id, &local_flow_id);
	if (ret) {
		netdev_dbg(bp->dev, "Function %s: flow_id alloc failed\n", __func__);
		goto invalidate_flow_id;
	}
	*flow_id = local_flow_id;
	cparms->flow_id = local_flow_id;
	ret = ulp_mapper_flow_create(bp->ulp_ctx, cparms);
	if (!ret)
		goto done;
	netdev_dbg(bp->dev, "Function %s flow_create failed\n", __func__);
	ulp_flow_db_fid_free(ulp_ctx, BNXT_ULP_FDB_TYPE_REGULAR,
			     *flow_id);
invalidate_flow_id:
	/* flow_id == 0 means invalid flow id. Invalidate the flow_id
	 * when the flow creation under the hood fails, so that when
	 * the user deletes the flow, we will not try to delete it
	 * again in the hardware
	 */
	*flow_id = 0;
err:
done:
	mutex_unlock(&ulp_ctx->cfg_data->flow_db_lock);
	return ret;
}

bool bnxt_ulp_flow_chain_validate(struct bnxt *bp, u16 src_fid,
				  struct flow_cls_offload *tc_flow_cmd)
{
	u32 chain = tc_flow_cmd->common.chain_index;
	struct bnxt_ulp_context *ulp_ctx;
	u8 app_id;

	ulp_ctx = bnxt_ulp_bp_ptr2_cntxt_get(bp);
	if (!ulp_ctx) {
		netdev_dbg(bp->dev, "%s: ULP context is not initialized\n",
			   __func__);
		return false;
	}

	if (bnxt_ulp_cntxt_app_id_get(ulp_ctx, &app_id)) {
		netdev_dbg(bp->dev, "%s: Failed to get the app id\n", __func__);
		return false;
	}

	if (!chain)
		return true;

	/* non-zero chain */
	if (app_id != 0 && app_id != 1) {
		netdev_dbg(bp->dev,
			   "%s: Flow chaining is unsupported, app:%u chain:%u\n",
			   __func__, app_id, chain);
		return false;
	}
	return true;
}

#endif /* CONFIG_BNXT_FLOWER_OFFLOAD */
