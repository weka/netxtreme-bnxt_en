/* Broadcom NetXtreme-C/E network driver.
 *
 * Copyright (c) 2022 Broadcom Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation.
 */

#include <linux/stddef.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/hashtable.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/tcp.h>
#include <net/inet_hashtables.h>
#include <net/inet6_hashtables.h>
#ifdef HAVE_KTLS
#include <net/tls.h>
#endif

#include "bnxt_compat.h"
#include "bnxt_hsi.h"
#include "bnxt.h"
#include "bnxt_hwrm.h"
#include "bnxt_mpc.h"
#include "bnxt_ktls.h"

void bnxt_alloc_ktls_info(struct bnxt *bp, u16 max_key_ctxs_alloc)
{
	struct bnxt_ktls_info *ktls = bp->ktls_info;

	if (BNXT_VF(bp))
		return;
	if (!ktls) {
		struct bnxt_kctx *kctx;
		int i;

		ktls = kzalloc(sizeof(*ktls), GFP_KERNEL);
		if (!ktls)
			return;

		for (i = 0; i < BNXT_MAX_KCTX; i++) {
			kctx = &ktls->kctx[i];
			kctx->type = i;
			if (i == BNXT_TKC)
				kctx->max_ctx = BNXT_MAX_TKC;
			else
				kctx->max_ctx = BNXT_MAX_RKC;
			INIT_LIST_HEAD(&kctx->list);
			spin_lock_init(&kctx->lock);
			atomic_set(&kctx->alloc_pending, 0);
			init_waitqueue_head(&kctx->alloc_pending_wq);
		}

		hash_init(ktls->filter_tbl);
		spin_lock_init(&ktls->filter_lock);

		atomic_set(&ktls->pending, 0);

		bp->ktls_info = ktls;
	}
	ktls->max_key_ctxs_alloc = max_key_ctxs_alloc;
}

void bnxt_clear_cfa_tls_filters_tbl(struct bnxt *bp)
{
	struct bnxt_ktls_info *ktls = bp->ktls_info;
	struct bnxt_kfltr_info *kfltr;
	struct hlist_node *tmp_node;
	int bkt;

	if (!ktls)
		return;

	spin_lock(&ktls->filter_lock);
	hash_for_each_safe(ktls->filter_tbl, bkt, tmp_node, kfltr, hash) {
		hash_del_rcu(&kfltr->hash);
		kfree_rcu(kfltr, rcu);
	}
	ktls->filter_count = 0;
	spin_unlock(&ktls->filter_lock);
}

void bnxt_free_ktls_info(struct bnxt *bp)
{
	struct bnxt_ktls_info *ktls = bp->ktls_info;
	struct bnxt_kid_info *kid, *tmp;
	struct bnxt_kctx *kctx;
	int i;

	if (!ktls)
		return;

	/* Shutting down, no need to protect the lists. */
	for (i = 0; i < BNXT_MAX_KCTX; i++) {
		kctx = &ktls->kctx[i];
		list_for_each_entry_safe(kid, tmp, &kctx->list, list) {
			list_del(&kid->list);
			kfree(kid);
		}
	}
	bnxt_clear_cfa_tls_filters_tbl(bp);
	kmem_cache_destroy(ktls->mpc_cache);
	kfree(ktls);
	bp->ktls_info = NULL;
}

#if defined(HAVE_KTLS) && IS_ENABLED(CONFIG_TLS_DEVICE) && (LINUX_VERSION_CODE >= KERNEL_VERSION(5,0,0))
void bnxt_hwrm_reserve_pf_key_ctxs(struct bnxt *bp,
				   struct hwrm_func_cfg_input *req)
{
	struct bnxt_ktls_info *ktls = bp->ktls_info;
	struct bnxt_hw_resc *hw_resc = &bp->hw_resc;
	u32 tx, rx;

	if (!ktls)
		return;

	tx = min(ktls->tkc.max_ctx, hw_resc->max_tx_key_ctxs);
	req->num_tx_key_ctxs = cpu_to_le32(tx);
	rx = min(ktls->rkc.max_ctx, hw_resc->max_rx_key_ctxs);
	req->num_rx_key_ctxs = cpu_to_le32(rx);
	req->enables |= cpu_to_le32(FUNC_CFG_REQ_ENABLES_TX_KEY_CTXS |
				    FUNC_CFG_REQ_ENABLES_RX_KEY_CTXS);
}

static int bnxt_key_ctx_store(struct bnxt *bp, __le32 *key_buf, u32 num,
			      bool contig, struct bnxt_kctx *kctx, u32 *id)
{
	struct bnxt_kid_info *kid;
	u32 i;

	for (i = 0; i < num; ) {
		kid = kzalloc(sizeof(*kid), GFP_KERNEL);
		if (!kid)
			return -ENOMEM;
		kid->start_id = le32_to_cpu(key_buf[i]);
		if (contig)
			kid->count = num;
		else
			kid->count = 1;
		bitmap_set(kid->ids, 0, kid->count);
		if (id && !i) {
			clear_bit(0, kid->ids);
			*id = kid->start_id;
		}
		spin_lock(&kctx->lock);
		list_add_tail_rcu(&kid->list, &kctx->list);
		kctx->total_alloc += kid->count;
		spin_unlock(&kctx->lock);
		i += kid->count;
	}
	return 0;
}

static int bnxt_hwrm_key_ctx_alloc(struct bnxt *bp, struct bnxt_kctx *kctx,
				   u32 num, u32 *id)
{
	struct bnxt_ktls_info *ktls = bp->ktls_info;
	struct hwrm_func_key_ctx_alloc_output *resp;
	struct hwrm_func_key_ctx_alloc_input *req;
	dma_addr_t mapping;
	int pending_count;
	__le32 *key_buf;
	bool contig;
	int rc;

	num = min_t(u32, num, ktls->max_key_ctxs_alloc);
	rc = hwrm_req_init(bp, req, HWRM_FUNC_KEY_CTX_ALLOC);
	if (rc)
		return rc;

	key_buf = hwrm_req_dma_slice(bp, req, num * 4, &mapping);
	if (!key_buf) {
		rc = -ENOMEM;
		goto key_alloc_exit;
	}
	resp = hwrm_req_hold(bp, req);

	req->key_ctx_type = kctx->type;
	req->num_key_ctxs = cpu_to_le16(num);
	req->dma_bufr_size_bytes = cpu_to_le32(num * 4);
	req->host_dma_addr = cpu_to_le64(mapping);

	pending_count = atomic_inc_return(&kctx->alloc_pending);
	rc = hwrm_req_send(bp, req);
	atomic_dec(&kctx->alloc_pending);
	if (rc)
		goto key_alloc_exit_wake;

	num = le16_to_cpu(resp->num_key_ctxs_allocated);
	contig =
		resp->flags & FUNC_KEY_CTX_ALLOC_RESP_FLAGS_KEY_CTXS_CONTIGUOUS;
	rc = bnxt_key_ctx_store(bp, key_buf, num, contig, kctx, id);

key_alloc_exit_wake:
	if (pending_count >= BNXT_KCTX_ALLOC_PENDING_MAX)
		wake_up_all(&kctx->alloc_pending_wq);
key_alloc_exit:
	hwrm_req_drop(bp, req);
	return rc;
}

static int bnxt_alloc_one_kctx(struct bnxt_kctx *kctx, u32 *id)
{
	struct bnxt_kid_info *kid;
	int rc = -ENOMEM;

	rcu_read_lock();
	list_for_each_entry_rcu(kid, &kctx->list, list) {
		u32 idx = 0;

		do {
			idx = find_next_bit(kid->ids, kid->count, idx);
			if (idx >= kid->count)
				break;
			if (test_and_clear_bit(idx, kid->ids)) {
				*id = kid->start_id + idx;
				rc = 0;
				goto alloc_done;
			}
		} while (1);
	}

alloc_done:
	rcu_read_unlock();
	return rc;
}

static void bnxt_free_one_kctx(struct bnxt_kctx *kctx, u32 id)
{
	struct bnxt_kid_info *kid;

	rcu_read_lock();
	list_for_each_entry_rcu(kid, &kctx->list, list) {
		if (id >= kid->start_id && id < kid->start_id + kid->count) {
			set_bit(id - kid->start_id, kid->ids);
			break;
		}
	}
	rcu_read_unlock();
}

#define BNXT_KCTX_ALLOC_RETRY_MAX	3

static int bnxt_key_ctx_alloc_one(struct bnxt *bp, struct bnxt_kctx *kctx,
				  u32 *id)
{
	int rc, retry = 0;

	while (retry++ < BNXT_KCTX_ALLOC_RETRY_MAX) {
		rc = bnxt_alloc_one_kctx(kctx, id);
		if (!rc)
			return 0;

		if ((kctx->total_alloc + BNXT_KID_BATCH_SIZE) > kctx->max_ctx)
			return -ENOSPC;

		if (!BNXT_KCTX_ALLOC_OK(kctx)) {
			wait_event(kctx->alloc_pending_wq,
				   BNXT_KCTX_ALLOC_OK(kctx));
			continue;
		}
		rc = bnxt_hwrm_key_ctx_alloc(bp, kctx, BNXT_KID_BATCH_SIZE, id);
		if (!rc)
			return 0;
	}
	return -EAGAIN;
}

#define BNXT_TLS_FLTR_FLAGS					\
	(CFA_TLS_FILTER_ALLOC_REQ_ENABLES_L2_FILTER_ID |	\
	 CFA_TLS_FILTER_ALLOC_REQ_ENABLES_ETHERTYPE |		\
	 CFA_TLS_FILTER_ALLOC_REQ_ENABLES_IPADDR_TYPE |		\
	 CFA_TLS_FILTER_ALLOC_REQ_ENABLES_SRC_IPADDR |		\
	 CFA_TLS_FILTER_ALLOC_REQ_ENABLES_DST_IPADDR |		\
	 CFA_TLS_FILTER_ALLOC_REQ_ENABLES_IP_PROTOCOL |		\
	 CFA_TLS_FILTER_ALLOC_REQ_ENABLES_SRC_PORT |		\
	 CFA_TLS_FILTER_ALLOC_REQ_ENABLES_DST_PORT |		\
	 CFA_TLS_FILTER_ALLOC_REQ_ENABLES_KID |			\
	 CFA_TLS_FILTER_ALLOC_REQ_ENABLES_DST_ID)

static int bnxt_hwrm_cfa_tls_filter_alloc(struct bnxt *bp, struct sock *sk,
					  u32 kid)
{
	struct hwrm_cfa_tls_filter_alloc_output *resp;
	struct hwrm_cfa_tls_filter_alloc_input *req;
	struct bnxt_ktls_info *ktls = bp->ktls_info;
	struct inet_sock *inet = inet_sk(sk);
	struct bnxt_l2_filter *l2_fltr;
	struct bnxt_kfltr_info *kfltr;
	int rc;

	kfltr = kzalloc(sizeof(*kfltr), GFP_KERNEL);
	if (!kfltr)
		return -ENOMEM;

	rc = hwrm_req_init(bp, req, HWRM_CFA_TLS_FILTER_ALLOC);
	if (rc) {
		kfree(kfltr);
		return rc;
	}

	req->enables = cpu_to_le32(BNXT_TLS_FLTR_FLAGS);

	l2_fltr = bp->vnic_info[0].l2_filters[0];
	req->l2_filter_id = l2_fltr->base.filter_id;
	req->dst_id = cpu_to_le16(bp->vnic_info[0].fw_vnic_id);
	req->kid = cpu_to_le32(kid);

	req->ip_protocol = CFA_TLS_FILTER_ALLOC_REQ_IP_PROTOCOL_TCP;
	req->src_port = inet->inet_dport;
	req->dst_port = inet->inet_sport;

	switch (sk->sk_family) {
	case AF_INET:
	default:
		req->ethertype = htons(ETH_P_IP);
		req->ip_addr_type = CFA_TLS_FILTER_ALLOC_REQ_IP_ADDR_TYPE_IPV4;
		req->src_ipaddr[0] = inet->inet_daddr;
		req->dst_ipaddr[0] = inet->inet_saddr;
		break;
	case AF_INET6: {
		struct ipv6_pinfo *inet6 = inet6_sk(sk);

		req->ethertype = htons(ETH_P_IPV6);
		req->ip_addr_type = CFA_TLS_FILTER_ALLOC_REQ_IP_ADDR_TYPE_IPV6;
		memcpy(req->src_ipaddr, &sk->sk_v6_daddr, sizeof(req->src_ipaddr));
		memcpy(req->dst_ipaddr, &inet6->saddr, sizeof(req->dst_ipaddr));
		break;
	}
	}
	resp = hwrm_req_hold(bp, req);
	rc = hwrm_req_send(bp, req);
	if (rc) {
		kfree(kfltr);
	} else {
		kfltr->kid = kid;
		kfltr->filter_id = resp->tls_filter_id;
		spin_lock(&ktls->filter_lock);
		ktls->filter_count++;
		hash_add_rcu(ktls->filter_tbl, &kfltr->hash, kid);
		spin_unlock(&ktls->filter_lock);
	}
	hwrm_req_drop(bp, req);
	return rc;
}

static int bnxt_hwrm_cfa_tls_filter_free(struct bnxt *bp, u32 kid)
{
	struct bnxt_ktls_info *ktls = bp->ktls_info;
	struct hwrm_cfa_tls_filter_free_input *req;
	struct bnxt_kfltr_info *kfltr;
	bool found = false;
	int rc;

	rcu_read_lock();
	hash_for_each_possible_rcu(ktls->filter_tbl, kfltr, hash, kid) {
		if (kfltr->kid == kid) {
			found = true;
			break;
		}
	}
	rcu_read_unlock();
	if (!found)
		return -ENOENT;

	rc = hwrm_req_init(bp, req, HWRM_CFA_TLS_FILTER_FREE);
	if (rc)
		return rc;

	req->tls_filter_id = kfltr->filter_id;
	rc = hwrm_req_send(bp, req);

	spin_lock(&ktls->filter_lock);
	ktls->filter_count--;
	hash_del_rcu(&kfltr->hash);
	spin_unlock(&ktls->filter_lock);
	kfree_rcu(kfltr, rcu);
	return rc;
}

static int bnxt_xmit_crypto_cmd(struct bnxt *bp, struct bnxt_tx_ring_info *txr,
				void *cmd, uint len, uint tmo)
{
	struct bnxt_ktls_info *ktls = bp->ktls_info;
	struct bnxt_crypto_cmd_ctx *ctx = NULL;
	unsigned long tmo_left, handle = 0;
	int rc;

	if (tmo) {
		u32 kid = CE_CMD_KID(cmd);

		ctx = kmem_cache_alloc(ktls->mpc_cache, GFP_KERNEL);
		if (!ctx)
			return -ENOMEM;
		init_completion(&ctx->cmp);
		handle = (unsigned long)ctx;
		ctx->ce_cmp.opaque =
			BNXT_KMPC_OPAQUE(txr->tx_ring_struct.mpc_chnl_type,
					 kid);
		might_sleep();
	}
	spin_lock(&txr->tx_lock);
	rc = bnxt_start_xmit_mpc(bp, txr, cmd, len, handle);
	spin_unlock(&txr->tx_lock);
	if (rc || !tmo)
		goto xmit_done;

	tmo_left = wait_for_completion_timeout(&ctx->cmp, msecs_to_jiffies(tmo));
	if (!tmo_left) {
		ctx->ce_cmp.opaque = BNXT_INV_KMPC_OPAQUE;
		netdev_warn(bp->dev, "kTLS MP cmd %08x timed out\n",
			    *((u32 *)cmd));
		rc = -ETIMEDOUT;
		goto xmit_done;
	}
	if (CE_CMPL_STATUS(&ctx->ce_cmp) == CE_CMPL_STATUS_OK)
		rc = 0;
	else
		rc = -EIO;
xmit_done:
	if (ctx)
		kmem_cache_free(ktls->mpc_cache, ctx);
	return rc;
}

static void bnxt_copy_tls_mp_data(u8 *dst, u8 *src, int bytes)
{
	int i;

	for (i = 0; i < bytes; i++)
		dst[-i] = src[i];
}

static int bnxt_crypto_add(struct bnxt *bp,
			   enum tls_offload_ctx_dir direction,
			   struct tls_crypto_info *crypto_info, u32 tcp_seq_no,
			   u32 kid)
{
	struct bnxt_mpc_info *mpc = bp->mpc_info;
	struct bnxt_tx_ring_info *txr;
	struct ce_add_cmd cmd = {0};
	u32 data;

	if (direction == TLS_OFFLOAD_CTX_DIR_TX) {
		txr = &mpc->mpc_rings[BNXT_MPC_TCE_TYPE][0];
		cmd.ctx_kind = CE_ADD_CMD_CTX_KIND_CK_TX;
	} else {
		txr = &mpc->mpc_rings[BNXT_MPC_RCE_TYPE][0];
		cmd.ctx_kind = CE_ADD_CMD_CTX_KIND_CK_RX;
	}

	data = CE_ADD_CMD_OPCODE_ADD | (kid << CE_ADD_CMD_KID_SFT);
	switch (crypto_info->cipher_type) {
	case TLS_CIPHER_AES_GCM_128: {
		struct tls12_crypto_info_aes_gcm_128 *aes;

		aes = (void *)crypto_info;
		data |= CE_ADD_CMD_ALGORITHM_AES_GCM_128;
		if (crypto_info->version == TLS_1_3_VERSION)
			data |= CE_ADD_CMD_VERSION_TLS1_3;
		memcpy(&cmd.session_key, aes->key, sizeof(aes->key));
		memcpy(&cmd.salt, aes->salt, sizeof(aes->salt));
		memcpy(&cmd.addl_iv, aes->iv, sizeof(aes->iv));
		bnxt_copy_tls_mp_data(&cmd.record_seq_num_end, aes->rec_seq,
				      sizeof(aes->rec_seq));
		break;
	}
	case TLS_CIPHER_AES_GCM_256: {
		struct tls12_crypto_info_aes_gcm_256 *aes;

		aes = (void *)crypto_info;
		data |= CE_ADD_CMD_ALGORITHM_AES_GCM_256;
		if (crypto_info->version == TLS_1_3_VERSION)
			data |= CE_ADD_CMD_VERSION_TLS1_3;
		memcpy(&cmd.session_key, aes->key, sizeof(aes->key));
		memcpy(&cmd.salt, aes->salt, sizeof(aes->salt));
		memcpy(&cmd.addl_iv, aes->iv, sizeof(aes->iv));
		bnxt_copy_tls_mp_data(&cmd.record_seq_num_end, aes->rec_seq,
				      sizeof(aes->rec_seq));
		break;
	}
	}
	cmd.ver_algo_kid_opcode = cpu_to_le32(data);
	cmd.pkt_tcp_seq_num = cpu_to_le32(tcp_seq_no);
	cmd.tls_header_tcp_seq_num = cmd.pkt_tcp_seq_num;
	return bnxt_xmit_crypto_cmd(bp, txr, &cmd, sizeof(cmd),
				    BNXT_MPC_TMO_MSECS);
}

static int bnxt_crypto_del(struct bnxt *bp,
			   enum tls_offload_ctx_dir direction, u32 kid)
{
	struct bnxt_mpc_info *mpc = bp->mpc_info;
	struct bnxt_tx_ring_info *txr;
	struct ce_delete_cmd cmd = {0};
	u32 data;

	if (direction == TLS_OFFLOAD_CTX_DIR_TX) {
		txr = &mpc->mpc_rings[BNXT_MPC_TCE_TYPE][0];
		data = CE_DELETE_CMD_CTX_KIND_CK_TX;
	} else {
		txr = &mpc->mpc_rings[BNXT_MPC_RCE_TYPE][0];
		data = CE_DELETE_CMD_CTX_KIND_CK_RX;
	}

	data |= CE_DELETE_CMD_OPCODE_DEL | (kid << CE_DELETE_CMD_KID_SFT);

	cmd.ctx_kind_kid_opcode = cpu_to_le32(data);
	return bnxt_xmit_crypto_cmd(bp, txr, &cmd, sizeof(cmd),
				    BNXT_MPC_TMO_MSECS);
}

static bool bnxt_ktls_cipher_supported(struct bnxt *bp,
				       struct tls_crypto_info *crypto_info)
{
	u16 type = crypto_info->cipher_type;
	u16 version = crypto_info->version;

	if ((type == TLS_CIPHER_AES_GCM_128 ||
	     type == TLS_CIPHER_AES_GCM_256) &&
	    (version == TLS_1_2_VERSION ||
	     version == TLS_1_3_VERSION))
		return true;
	return false;
}

static int bnxt_ktls_dev_add(struct net_device *dev, struct sock *sk,
			     enum tls_offload_ctx_dir direction,
			     struct tls_crypto_info *crypto_info,
			     u32 start_offload_tcp_sn)
{
	struct bnxt_ktls_offload_ctx_tx *kctx_tx;
	struct bnxt_ktls_offload_ctx_rx *kctx_rx;
	struct bnxt *bp = netdev_priv(dev);
	struct bnxt_ktls_info *ktls;
	struct bnxt_kctx *kctx;
	u32 kid;
	int rc;

	BUILD_BUG_ON(sizeof(struct bnxt_ktls_offload_ctx_tx) >
		     TLS_OFFLOAD_CONTEXT_SIZE_TX);
	BUILD_BUG_ON(sizeof(struct bnxt_ktls_offload_ctx_rx) >
		     TLS_OFFLOAD_CONTEXT_SIZE_RX);

	if (!bnxt_ktls_cipher_supported(bp, crypto_info))
		return -EOPNOTSUPP;

	ktls = bp->ktls_info;
	atomic_inc(&ktls->pending);
	/* Make sure bnxt_close_nic() sees pending before we check the
	 * BNXT_STATE_OPEN flag.
	 */
	smp_mb__after_atomic();
	if (!test_bit(BNXT_STATE_OPEN, &bp->state)) {
		rc = -ENODEV;
		goto exit;
	}

	if (direction == TLS_OFFLOAD_CTX_DIR_TX) {
		kctx_tx = tls_driver_ctx(sk, TLS_OFFLOAD_CTX_DIR_TX);
		kctx = &ktls->tkc;
	} else {
		if (ktls->filter_count > BNXT_MAX_KTLS_FILTER)
			return -ENOSPC;
		kctx_rx = tls_driver_ctx(sk, TLS_OFFLOAD_CTX_DIR_RX);
		kctx = &ktls->rkc;
	}
	rc = bnxt_key_ctx_alloc_one(bp, kctx, &kid);
	if (rc)
		goto exit;
	rc = bnxt_crypto_add(bp, direction, crypto_info, start_offload_tcp_sn,
			     kid);
	if (rc)
		goto free_kctx;
	if (direction == TLS_OFFLOAD_CTX_DIR_TX) {
		kctx_tx->kid = kid;
		kctx_tx->tcp_seq_no = start_offload_tcp_sn;
		atomic64_inc(&ktls->ktls_tx_add_count);
	} else {
		kctx_rx->kid = kid;
		rc = bnxt_hwrm_cfa_tls_filter_alloc(bp, sk, kid);
		if (rc) {
			int err = bnxt_crypto_del(bp, direction, kid);

			/* If unable to free, keep the KID */
			if (err)
				goto exit;
			goto free_kctx;
		}
		atomic64_inc(&ktls->ktls_rx_add_count);
	}
free_kctx:
	if (rc)
		bnxt_free_one_kctx(kctx, kid);
exit:
	atomic_dec(&ktls->pending);
	return rc;
}

#if defined(BNXT_FPGA)
#define BNXT_RETRY_MAX	200
#else
#define BNXT_RETRY_MAX	20
#endif

static void bnxt_ktls_dev_del(struct net_device *dev,
			      struct tls_context *tls_ctx,
			      enum tls_offload_ctx_dir direction)
{
	struct bnxt_ktls_offload_ctx_tx *kctx_tx;
	struct bnxt_ktls_offload_ctx_rx *kctx_rx;
	struct bnxt *bp = netdev_priv(dev);
	struct bnxt_ktls_info *ktls;
	struct bnxt_kctx *kctx;
	int retry_cnt = 0;
	u32 kid;
	int rc;

	ktls = bp->ktls_info;
retry:
	atomic_inc(&ktls->pending);
	/* Make sure bnxt_close_nic() sees pending before we check the
	 * BNXT_STATE_OPEN flag.
	 */
	smp_mb__after_atomic();
	while (!test_bit(BNXT_STATE_OPEN, &bp->state)) {
		atomic_dec(&ktls->pending);
		if (!netif_running(dev))
			return;
		if (retry_cnt > BNXT_RETRY_MAX) {
			netdev_warn(bp->dev, "%s retry max %d exceeded, state %lx\n",
				    __func__, retry_cnt, bp->state);
			return;
		}
		retry_cnt++;
		msleep(100);
		goto retry;
	}

	if (direction == TLS_OFFLOAD_CTX_DIR_TX) {
		kctx_tx = __tls_driver_ctx(tls_ctx, TLS_OFFLOAD_CTX_DIR_TX);
		kid = kctx_tx->kid;
		kctx = &ktls->tkc;
	} else {
		kctx_rx = __tls_driver_ctx(tls_ctx, TLS_OFFLOAD_CTX_DIR_RX);
		kid = kctx_rx->kid;
		kctx = &ktls->rkc;
		bnxt_hwrm_cfa_tls_filter_free(bp, kid);
	}
	rc = bnxt_crypto_del(bp, direction, kid);
	if (!rc) {
		bnxt_free_one_kctx(kctx, kid);
		if (direction == TLS_OFFLOAD_CTX_DIR_TX)
			atomic64_inc(&ktls->ktls_tx_del_count);
		else
			atomic64_inc(&ktls->ktls_rx_del_count);
	}
	atomic_dec(&ktls->pending);
}

static int
bnxt_ktls_dev_resync(struct net_device *dev, struct sock *sk, u32 seq,
		     u8 *rcd_sn, enum tls_offload_ctx_dir direction)
{
	struct bnxt_ktls_offload_ctx_rx *kctx_rx;
	struct ce_resync_resp_ack_cmd cmd = {0};
	struct bnxt *bp = netdev_priv(dev);
	struct bnxt_tx_ring_info *txr;
	struct bnxt_ktls_info *ktls;
	struct bnxt_mpc_info *mpc;
	u32 data;
	int rc;

	if (direction == TLS_OFFLOAD_CTX_DIR_TX)
		return -EOPNOTSUPP;

	ktls = bp->ktls_info;
	atomic_inc(&ktls->pending);
	/* Make sure bnxt_close_nic() sees pending before we check the
	 * BNXT_STATE_OPEN flag.
	 */
	smp_mb__after_atomic();
	if (!test_bit(BNXT_STATE_OPEN, &bp->state)) {
		atomic_dec(&ktls->pending);
		return -ENODEV;
	}
	mpc = bp->mpc_info;
	txr = &mpc->mpc_rings[BNXT_MPC_RCE_TYPE][0];
	kctx_rx = tls_driver_ctx(sk, TLS_OFFLOAD_CTX_DIR_RX);
	data = CE_RESYNC_RESP_ACK_CMD_OPCODE_RESYNC |
	       (kctx_rx->kid << CE_RESYNC_RESP_ACK_CMD_KID_SFT);
	cmd.resync_status_kid_opcode = cpu_to_le32(data);
	cmd.resync_record_tcp_seq_num = cpu_to_le32(seq - TLS_HEADER_SIZE + 1);
	bnxt_copy_tls_mp_data(&cmd.resync_record_seq_num_end, rcd_sn,
			      sizeof(cmd.resync_record_seq_num));
	rc = bnxt_xmit_crypto_cmd(bp, txr, &cmd, sizeof(cmd), 0);
	atomic64_inc(&ktls->ktls_rx_resync_ack_count);
	atomic_dec(&ktls->pending);
	return rc;
}

static const struct tlsdev_ops bnxt_ktls_ops = {
	.tls_dev_add = bnxt_ktls_dev_add,
	.tls_dev_del = bnxt_ktls_dev_del,
	.tls_dev_resync = bnxt_ktls_dev_resync,
};

int bnxt_ktls_init(struct bnxt *bp)
{
	struct bnxt_ktls_info *ktls = bp->ktls_info;
	struct bnxt_hw_resc *hw_resc = &bp->hw_resc;
	struct net_device *dev = bp->dev;
	int rc;

	if (!ktls)
		return 0;

	ktls->tkc.max_ctx = hw_resc->resv_tx_key_ctxs;
	ktls->rkc.max_ctx = hw_resc->resv_rx_key_ctxs;

	if (!ktls->tkc.max_ctx || !ktls->rkc.max_ctx)
		return 0;

	rc = bnxt_hwrm_key_ctx_alloc(bp, &ktls->tkc, BNXT_KID_BATCH_SIZE, NULL);
	if (rc)
		return rc;

	rc = bnxt_hwrm_key_ctx_alloc(bp, &ktls->rkc, BNXT_KID_BATCH_SIZE, NULL);
	if (rc)
		return rc;

	ktls->mpc_cache = kmem_cache_create("bnxt_ktls",
					    sizeof(struct bnxt_crypto_cmd_ctx),
					    0, 0, NULL);
	if (!ktls->mpc_cache)
		return -ENOMEM;

	dev->tlsdev_ops = &bnxt_ktls_ops;
	dev->hw_features |= NETIF_F_HW_TLS_TX | NETIF_F_HW_TLS_RX;
	dev->features |= NETIF_F_HW_TLS_TX | NETIF_F_HW_TLS_RX;
	return 0;
}

void bnxt_ktls_mpc_cmp(struct bnxt *bp, u32 client, unsigned long handle,
		       struct bnxt_cmpl_entry cmpl[], u32 entries)
{
	struct bnxt_crypto_cmd_ctx *ctx;
	struct ce_cmpl *cmp;
	u32 len, kid;

	cmp = cmpl[0].cmpl;
	if (!handle || entries != 1) {
		if (entries != 1) {
			netdev_warn(bp->dev, "Invalid entries %d with handle %lx cmpl %08x in %s()\n",
				    entries, handle, *(u32 *)cmp, __func__);
		}
		return;
	}
	ctx = (void *)handle;
	kid = CE_CMPL_KID(cmp);
	if (ctx->ce_cmp.opaque != BNXT_KMPC_OPAQUE(client, kid)) {
		netdev_warn(bp->dev, "Invalid CE cmpl software opaque %08x, cmpl %08x, kid %x\n",
			    ctx->ce_cmp.opaque, *(u32 *)cmp, kid);
		return;
	}
	len = min_t(u32, cmpl[0].len, sizeof(ctx->ce_cmp));
	memcpy(&ctx->ce_cmp, cmpl[0].cmpl, len);
	complete(&ctx->cmp);
}

struct sk_buff *bnxt_ktls_xmit(struct bnxt *bp, struct sk_buff *skb,
			       __le32 *lflags, u32 *kid)
{
#if defined(BNXT_FPGA)
	struct bnxt_ktls_offload_ctx_tx *kctx_tx;
	u32 seq;

	if (!IS_ENABLED(CONFIG_TLS_DEVICE) || !skb->sk ||
	    !tls_is_sk_tx_device_offloaded(skb->sk))
		return skb;

	seq = ntohl(tcp_hdr(skb)->seq);
	kctx_tx = tls_driver_ctx(skb->sk, TLS_OFFLOAD_CTX_DIR_TX);
	if (kctx_tx->tcp_seq_no == seq) {
		kctx_tx->tcp_seq_no += skb->len - skb_tcp_all_headers(skb);
		*kid = kctx_tx->kid;
		*lflags |= cpu_to_le32(TX_BD_FLAGS_CRYPTO_EN |
				       BNXT_TX_KID_LO(*kid));
		atomic64_inc(&bp->ktls_info->ktls_tx_hw_pkt_count);
	} else {
		skb = tls_encrypt_skb(skb);
		if (!skb)
			return NULL;
		atomic64_inc(&bp->ktls_info->ktls_tx_sw_pkt_count);
		atomic64_inc(&bp->ktls_info->ktls_tx_tls_ooo);
	}
#endif
	return skb;
}

#define BNXT_METADATA_OFF(len)	ALIGN(len, 32)

void bnxt_ktls_rx(struct bnxt *bp, struct sk_buff *skb, u8 *data_ptr,
		  unsigned int len, struct rx_cmp *rxcmp,
		  struct rx_cmp_ext *rxcmp1)
{
#if defined(BNXT_FPGA)
	unsigned int off = BNXT_METADATA_OFF(len);
	struct tls_metadata_base_msg *md;
	u32 md_data;

	md = (struct tls_metadata_base_msg *)(data_ptr + off);
	md_data = le32_to_cpu(md->md_type_link_flags_kid_lo);
	if (md_data & TLS_METADATA_BASE_MSG_FLAGS_DECRYPTED) {
		skb->decrypted = true;
		atomic64_inc(&bp->ktls_info->ktls_rx_hw_pkt_count);
	} else {
		u32 misc = le32_to_cpu(rxcmp->rx_cmp_misc_v1);
		struct tls_metadata_resync_msg *resync_msg;
		u32 payload_off, tcp_seq, md_type;
		struct net_device *dev = bp->dev;
		struct net *net = dev_net(dev);
		u8 agg_bufs, *l3_ptr;
		struct tcphdr *th;
		struct sock *sk;

		payload_off = RX_CMP_PAYLOAD_OFF(misc);
		agg_bufs = (misc & RX_CMP_AGG_BUFS) >> RX_CMP_AGG_BUFS_SHIFT;
		/* No payload */
		if (payload_off == len && !agg_bufs)
			return;

		md_type = md_data & TLS_METADATA_BASE_MSG_MD_TYPE_MASK;
		if (md_type != TLS_METADATA_BASE_MSG_MD_TYPE_TLS_RESYNC)
			goto rx_done_no_sk;

		l3_ptr = data_ptr + RX_CMP_INNER_L3_OFF(rxcmp1);
		if (RX_CMP_IS_IPV6(rxcmp1)) {
			struct ipv6hdr *ip6h = (struct ipv6hdr *)l3_ptr;
			u8 *nextp = (u8 *)(ip6h + 1);
			u8 nexthdr = ip6h->nexthdr;

			while (ipv6_ext_hdr(nexthdr)) {
				struct ipv6_opt_hdr *hp;

				hp = (struct ipv6_opt_hdr *)nextp;
				if (nexthdr == NEXTHDR_AUTH)
					nextp += ipv6_authlen(hp);
				else
					nextp += ipv6_optlen(hp);
				nexthdr = hp->nexthdr;
			}
			th = (struct tcphdr *)nextp;
			sk = __inet6_lookup_established(net,
					net->ipv4.tcp_death_row.hashinfo,
					&ip6h->saddr, th->source, &ip6h->daddr,
					ntohs(th->dest), dev->ifindex, 0);
		} else {
			struct iphdr *iph = (struct iphdr *)l3_ptr;

			th = (struct tcphdr *)(l3_ptr + iph->ihl * 4);
			sk = inet_lookup_established(net,
					net->ipv4.tcp_death_row.hashinfo,
					iph->saddr, th->source, iph->daddr,
					th->dest, dev->ifindex);
		}
		if (!sk)
			goto rx_done_no_sk;

		if (!tls_is_sk_rx_device_offloaded(sk))
			goto rx_done;

		resync_msg = (struct tls_metadata_resync_msg *)md;
		tcp_seq = le32_to_cpu(resync_msg->resync_record_tcp_seq_num);
		tcp_seq += TLS_HEADER_SIZE - 1;
		tls_offload_rx_resync_request(sk, htonl(tcp_seq));
		atomic64_inc(&bp->ktls_info->ktls_rx_resync_req_count);
rx_done:
		sock_gen_put(sk);
rx_done_no_sk:
		atomic64_inc(&bp->ktls_info->ktls_rx_sw_pkt_count);
	}
#endif
}

#else	/* HAVE_KTLS */

void bnxt_hwrm_reserve_pf_key_ctxs(struct bnxt *bp,
				   struct hwrm_func_cfg_input *req)
{
}

int bnxt_ktls_init(struct bnxt *bp)
{
	return 0;
}

void bnxt_ktls_mpc_cmp(struct bnxt *bp, u32 client, unsigned long handle,
		       struct bnxt_cmpl_entry cmpl[], u32 entries)
{
}

struct sk_buff *bnxt_ktls_xmit(struct bnxt *bp, struct sk_buff *skb,
			       __le32 *lflags, u32 *kid)
{
	return skb;
}

void bnxt_ktls_rx(struct bnxt *bp, struct sk_buff *skb, u8 *data_ptr,
		  unsigned int len, struct rx_cmp *rxcmp,
		  struct rx_cmp_ext *rxcmp1)
{
}
#endif	/* HAVE_KTLS */
