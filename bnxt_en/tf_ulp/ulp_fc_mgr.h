/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2019-2021 Broadcom
 * All rights reserved.
 */

#ifndef _ULP_FC_MGR_H_
#define _ULP_FC_MGR_H_

#include "bnxt.h"
#include "bnxt_tf_ulp.h"
#include "tf_core.h"

#define ULP_FLAG_FC_THREAD			BIT(0)
#define ULP_FC_TIMER	100/* Timer freq in Sec Flow Counters */

/* Macros to extract packet/byte counters from a 64-bit flow counter. */
#define FLOW_CNTR_BYTE_WIDTH 36
#define FLOW_CNTR_BYTE_MASK  (((u64)1 << FLOW_CNTR_BYTE_WIDTH) - 1)

#define FLOW_CNTR_PKTS(v, d) (((v) & (d)->packet_count_mask) >> \
		(d)->packet_count_shift)
#define FLOW_CNTR_BYTES(v, d) (((v) & (d)->byte_count_mask) >> \
		(d)->byte_count_shift)

struct sw_acc_counter {
	u64 pkt_count;
	u64 byte_count;
	bool	valid;
	u32 hw_cntr_id;
	enum bnxt_ulp_session_type session_type;
};

struct hw_fc_mem_info {
	/* [out] mem_va, pointer to the allocated memory.
	 */
	void *mem_va;
	/* [out] mem_pa, physical address of the allocated memory.
	 */
	void *mem_pa;
	u32 start_idx;
	bool start_idx_is_set;
};

struct bnxt_ulp_fc_info {
	struct sw_acc_counter	*sw_acc_tbl[TF_DIR_MAX];
	struct hw_fc_mem_info	shadow_hw_tbl[TF_DIR_MAX];
	u32		flags;
	u32		num_entries;
	/* Serialize flow counter thread operations */
	struct mutex		fc_lock;
	u32		num_counters;
};

int
ulp_fc_mgr_init(struct bnxt_ulp_context *ctxt);

/* Release all resources in the flow counter manager for this ulp context
 *
 * @ctxt: The ulp context for the flow counter manager
 */
int
ulp_fc_mgr_deinit(struct bnxt_ulp_context *ctxt);

/* Setup the Flow counter timer thread that will fetch/accumulate raw counter
 * data from the chip's internal flow counters
 *
 * @ctxt: The ulp context for the flow counter manager
 */
void
ulp_fc_mgr_thread_start(struct bnxt_ulp_context *ctxt);

/* Alarm handler that will issue the TF-Core API to fetch
 * data from the chip's internal flow counters
 *
 * @ctxt: The ulp context for the flow counter manager
 */
void
ulp_fc_mgr_alarm_cb(struct work_struct *work);

/* Cancel the alarm handler
 *
 * @ctxt: The ulp context for the flow counter manager
 *
 */
void ulp_fc_mgr_thread_cancel(struct bnxt_ulp_context *ctxt);

/* Set the starting index that indicates the first HW flow
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
			     u32 start_idx);

/* Set the corresponding SW accumulator table entry based on
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
			u32 hw_cntr_id,
			enum bnxt_ulp_session_type session_type);
/* Reset the corresponding SW accumulator table entry based on
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
			  u32 hw_cntr_id);
/* Check if the starting HW counter ID value is set in the
 * flow counter manager.
 *
 * @ctxt: The ulp context for the flow counter manager
 *
 * @dir: The direction of the flow
 *
 */
bool ulp_fc_mgr_start_idx_isset(struct bnxt_ulp_context *ctxt, enum tf_dir dir);

/* Check if the alarm thread that walks through the flows is started
 *
 * @ctxt: The ulp context for the flow counter manager
 *
 */
bool ulp_fc_mgr_thread_isstarted(struct bnxt_ulp_context *ctxt);

/* Fill packets & bytes with the values obtained and
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
				  unsigned long *lastused);

#endif /* _ULP_FC_MGR_H_ */
