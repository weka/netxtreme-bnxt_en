// SPDX-License-Identifier: BSD-3-Clause
/* Copyright(c) 2019-2021 Broadcom
 * All rights reserved.
 */

#include "bnxt_compat.h"
#include "ulp_matcher.h"
#include "ulp_utils.h"
#include "ulp_template_debug_proto.h"
#include <linux/vmalloc.h>


#if defined(CONFIG_BNXT_FLOWER_OFFLOAD) || defined(CONFIG_BNXT_CUSTOM_FLOWER_OFFLOAD)
static int ulp_matcher_class_list_lookup(struct ulp_tc_parser_params *params,
					 u32 *class_match_idx)
{
	struct bnxt_ulp_class_match_info *class_list = ulp_class_match_list;
	u32 idx = 0;

	while (++idx < BNXT_ULP_CLASS_MATCH_LIST_MAX_SZ) {
		/* iterate the list of class matches to find header match */
		if (class_list[idx].app_id == params->app_id &&
		    !ULP_BITMAP_CMP(&class_list[idx].hdr_bitmap,
				    &params->hdr_bitmap)) {
			/* Found the match */
			*class_match_idx = idx;
			return 0;
		}
	}

	netdev_dbg(params->ulp_ctx->bp->dev, "Did not find any matching protocol hdr\n");
	return -1;
}

static int ulp_matcher_class_hdr_field_validate(struct ulp_tc_parser_params
						*params, u32 idx)
{
	struct bnxt_ulp_class_match_info *info = &ulp_class_match_list[idx];
	u64 bitmap;

	/* manadatory fields should be enabled */
	if ((params->fld_s_bitmap.bits & info->field_man_bitmap) !=
	    info->field_man_bitmap){
		netdev_dbg(params->ulp_ctx->bp->dev,
			   "mismatch in manadatory hdr fields\n");
		return -EINVAL;
	}

	/* optional fields may be enaled or not */
	bitmap = params->fld_s_bitmap.bits & (~info->field_man_bitmap);
	if ((bitmap && (bitmap & info->field_opt_bitmap) != bitmap)) {
		netdev_dbg(params->ulp_ctx->bp->dev,
			   "mismatch in optional hdr fields\n");
		return -EINVAL;
	}

	return 0;
}

static u64 ulp_matcher_class_hdr_field_signature(struct ulp_tc_parser_params
						 *params, u32 idx)
{
	struct bnxt_ulp_class_match_info *info = &ulp_class_match_list[idx];

	/* remove the exclude bits */
	return (params->fld_s_bitmap.bits & ~info->field_exclude_bitmap);
}

static struct ulp_matcher_class_db_node *
ulp_matcher_class_hash_lookup(struct bnxt_ulp_matcher_data *mdata,
			      struct ulp_tc_parser_params *params)
{
	struct ulp_matcher_class_db_node *matcher_node;
	struct ulp_matcher_hash_db_key key = {{ 0 }};

	key.app_id = params->app_id;
	key.hdr_bitmap = params->hdr_bitmap;

	matcher_node = rhashtable_lookup_fast(&mdata->matcher_db, &key,
					      mdata->matcher_db_ht_params);
	if (!matcher_node)
		return NULL;

	if (!matcher_node->in_use) {
		netdev_dbg(params->ulp_ctx->bp->dev,
			   "Matcher database is corrupt\n");
		return NULL;
	}
	return matcher_node;
}

static struct ulp_matcher_class_db_node *
ulp_matcher_class_hash_add(struct bnxt_ulp_matcher_data *matcher_data,
			   struct ulp_tc_parser_params *params,
			   int class_match_idx)
{
	struct ulp_matcher_class_db_node *matcher_node;
	struct ulp_matcher_hash_db_key key = {{ 0 }};
	int rc;

	key.app_id = params->app_id;
	key.hdr_bitmap = params->hdr_bitmap;

	matcher_node = kzalloc(sizeof(*matcher_node), GFP_KERNEL);
	if (!matcher_node)
		return NULL;

	matcher_node->key = key;
	matcher_node->in_use = 1;
	matcher_node->match_info_idx = class_match_idx;
	rc = rhashtable_insert_fast(&matcher_data->matcher_db, &matcher_node->node,
				    matcher_data->matcher_db_ht_params);
	if (rc) {
		netdev_dbg(params->ulp_ctx->bp->dev,
			   "unable add the entry to matcher hash: %d\n",
			   class_match_idx);
		kfree_rcu(matcher_node, rcu);
		return NULL;
	}

	netdev_dbg(params->ulp_ctx->bp->dev,
		   "Added entry: %d to matcher hash\n",
		   class_match_idx);
	return matcher_node;
}

/* Utility function to calculate the action matcher hash */
static u32
ulp_matcher_action_hash_calculate(u64 hi_sig, u64 app_id)
{
	u64 hash;

	hi_sig |= ((hi_sig % BNXT_ULP_ACT_HID_HIGH_PRIME) <<
		   BNXT_ULP_ACT_HID_SHFTL);
	app_id |= ((app_id % BNXT_ULP_ACT_HID_LOW_PRIME) <<
		   (BNXT_ULP_ACT_HID_SHFTL + 2));
	hash = hi_sig ^ app_id;
	hash = (hash >> BNXT_ULP_ACT_HID_SHFTR) & BNXT_ULP_ACT_HID_MASK;
	return (u32)hash;
}

/* Function to handle the matching of RTE Flows and validating
 * the pattern masks against the flow templates.
 */
int
ulp_matcher_pattern_match(struct ulp_tc_parser_params *params,
			  u32 *class_id)
{
	struct ulp_matcher_class_db_node *matcher_node;
	struct bnxt_ulp_class_match_info *class_match;
	struct bnxt_ulp_matcher_data *matcher_data;
	u32 class_match_idx = 0;

	/* Get the matcher data for hash lookup  */
	matcher_data = (struct bnxt_ulp_matcher_data *)
		bnxt_ulp_cntxt_ptr2_matcher_data_get(params->ulp_ctx);
	if (!matcher_data) {
		netdev_dbg(params->ulp_ctx->bp->dev,
			   "Failed to get the ulp matcher data\n");
		return -EINVAL;
	}

	/* search the matcher hash db for the entry  */
	matcher_node = ulp_matcher_class_hash_lookup(matcher_data, params);
	if (!matcher_node) {
		/* find  the class list entry */
		if (ulp_matcher_class_list_lookup(params, &class_match_idx))
			goto error;

		/* add it to the hash */
		matcher_node = ulp_matcher_class_hash_add(matcher_data, params,
							  class_match_idx);
		if (!matcher_node)
			goto error;
	} else {
		class_match_idx = matcher_node->match_info_idx;
	}

	class_match = &ulp_class_match_list[matcher_node->match_info_idx];

	/* perform the field bitmap validation */
	if (ulp_matcher_class_hdr_field_validate(params,
						 matcher_node->match_info_idx))
		goto error;

	/* Update the fields for further processing */
	*class_id = class_match->class_tid;
	params->class_info_idx = matcher_node->match_info_idx;
	params->flow_sig_id =
		ulp_matcher_class_hdr_field_signature(params, class_match_idx);
	params->flow_pattern_id = class_match->flow_pattern_id;

	netdev_dbg(params->ulp_ctx->bp->dev,
		   "Found matching pattern template %u:%d\n",
		   class_match_idx, class_match->class_tid);
	return BNXT_TF_RC_SUCCESS;

error:
	netdev_err(params->ulp_ctx->bp->dev, "Did not find any matching template\n");
	netdev_err(params->ulp_ctx->bp->dev,
		   "hid:0x%x, Hdr:0x%llx Fld:0x%llx SFld:0x%llx\n",
		   class_match_idx, params->hdr_bitmap.bits,
		   params->fld_bitmap.bits, params->fld_s_bitmap.bits);
	*class_id = 0;
	return BNXT_TF_RC_ERROR;
}

/* Function to handle the matching of RTE Flows and validating
 * the action against the flow templates.
 */
int
ulp_matcher_action_match(struct ulp_tc_parser_params *params,
			 u32 *act_id)
{
	struct bnxt_ulp_act_match_info *act_match;
	u32 act_hid;
	u16 tmpl_id;

	/* calculate the hash of the given flow action */
	act_hid = ulp_matcher_action_hash_calculate(params->act_bitmap.bits,
						    params->app_id);

	/* validate the calculate hash values */
	if (act_hid >= BNXT_ULP_ACT_SIG_TBL_MAX_SZ)
		goto error;
	tmpl_id = ulp_act_sig_tbl[act_hid];
	if (!tmpl_id)
		goto error;

	act_match = &ulp_act_match_list[tmpl_id];
	if (ULP_BITMAP_CMP(&params->act_bitmap, &act_match->act_sig)) {
		netdev_dbg(params->ulp_ctx->bp->dev, "Action Header does not match\n");
		goto error;
	}

	/* Match the application id before proceeding */
	if (params->app_id != act_match->app_sig) {
		netdev_dbg(params->ulp_ctx->bp->dev, "Field to match the app id %u:%u\n",
			   params->app_id, act_match->app_sig);
		goto error;
	}

	*act_id = act_match->act_tid;
	params->act_pattern_id = act_match->act_pattern_id;
	netdev_dbg(params->ulp_ctx->bp->dev, "Found matching action template %u\n", *act_id);
	return BNXT_TF_RC_SUCCESS;

error:
	netdev_err(params->ulp_ctx->bp->dev, "Did not find any matching action template\n");
	netdev_err(params->ulp_ctx->bp->dev, "act_hid:0x%x, Hdr:%llx\n",
		   act_hid, params->act_bitmap.bits);
	*act_id = 0;
	return BNXT_TF_RC_ERROR;
}

static const struct rhashtable_params ulp_matcher_class_ht_params = {
	.head_offset = offsetof(struct ulp_matcher_class_db_node, node),
	.key_offset = offsetof(struct ulp_matcher_class_db_node, key),
	.key_len = sizeof(struct ulp_matcher_hash_db_key),
	.automatic_shrinking = true
};

int ulp_matcher_init(struct bnxt_ulp_context *ulp_ctx)
{
	struct bnxt_ulp_matcher_data *data;
	int rc;

	data = vzalloc(sizeof(*data));
	if (!data)
		return -ENOMEM;

	if (bnxt_ulp_cntxt_ptr2_matcher_data_set(ulp_ctx, data)) {
		netdev_dbg(ulp_ctx->bp->dev,
			   "Failed to set matcher data in context\n");
		goto free_matcher_data;
	}

	data->matcher_db_ht_params = ulp_matcher_class_ht_params;
	rc = rhashtable_init(&data->matcher_db,
			     &data->matcher_db_ht_params);
	if (rc) {
		netdev_dbg(ulp_ctx->bp->dev,
			   "Failed to create matcher hash table\n");
		goto clear_matcher_data;
	}

	return 0;

clear_matcher_data:
	bnxt_ulp_cntxt_ptr2_matcher_data_set(ulp_ctx, NULL);
free_matcher_data:
	vfree(data);
	return -ENOMEM;
}

static void ulp_matcher_class_hash_deinit(struct bnxt_ulp_context *ulp_ctx)
{
	struct bnxt_ulp_class_match_info *class_list = ulp_class_match_list;
	struct ulp_matcher_class_db_node *matcher_node = NULL;
	struct ulp_matcher_hash_db_key key = {{ 0 }};
	struct bnxt_ulp_matcher_data *mdata;
	u32 idx = 0;
	int rc;

	/* Get the matcher data for hash lookup  */
	mdata = (struct bnxt_ulp_matcher_data *)
		bnxt_ulp_cntxt_ptr2_matcher_data_get(ulp_ctx);
	if (!mdata) {
		netdev_dbg(ulp_ctx->bp->dev,
			   "Failed to get the ulp matcher data\n");
		return;
	}

	while (++idx < BNXT_ULP_CLASS_MATCH_LIST_MAX_SZ) {
		/* iterate the list of class matches to find header match */
		key.app_id = class_list[idx].app_id;
		key.hdr_bitmap.bits = class_list[idx].hdr_bitmap.bits;

		matcher_node = rhashtable_lookup_fast(&mdata->matcher_db, &key,
						      mdata->matcher_db_ht_params);
		if (!matcher_node)
			continue;
		rc = rhashtable_remove_fast(&mdata->matcher_db,
					    &matcher_node->node,
					    mdata->matcher_db_ht_params);
		if (rc) {
			netdev_dbg(ulp_ctx->bp->dev,
				   "Failed to remove: %d from matcher hash\n",
				   idx);
			continue;
		}
		netdev_dbg(ulp_ctx->bp->dev,
			   "Removed entry: %d from matcher hash\n",
			   idx);
		kfree(matcher_node);
	}
}

void ulp_matcher_deinit(struct bnxt_ulp_context *ulp_ctx)
{
	struct bnxt_ulp_matcher_data *data;

	if (!ulp_ctx)
		return;

	data = (struct bnxt_ulp_matcher_data *)
		bnxt_ulp_cntxt_ptr2_matcher_data_get(ulp_ctx);
	if (!data)
		return;

	ulp_matcher_class_hash_deinit(ulp_ctx);
	rhashtable_destroy(&data->matcher_db);
	bnxt_ulp_cntxt_ptr2_matcher_data_set(ulp_ctx, NULL);
	vfree(data);
}

#endif /* CONFIG_BNXT_FLOWER_OFFLOAD */
