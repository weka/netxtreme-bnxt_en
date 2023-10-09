/* Broadcom NetXtreme-C/E network driver.
 *
 * Copyright (c) 2022 Broadcom Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation.
 */

#ifndef BNXT_KTLS_H
#define BNXT_KTLS_H

#include <linux/hashtable.h>

#define BNXT_MAX_TKC		16384
#define BNXT_MAX_RKC		16384

#define BNXT_TKC	FUNC_KEY_CTX_ALLOC_REQ_KEY_CTX_TYPE_TX
#define BNXT_RKC	FUNC_KEY_CTX_ALLOC_REQ_KEY_CTX_TYPE_RX

#define BNXT_KID_BATCH_SIZE	128

struct bnxt_kid_info {
	struct list_head	list;
	u32			start_id;
	u32			count;
	DECLARE_BITMAP(ids, BNXT_KID_BATCH_SIZE);
};

struct bnxt_kctx {
	struct list_head	list;
	/* to serialize update to the linked list and total_alloc */
	spinlock_t		lock;
	u8			type;
	u32			total_alloc;
	u32			max_ctx;
	atomic_t		alloc_pending;
#define BNXT_KCTX_ALLOC_PENDING_MAX	8
	wait_queue_head_t	alloc_pending_wq;
};

#define BNXT_KCTX_ALLOC_OK(kctx)	\
	(atomic_read(&((kctx)->alloc_pending)) < BNXT_KCTX_ALLOC_PENDING_MAX)

struct bnxt_kfltr_info {
	u32			kid;
	__le64			filter_id;
	struct hlist_node	hash;
	struct rcu_head		rcu;
};

#define BNXT_MAX_KCTX		(BNXT_RKC + 1)

struct bnxt_ktls_info {
	u16			max_key_ctxs_alloc;
	struct bnxt_kctx	kctx[BNXT_MAX_KCTX];

	struct kmem_cache	*mpc_cache;
	atomic_t		pending;

	DECLARE_HASHTABLE(filter_tbl, 8);
	/* to serialize adding to and deleting from the filter_tbl */
	spinlock_t		filter_lock;
	u32			filter_count;
#define BNXT_MAX_KTLS_FILTER	460

	atomic64_t		ktls_tx_add_count;
	atomic64_t		ktls_tx_del_count;
	atomic64_t		ktls_tx_hw_pkt_count;
	atomic64_t		ktls_tx_sw_pkt_count;
	atomic64_t		ktls_tx_tls_ooo;
	atomic64_t		ktls_rx_add_count;
	atomic64_t		ktls_rx_del_count;
	atomic64_t		ktls_rx_hw_pkt_count;
	atomic64_t		ktls_rx_sw_pkt_count;
	atomic64_t		ktls_rx_resync_req_count;
	atomic64_t		ktls_rx_resync_ack_count;
};

#define tkc	kctx[BNXT_TKC]
#define rkc	kctx[BNXT_RKC]

struct bnxt_ktls_offload_ctx_tx {
	u32		tcp_seq_no;
	u32		kid;
};

struct bnxt_ktls_offload_ctx_rx {
	u32		kid;
};

struct ce_add_cmd {
	__le32	ver_algo_kid_opcode;
	#define CE_ADD_CMD_OPCODE_MASK			0xfUL
	#define CE_ADD_CMD_OPCODE_SFT			0
	#define CE_ADD_CMD_OPCODE_ADD			 0x1UL
	#define CE_ADD_CMD_KID_MASK			0xfffff0UL
	#define CE_ADD_CMD_KID_SFT			4
	#define CE_ADD_CMD_ALGORITHM_MASK		0xf000000UL
	#define CE_ADD_CMD_ALGORITHM_SFT		24
	#define CE_ADD_CMD_ALGORITHM_AES_GCM_128	 0x1000000UL
	#define CE_ADD_CMD_ALGORITHM_AES_GCM_256	 0x2000000UL
	#define CE_ADD_CMD_VERSION_MASK			0xf0000000UL
	#define CE_ADD_CMD_VERSION_SFT			28
	#define CE_ADD_CMD_VERSION_TLS1_2		 (0x0UL << 28)
	#define CE_ADD_CMD_VERSION_TLS1_3		 (0x1UL << 28)
	u8	ctx_kind;
	#define CE_ADD_CMD_CTX_KIND_MASK		0x1fUL
	#define CE_ADD_CMD_CTX_KIND_SFT			0
	#define CE_ADD_CMD_CTX_KIND_CK_TX		 0x11UL
	#define CE_ADD_CMD_CTX_KIND_CK_RX		 0x12UL
	u8	unused0[3];
	u8	salt[4];
	u8	unused1[4];
	__le32	pkt_tcp_seq_num;
	__le32	tls_header_tcp_seq_num;
	u8	record_seq_num[8];
	u8	session_key[32];
	u8	addl_iv[8];
};

#define record_seq_num_end	record_seq_num[7]

struct ce_delete_cmd {
	__le32  ctx_kind_kid_opcode;
	#define CE_DELETE_CMD_OPCODE_MASK		0xfUL
	#define CE_DELETE_CMD_OPCODE_SFT		0
	#define CE_DELETE_CMD_OPCODE_DEL		 0x2UL
	#define CE_DELETE_CMD_KID_MASK			0xfffff0UL
	#define CE_DELETE_CMD_KID_SFT			4
	#define CE_DELETE_CMD_CTX_KIND_MASK		0x1f000000UL
	#define CE_DELETE_CMD_CTX_KIND_SFT		24
	#define CE_DELETE_CMD_CTX_KIND_CK_TX		 (0x11UL << 24)
	#define CE_DELETE_CMD_CTX_KIND_CK_RX		 (0x12UL << 24)
};

struct ce_resync_resp_ack_cmd {
	__le32	resync_status_kid_opcode;
	#define CE_RESYNC_RESP_ACK_CMD_OPCODE_MASK	0xfUL
	#define CE_RESYNC_RESP_ACK_CMD_OPCODE_SFT	0
	#define CE_RESYNC_RESP_ACK_CMD_OPCODE_RESYNC	 0x3UL
	#define CE_RESYNC_RESP_ACK_CMD_KID_MASK		0xfffff0UL
	#define CE_RESYNC_RESP_ACK_CMD_KID_SFT		4
	#define CE_RESYNC_RESP_ACK_CMD_RESYNC_STATUS	0x1000000UL
	#define CE_RESYNC_RESP_ACK_CMD_RESYNC_STATUS_ACK (0x0UL << 24)
	__le32	resync_record_tcp_seq_num;
	u8	resync_record_seq_num[8];
};

#define resync_record_seq_num_end	resync_record_seq_num[7]

#define CE_CMD_KID_MASK			0xfffff0UL
#define CE_CMD_KID_SFT			4

#define CE_CMD_KID(cmd_p)					\
	((*(u32 *)(cmd_p) & CE_CMD_KID_MASK) >> CE_CMD_KID_SFT)

#define BNXT_KMPC_OPAQUE(client, kid)				\
	(((client) << 24) | (kid))

#define BNXT_INV_KMPC_OPAQUE	0xffffffff

struct ce_cmpl {
	__le16	client_subtype_type;
	#define CE_CMPL_TYPE_MASK			0x3fUL
	#define CE_CMPL_TYPE_SFT			0
	#define CE_CMPL_TYPE_MID_PATH_SHORT		 0x1eUL
	#define CE_CMPL_SUBTYPE_MASK			0xf00UL
	#define CE_CMPL_SUBTYPE_SFT			8
	#define CE_CMPL_SUBTYPE_SOLICITED		 (0x0UL << 8)
	#define CE_CMPL_SUBTYPE_ERR			 (0x1UL << 8)
	#define CE_CMPL_SUBTYPE_RESYNC			 (0x2UL << 8)
	#define CE_CMPL_MP_CLIENT_MASK			0xf000UL
	#define CE_CMPL_MP_CLIENT_SFT			12
	#define CE_CMPL_MP_CLIENT_TCE			 (0x0UL << 12)
	#define CE_CMPL_MP_CLIENT_RCE			 (0x1UL << 12)
	__le16	status;
	#define CE_CMPL_STATUS_MASK			0xfUL
	#define CE_CMPL_STATUS_SFT			0
	#define CE_CMPL_STATUS_OK			 0x0UL
	#define CE_CMPL_STATUS_CTX_LD_ERR		 0x1UL
	#define CE_CMPL_STATUS_FID_CHK_ERR		 0x2UL
	#define CE_CMPL_STATUS_CTX_VER_ERR		 0x3UL
	#define CE_CMPL_STATUS_DST_ID_ERR		 0x4UL
	#define CE_CMPL_STATUS_MP_CMD_ERR		 0x5UL
	u32	opaque;
	__le32	v;
	#define CE_CMPL_V           0x1UL
	__le32	kid;
	#define CE_CMPL_KID_MASK    0xfffffUL
	#define CE_CMPL_KID_SFT     0
};

#define CE_CMPL_STATUS(ce_cmpl)						\
	(le16_to_cpu((ce_cmpl)->status) & CE_CMPL_STATUS_MASK)

#define CE_CMPL_KID(ce_cmpl)						\
	(le32_to_cpu((ce_cmpl)->kid) & CE_CMPL_KID_MASK)

struct bnxt_crypto_cmd_ctx {
	struct completion cmp;
	struct ce_cmpl ce_cmp;
};

static inline bool bnxt_ktls_busy(struct bnxt *bp)
{
	return bp->ktls_info && atomic_read(&bp->ktls_info->pending) > 0;
}

void bnxt_alloc_ktls_info(struct bnxt *bp, u16 max_key_ctxs_alloc);
void bnxt_clear_cfa_tls_filters_tbl(struct bnxt *bp);
void bnxt_free_ktls_info(struct bnxt *bp);
void bnxt_hwrm_reserve_pf_key_ctxs(struct bnxt *bp,
				   struct hwrm_func_cfg_input *req);
int bnxt_ktls_init(struct bnxt *bp);
void bnxt_ktls_mpc_cmp(struct bnxt *bp, u32 client, unsigned long handle,
		       struct bnxt_cmpl_entry cmpl[], u32 entries);
struct sk_buff *bnxt_ktls_xmit(struct bnxt *bp, struct sk_buff *skb,
			       __le32 *lflags, u32 *kid);
void bnxt_ktls_rx(struct bnxt *bp, struct sk_buff *skb, u8 *data_ptr,
		  unsigned int len, struct rx_cmp *rxcmp,
		  struct rx_cmp_ext *rxcmp1);
#endif
