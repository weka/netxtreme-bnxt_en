// SPDX-License-Identifier: BSD-3-Clause
/* Copyright(c) 2019-2021 Broadcom
 * All rights reserved.
 */
#include "ulp_linux.h"
#include "bnxt_compat.h"
#include "bnxt_hsi.h"
#include "tf_core.h"
#include "ulp_mapper.h"
#include "ulp_flow_db.h"
#include "ulp_template_debug_proto.h"
#include "ulp_tf_debug.h"

#if defined(CONFIG_BNXT_FLOWER_OFFLOAD) || defined(CONFIG_BNXT_CUSTOM_FLOWER_OFFLOAD)
/* Retrieve the generic table  initialization parameters for the tbl_idx */
static struct bnxt_ulp_generic_tbl_params*
ulp_mapper_gen_tbl_params_get(u32 tbl_idx)
{
	if (tbl_idx >= BNXT_ULP_GEN_TBL_MAX_SZ)
		return NULL;

	return &ulp_generic_tbl_params[tbl_idx];
}

/**
 * Initialize the generic table list
 *
 * @mapper_data: Pointer to the mapper data and the generic table is
 * part of it
 *
 * returns 0 on success
 */
int
ulp_mapper_generic_tbl_list_init(struct bnxt_ulp_context *ulp_ctx,
				 struct bnxt_ulp_mapper_data *mapper_data)
{
	struct bnxt_ulp_generic_tbl_params *tbl;
	struct ulp_mapper_gen_tbl_list *entry;
	u32 idx, size;
	struct rhashtable_params bnxt_tf_tc_ht_params = { 0 };
	int rc = 0;

	/* Allocate the generic tables. */
	for (idx = 0; idx < BNXT_ULP_GEN_TBL_MAX_SZ; idx++) {
		tbl = ulp_mapper_gen_tbl_params_get(idx);
		if (!tbl) {
			netdev_dbg(ulp_ctx->bp->dev, "Failed to get gen table parms %d\n",
				   idx);
			return -EINVAL;
		}
		entry = &mapper_data->gen_tbl_list[idx];
		if (tbl->result_num_entries != 0) {
			/* assign the name */
			entry->gen_tbl_name = tbl->name;
			/* add 4 bytes for reference count */
			entry->mem_data_size = (tbl->result_num_entries + 1) *
				(tbl->result_num_bytes + sizeof(u32));

			/* allocate the big chunk of memory */
			entry->mem_data = vzalloc(entry->mem_data_size);
			if (!entry->mem_data)
				return -ENOMEM;

			/* Populate the generic table container */
			entry->container.num_elem = tbl->result_num_entries;
			entry->container.byte_data_size = tbl->result_num_bytes;
			entry->container.ref_count =
				(u32 *)entry->mem_data;
			size = sizeof(u32) * (tbl->result_num_entries + 1);
			entry->container.byte_data = &entry->mem_data[size];
			entry->container.byte_order = tbl->result_byte_order;
		} else {
			netdev_dbg(ulp_ctx->bp->dev, "%s: Unused Gen tbl entry is %d\n",
				   tbl->name, idx);
		}
		if (tbl->hash_tbl_entries) {
			bnxt_tf_tc_ht_params.head_offset =
				offsetof(struct ulp_gen_hash_entry_params, node);
			bnxt_tf_tc_ht_params.key_offset =
				offsetof(struct ulp_gen_hash_entry_params, key_data);
			bnxt_tf_tc_ht_params.key_len = tbl->key_num_bytes;
			bnxt_tf_tc_ht_params.automatic_shrinking = true;
			bnxt_tf_tc_ht_params.nelem_hint = /* Set to about 75% */
				(tbl->result_num_entries * 75) / 100;
			bnxt_tf_tc_ht_params.max_size =	tbl->result_num_entries;
			entry->hash_tbl_params = bnxt_tf_tc_ht_params;
			entry->hash_tbl = vzalloc(sizeof(*entry->hash_tbl));
			rc = rhashtable_init(entry->hash_tbl, &entry->hash_tbl_params);
			if (rc) {
				netdev_dbg(ulp_ctx->bp->dev, "HASH_TABLE initialization failed\n");
				return rc;
			}
		}
	}
	/* success */
	return 0;
}

/**
 * Free the generic table list
 *
 * @mapper_data: Pointer to the mapper data and the generic table is
 * part of it
 *
 * returns 0 on success
 */
int
ulp_mapper_generic_tbl_list_deinit(struct bnxt_ulp_mapper_data *mapper_data)
{
	struct ulp_mapper_gen_tbl_list *tbl_list;
	u32 idx;

	/* iterate the generic table. */
	for (idx = 0; idx < BNXT_ULP_GEN_TBL_MAX_SZ; idx++) {
		tbl_list = &mapper_data->gen_tbl_list[idx];
		vfree(tbl_list->mem_data);
		tbl_list->mem_data = NULL;
		if (tbl_list->hash_tbl) {
			rhashtable_destroy(tbl_list->hash_tbl);
			vfree(tbl_list->hash_tbl);
		}
	}
	/* success */
	return 0;
}

/**
 * Get the generic table list entry
 *
 * @tbl_list: - Ptr to generic table
 * @key: - Key index to the table
 * @entry: - output will include the entry if found
 *
 * returns 0 on success.
 */
int
ulp_mapper_gen_tbl_entry_get(struct bnxt_ulp_context *ulp_ctx,
			     struct ulp_mapper_gen_tbl_list *tbl_list,
			     u32 key,
			     struct ulp_mapper_gen_tbl_entry *entry)
{
	/* populate the output and return the values */
	if (key > tbl_list->container.num_elem) {
		netdev_dbg(ulp_ctx->bp->dev, "%s: invalid key %x:%x\n",
			   tbl_list->gen_tbl_name, key,
			   tbl_list->container.num_elem);
		return -EINVAL;
	}
	entry->ref_count = &tbl_list->container.ref_count[key];
	entry->byte_data_size = tbl_list->container.byte_data_size;
	entry->byte_data = &tbl_list->container.byte_data[key *
		entry->byte_data_size];
	entry->byte_order = tbl_list->container.byte_order;
	return 0;
}

/**
 * utility function to calculate the table idx
 *
 * @res_sub_type: - Resource sub type
 * @dir: - Direction
 *
 * returns None
 */
int
ulp_mapper_gen_tbl_idx_calculate(struct bnxt_ulp_context *ulp_ctx,
				 u32 res_sub_type, u32 dir)
{
	int tbl_idx;

	/* Validate for direction */
	if (dir >= TF_DIR_MAX) {
		netdev_dbg(ulp_ctx->bp->dev, "invalid argument %x\n", dir);
		return -EINVAL;
	}
	tbl_idx = (res_sub_type << 1) | (dir & 0x1);
	if (tbl_idx >= BNXT_ULP_GEN_TBL_MAX_SZ) {
		netdev_dbg(ulp_ctx->bp->dev, "invalid table index %x\n", tbl_idx);
		return -EINVAL;
	}
	return tbl_idx;
}

/**
 * Set the data in the generic table entry, Data is in Big endian format
 *
 * @entry: - generic table entry
 * @len: - The length of the data in bits to be set
 * @data: - pointer to the data to be used for setting the value.
 * @data_size: - length of the data pointer in bytes.
 *
 * returns 0 on success
 */
int
ulp_mapper_gen_tbl_entry_data_set(struct bnxt_ulp_context *ulp_ctx,
				  struct ulp_mapper_gen_tbl_entry *entry,
				  u32 len, u8 *data,
				  u32 data_size)
{
	/* validate the null arguments */
	if (!entry || !data) {
		netdev_dbg(ulp_ctx->bp->dev, "invalid argument\n");
		return -EINVAL;
	}

	/* check the size of the buffer for validation */
	if (len > ULP_BYTE_2_BITS(entry->byte_data_size) ||
	    data_size < ULP_BITS_2_BYTE(len)) {
		netdev_dbg(ulp_ctx->bp->dev, "invalid offset or length %x:%x\n",
			   len, entry->byte_data_size);
		return -EINVAL;
	}
	memcpy(entry->byte_data, data, ULP_BITS_2_BYTE(len));
	return 0;
}

/**
 * Get the data in the generic table entry, Data is in Big endian format
 *
 * @entry: - generic table entry
 * @offset: - The offset in bits where the data has to get
 * @len: - The length of the data in bits to be get
 * @data: - pointer to the data to be used for setting the value.
 * @data_size: - The size of data in bytes
 *
 * returns 0 on success
 */
int
ulp_mapper_gen_tbl_entry_data_get(struct bnxt_ulp_context *ulp_ctx,
				  struct ulp_mapper_gen_tbl_entry *entry,
				  u32 offset, u32 len, u8 *data,
				  u32 data_size)
{
	/* validate the null arguments */
	if (!entry || !data) {
		netdev_dbg(ulp_ctx->bp->dev, "invalid argument\n");
		return -EINVAL;
	}

	/* check the size of the buffer for validation */
	if ((offset + len) > ULP_BYTE_2_BITS(entry->byte_data_size) ||
	    len > ULP_BYTE_2_BITS(data_size)) {
		netdev_dbg(ulp_ctx->bp->dev, "invalid offset or length %x:%x:%x\n",
			   offset, len, entry->byte_data_size);
		return -EINVAL;
	}
	if (entry->byte_order == BNXT_ULP_BYTE_ORDER_LE)
		ulp_bs_pull_lsb(entry->byte_data, data, data_size, offset, len);
	else
		ulp_bs_pull_msb(entry->byte_data, data, offset, len);

	return 0;
}

/**
 * Free the generic table list resource
 *
 * @ulp_ctx: - Pointer to the ulp context
 * @res: - Pointer to flow db resource entry
 *
 * returns 0 on success
 */
int
ulp_mapper_gen_tbl_res_free(struct bnxt_ulp_context *ulp_ctx,
			    struct ulp_flow_db_res_params *res)
{
	struct ulp_mapper_gen_tbl_entry entry, *actual_entry;
	struct ulp_gen_hash_entry_params *hash_entry = NULL;
	struct ulp_mapper_gen_tbl_list *gen_tbl_list;
	struct bnxt_ulp_mapper_data *mapper_data;
	int tbl_idx;
	u32 fid = 0;
	u32 key_idx;
	int rc;

	/* Extract the resource sub type and direction */
	tbl_idx = ulp_mapper_gen_tbl_idx_calculate(ulp_ctx, res->resource_sub_type,
						   res->direction);
	if (tbl_idx < 0) {
		netdev_dbg(ulp_ctx->bp->dev, "invalid argument %x:%x\n",
			   res->resource_sub_type, res->direction);
		return -EINVAL;
	}

	mapper_data = bnxt_ulp_cntxt_ptr2_mapper_data_get(ulp_ctx);
	if (!mapper_data) {
		netdev_dbg(ulp_ctx->bp->dev, "invalid ulp context %x\n", tbl_idx);
		return -EINVAL;
	}
	/* get the generic table  */
	gen_tbl_list = &mapper_data->gen_tbl_list[tbl_idx];

	/* Get the generic table entry*/
	if (gen_tbl_list->hash_tbl) {
		/* use the hash index to get the value */
		hash_entry = rhashtable_lookup_fast(gen_tbl_list->hash_tbl,
						    res->key_data,
						    gen_tbl_list->hash_tbl_params);
		if (!hash_entry) {
			netdev_dbg(ulp_ctx->bp->dev, "invalid hash entry %p\n", hash_entry);
			return -EINVAL;
		}

		if (!hash_entry->entry.hash_ref_count) {
			netdev_dbg(ulp_ctx->bp->dev, "generic table corrupt %x: %llu\n",
				   tbl_idx, res->resource_hndl);
			return -EINVAL;
		}
		hash_entry->entry.hash_ref_count--;
		if (hash_entry->entry.hash_ref_count)
			return 0;

		actual_entry = &hash_entry->entry;
	} else {
		key_idx =  (u32)res->resource_hndl;
		if (ulp_mapper_gen_tbl_entry_get(ulp_ctx, gen_tbl_list,
						 key_idx, &entry)) {
			netdev_dbg(ulp_ctx->bp->dev,
				   "Gen tbl entry get failed %x: %llu\n",
				   tbl_idx, res->resource_hndl);
			return -EINVAL;
		}
		/* Decrement the reference count */
		if (!ULP_GEN_TBL_REF_CNT(&entry)) {
			netdev_dbg(ulp_ctx->bp->dev,
				   "generic table entry already free %x: %llu\n",
				   tbl_idx, res->resource_hndl);
			return 0;
		}
		ULP_GEN_TBL_REF_CNT_DEC(&entry);

		/* retain the details since there are other users */
		if (ULP_GEN_TBL_REF_CNT(&entry)) {
			netdev_dbg(ulp_ctx->bp->dev, "Key idx not freed, have references\n");
			return 0;
		}

		actual_entry = &entry;
	}

	/* Delete the generic table entry. First extract the fid */
	if (ulp_mapper_gen_tbl_entry_data_get(ulp_ctx, actual_entry, ULP_GEN_TBL_FID_OFFSET,
					      ULP_GEN_TBL_FID_SIZE_BITS,
					      (u8 *)&fid,
					      sizeof(fid))) {
		netdev_dbg(ulp_ctx->bp->dev, "Unable to get fid %x: %llu\n",
			   tbl_idx, res->resource_hndl);
		return -EINVAL;
	}

	fid = be32_to_cpu(fid);
	/* no need to del if fid is 0 since there is no associated resource */
	if (fid) {
		/* Destroy the flow associated with the shared flow id */
		if (ulp_mapper_flow_destroy(ulp_ctx, BNXT_ULP_FDB_TYPE_RID,
					    fid))
			netdev_dbg(ulp_ctx->bp->dev,
				   "Error in deleting shared flow id %x\n",
				   fid);
	}

	/* Delete the entry from the hash table */
	if (gen_tbl_list->hash_tbl) {
		rc = rhashtable_remove_fast(gen_tbl_list->hash_tbl, &hash_entry->node,
					    gen_tbl_list->hash_tbl_params);
		if (rc) {
			netdev_dbg(ulp_ctx->bp->dev,
				   "Unable to delete hash entry %p\n", hash_entry);
			return rc;
		}

		vfree(hash_entry->entry.byte_data);
		kfree_rcu(hash_entry, rcu);
	}

	return 0;
}
#endif /* CONFIG_BNXT_FLOWER_OFFLOAD */
