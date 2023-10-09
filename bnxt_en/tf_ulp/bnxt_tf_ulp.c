// SPDX-License-Identifier: BSD-3-Clause
/* Copyright(c) 2019-2021 Broadcom
 * All rights reserved.
 */

#include "ulp_linux.h"
#include "bnxt_compat.h"
#include "bnxt_hsi.h"
#include "bnxt.h"
#include "bnxt_vfr.h"
#include "bnxt_tf_ulp.h"
#include "bnxt_ulp_flow.h"
#include "bnxt_tf_common.h"
#include "tf_core.h"
#include "tf_ext_flow_handle.h"

#include "ulp_template_db_enum.h"
#include "ulp_template_struct.h"
#include "ulp_mark_mgr.h"
#include "ulp_fc_mgr.h"
#include "ulp_flow_db.h"
#include "ulp_mapper.h"
#include "ulp_matcher.h"
#include "ulp_port_db.h"

#if defined(CONFIG_BNXT_FLOWER_OFFLOAD) || defined(CONFIG_BNXT_CUSTOM_FLOWER_OFFLOAD)
/* Linked list of all TF sessions. */
HLIST_HEAD(bnxt_ulp_session_list);
/* Mutex to synchronize bnxt_ulp_session_list operations. */
DEFINE_MUTEX(bnxt_ulp_global_mutex);

/* Spin lock to protect context global list */
DEFINE_MUTEX(bnxt_ulp_ctxt_lock);
static HLIST_HEAD(ulp_cntx_list);

/* Static function declarations */
static int bnxt_ulp_cntxt_list_add(struct bnxt_ulp_context *ulp_ctx);
static void bnxt_ulp_cntxt_list_del(struct bnxt_ulp_context *ulp_ctx);

/* Allow the deletion of context only for the bnxt device that
 * created the session.
 */
bool
ulp_ctx_deinit_allowed(struct bnxt_ulp_context *ulp_ctx)
{
	if (!ulp_ctx || !ulp_ctx->cfg_data)
		return false;

	if (!ulp_ctx->cfg_data->ref_cnt) {
		netdev_dbg(ulp_ctx->bp->dev, "ulp ctx shall initiate deinit\n");
		return true;
	}

	return false;
}

int
bnxt_ulp_devid_get(struct bnxt *bp)
{
	if (BNXT_CHIP_THOR(bp))
		return BNXT_ULP_DEVICE_ID_THOR;
	else
		return BNXT_ULP_DEVICE_ID_WH_PLUS;
}

struct tf *bnxt_ulp_bp_tfp_get(struct bnxt *bp,
			       enum bnxt_ulp_session_type type)
{
	enum bnxt_session_type btype;

	if (type & BNXT_ULP_SESSION_TYPE_SHARED)
		btype = BNXT_SESSION_TYPE_SHARED_COMMON;
	else if (type & BNXT_ULP_SESSION_TYPE_SHARED_WC)
		btype = BNXT_SESSION_TYPE_SHARED_WC;
	else
		btype = BNXT_SESSION_TYPE_REGULAR;

	return bnxt_get_tfp_session(bp, btype);
}

bool bnxt_ulp_cntxt_shared_session_enabled(struct bnxt_ulp_context *ulp_ctx)
{
	return ULP_SHARED_SESSION_IS_ENABLED(ulp_ctx->cfg_data->ulp_flags);
}

struct bnxt_ulp_app_capabilities_info *
bnxt_ulp_app_cap_list_get(u32 *num_entries)
{
	if (!num_entries)
		return NULL;
	*num_entries = BNXT_ULP_APP_CAP_TBL_MAX_SZ;
	return ulp_app_cap_info_list;
}

struct bnxt_ulp_resource_resv_info *
bnxt_ulp_resource_resv_list_get(u32 *num_entries)
{
	if (!num_entries)
		return NULL;
	*num_entries = BNXT_ULP_RESOURCE_RESV_LIST_MAX_SZ;
	return ulp_resource_resv_list;
}

static struct bnxt_ulp_resource_resv_info *
bnxt_ulp_app_resource_resv_list_get(u32 *num_entries)
{
	if (!num_entries)
		return NULL;
	*num_entries = BNXT_ULP_APP_RESOURCE_RESV_LIST_MAX_SZ;
	return ulp_app_resource_resv_list;
}

struct bnxt_ulp_glb_resource_info *
bnxt_ulp_app_glb_resource_info_list_get(u32 *num_entries)
{
	if (!num_entries)
		return NULL;
	*num_entries = BNXT_ULP_APP_GLB_RESOURCE_TBL_MAX_SZ;
	return ulp_app_glb_resource_tbl;
}

static int
bnxt_ulp_unnamed_resources_calc(struct bnxt_ulp_context *ulp_ctx,
				struct bnxt_ulp_resource_resv_info *info,
				u32 num,
				enum bnxt_ulp_session_type stype,
				struct tf_session_resources *res)
{
	struct bnxt *bp;
	u32 dev_id, res_type, i;
	enum tf_dir dir;
	int rc = 0;
	u8 app_id;

	if (!ulp_ctx || !res || !info || num == 0)
		return -EINVAL;

	bp = ulp_ctx->bp;

	rc = bnxt_ulp_cntxt_app_id_get(ulp_ctx, &app_id);
	if (rc) {
		netdev_dbg(bp->dev, "Unable to get the app id from ulp.\n");
		return -EINVAL;
	}

	dev_id = bnxt_ulp_devid_get(bp);

	for (i = 0; i < num; i++) {
		if (app_id != info[i].app_id || dev_id != info[i].device_id)
			continue;

		/* check to see if session type matches only then include */
		if ((stype || info[i].session_type) &&
		    !(info[i].session_type & stype))
			continue;

		dir = info[i].direction;
		res_type = info[i].resource_type;

		switch (info[i].resource_func) {
		case BNXT_ULP_RESOURCE_FUNC_IDENTIFIER:
			res->ident_cnt[dir].cnt[res_type] = info[i].count;
			break;
		case BNXT_ULP_RESOURCE_FUNC_INDEX_TABLE:
			res->tbl_cnt[dir].cnt[res_type] = info[i].count;
			break;
		case BNXT_ULP_RESOURCE_FUNC_TCAM_TABLE:
			res->tcam_cnt[dir].cnt[res_type] = info[i].count;
			break;
		case BNXT_ULP_RESOURCE_FUNC_EM_TABLE:
			res->em_cnt[dir].cnt[res_type] = info[i].count;
			break;
		default:
			netdev_dbg(bp->dev, "Unsupported resource\n");
			return -EINVAL;
		}
	}
	return 0;
}

static int
bnxt_ulp_named_resources_calc(struct bnxt_ulp_context *ulp_ctx,
			      struct bnxt_ulp_glb_resource_info *info,
			      u32 num,
			      enum bnxt_ulp_session_type stype,
			      struct tf_session_resources *res)
{
	u32 dev_id = BNXT_ULP_DEVICE_ID_LAST, res_type, i;
	enum tf_dir dir;
	struct bnxt *bp;
	int rc = 0;
	u8 app_id;

	if (!ulp_ctx || !info || !res || !num)
		return -EINVAL;

	bp = ulp_ctx->bp;

	rc = bnxt_ulp_cntxt_app_id_get(ulp_ctx, &app_id);
	if (rc) {
		netdev_dbg(bp->dev, "Unable to get the app id from ulp.\n");
		return -EINVAL;
	}

	dev_id = bnxt_ulp_devid_get(bp);

	for (i = 0; i < num; i++) {
		if (dev_id != info[i].device_id || app_id != info[i].app_id)
			continue;
		/* check to see if the session type matches only then include */
		if ((stype || info[i].session_type) &&
		    !(info[i].session_type & stype))
			continue;

		dir = info[i].direction;
		res_type = info[i].resource_type;

		switch (info[i].resource_func) {
		case BNXT_ULP_RESOURCE_FUNC_IDENTIFIER:
			res->ident_cnt[dir].cnt[res_type]++;
			break;
		case BNXT_ULP_RESOURCE_FUNC_INDEX_TABLE:
			res->tbl_cnt[dir].cnt[res_type]++;
			break;
		case BNXT_ULP_RESOURCE_FUNC_TCAM_TABLE:
			res->tcam_cnt[dir].cnt[res_type]++;
			break;
		case BNXT_ULP_RESOURCE_FUNC_EM_TABLE:
			res->em_cnt[dir].cnt[res_type]++;
			break;
		default:
			netdev_dbg(bp->dev, "Unknown resource func (0x%x)\n,",
				   info[i].resource_func);
			continue;
		}
	}

	return 0;
}

static int
bnxt_ulp_tf_resources_get(struct bnxt_ulp_context *ulp_ctx,
			  struct tf_session_resources *res)
{
	struct bnxt_ulp_resource_resv_info *unnamed = NULL;
	struct bnxt *bp;
	int rc = 0;
	u32 unum;

	if (!ulp_ctx || !res)
		return -EINVAL;

	bp = ulp_ctx->bp;

	unnamed = bnxt_ulp_resource_resv_list_get(&unum);
	if (!unnamed) {
		netdev_dbg(bp->dev, "Unable to get resource resv list.\n");
		return -EINVAL;
	}

	rc = bnxt_ulp_unnamed_resources_calc(ulp_ctx, unnamed, unum,
					     BNXT_ULP_SESSION_TYPE_DEFAULT,
					     res);
	if (rc)
		netdev_dbg(bp->dev, "Unable to calc resources for session.\n");

	return rc;
}

static int
bnxt_ulp_tf_shared_session_resources_get(struct bnxt_ulp_context *ulp_ctx,
					 enum bnxt_ulp_session_type stype,
					 struct tf_session_resources *res)
{
	struct bnxt_ulp_resource_resv_info *unnamed;
	struct bnxt_ulp_glb_resource_info *named;
	struct bnxt *bp;
	u32 unum, nnum;
	int rc;

	if (!ulp_ctx || !res)
		return -EINVAL;

	bp = ulp_ctx->bp;

	/* Make sure the resources are zero before accumulating. */
	memset(res, 0, sizeof(struct tf_session_resources));

	/* Shared resources are comprised of both named and unnamed resources.
	 * First get the unnamed counts, and then add the named to the result.
	 */

	/* Get the baseline counts */
	unnamed = bnxt_ulp_app_resource_resv_list_get(&unum);
	if (!unnamed) {
		netdev_dbg(bp->dev,
			   "Unable to get shared resource resv list\n");
		return -EINVAL;
	}

	rc = bnxt_ulp_unnamed_resources_calc(ulp_ctx, unnamed, unum, stype,
					     res);
	if (rc) {
		netdev_dbg(bp->dev,
			   "Unable to calc resources for shared session.\n");
		return -EINVAL;
	}

	/* Get the named list and add the totals */
	named = bnxt_ulp_app_glb_resource_info_list_get(&nnum);
	if (!named) {
		netdev_dbg(bp->dev,
			   "Unable to get app global resource list\n");
		return -EINVAL;
	}
	rc = bnxt_ulp_named_resources_calc(ulp_ctx, named, nnum, stype, res);
	if (rc)
		netdev_dbg(bp->dev, "Unable to calc named resources\n");

	return rc;
}

int
bnxt_ulp_cntxt_app_caps_init(struct bnxt_ulp_context *ulp_ctx,
			     u8 app_id, u32 dev_id)
{
	struct bnxt_ulp_app_capabilities_info *info;
	struct bnxt *bp = ulp_ctx->bp;
	bool found = false;
	u32 num = 0;
	u16 i;

	if (ULP_APP_DEV_UNSUPPORTED_ENABLED(ulp_ctx->cfg_data->ulp_flags)) {
		netdev_dbg(bp->dev, "%s: APP ID %d, Device ID: 0x%x not supported.\n",
			   __func__,
			    app_id, dev_id);
		return -EINVAL;
	}

	info = bnxt_ulp_app_cap_list_get(&num);
	if (!info || !num) {
		netdev_dbg(bp->dev, "Failed to get app capabilities.\n");
		return -EINVAL;
	}

	for (i = 0; i < num; i++) {
		if (info[i].app_id != app_id || info[i].device_id != dev_id)
			continue;
		found = true;
		if (info[i].flags & BNXT_ULP_APP_CAP_SHARED_EN)
			ulp_ctx->cfg_data->ulp_flags |=
				BNXT_ULP_SHARED_SESSION_ENABLED;
		if (info[i].flags & BNXT_ULP_APP_CAP_HOT_UPGRADE_EN)
			ulp_ctx->cfg_data->ulp_flags |=
				BNXT_ULP_HIGH_AVAIL_ENABLED;
		if (info[i].flags & BNXT_ULP_APP_CAP_UNICAST_ONLY)
			ulp_ctx->cfg_data->ulp_flags |=
				BNXT_ULP_APP_UNICAST_ONLY;
		bnxt_ulp_cntxt_ptr2_wc_num_slices_set(ulp_ctx,
						      info[i].wc_num_slices);
	}
	if (!found) {
		netdev_dbg(bp->dev, "%s: APP ID %d, Device ID: 0x%x not found.\n",
			   __func__,
			    app_id, dev_id);
		ulp_ctx->cfg_data->ulp_flags |= BNXT_ULP_APP_DEV_UNSUPPORTED;
		return -EINVAL;
	}

	return 0;
}

static int bnxt_ulp_ctx_convert_dev_id(u32 ulp_dev_id,
				       enum tf_device_type *type)
{
	switch (ulp_dev_id) {
	case BNXT_ULP_DEVICE_ID_WH_PLUS:
		*type = TF_DEVICE_TYPE_WH;
		break;
	case BNXT_ULP_DEVICE_ID_THOR:
		*type = TF_DEVICE_TYPE_THOR;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static void bnxt_ulp_get_ctrl_chan_name(struct bnxt *bp,
					struct tf_open_session_parms *params)
{
	struct net_device *dev = bp->dev;

	memset(params->ctrl_chan_name, '\0', TF_SESSION_NAME_MAX);

	if ((strlen(dev_name(dev->dev.parent)) >=
	     strlen(params->ctrl_chan_name)) ||
	    (strlen(dev_name(dev->dev.parent)) >=
	     sizeof(params->ctrl_chan_name))) {
		strncpy(params->ctrl_chan_name, dev_name(dev->dev.parent),
			TF_SESSION_NAME_MAX - 1);
		/* Make sure the string is terminated */
		params->ctrl_chan_name[TF_SESSION_NAME_MAX - 1] = '\0';
		return;
	}

	strcpy(params->ctrl_chan_name, dev_name(dev->dev.parent));
}

static inline u32
bnxt_ulp_session_idx_get(enum bnxt_ulp_session_type session_type) {
	if (session_type & BNXT_ULP_SESSION_TYPE_SHARED)
		return 1;
	return 0;
}

static int bnxt_ulp_session_tfp_set(struct bnxt_ulp_session_state *session,
				    enum bnxt_ulp_session_type session_type,
				    struct tf *tfp)
{
	u32 idx = bnxt_ulp_session_idx_get(session_type);

	if (!session->session_opened[idx]) {
		session->g_tfp[idx] = vzalloc(sizeof(*tfp));
		if (!session->g_tfp[idx])
			return -ENOMEM;
		session->g_tfp[idx]->session  = tfp->session;
		session->session_opened[idx] = 1;
	}
	return 0;
}

static struct tf_session_info *
bnxt_ulp_session_tfp_get(struct bnxt_ulp_session_state *session,
			 enum bnxt_ulp_session_type session_type)
{
	u32 idx = bnxt_ulp_session_idx_get(session_type);

	if (session->session_opened[idx])
		return session->g_tfp[idx]->session;
	return NULL;
}

static u32 bnxt_ulp_session_is_open(struct bnxt_ulp_session_state *session,
				    enum bnxt_ulp_session_type session_type)
{
	u32 idx = bnxt_ulp_session_idx_get(session_type);

	return session->session_opened[idx];
}

/* Function to reset the tfp session details in session */
static void bnxt_ulp_session_tfp_reset(struct bnxt_ulp_session_state *session,
				       enum bnxt_ulp_session_type session_type)
{
	u32 idx = bnxt_ulp_session_idx_get(session_type);

	if (!session->session_opened[idx])
		return;

	session->session_opened[idx] = 0;
	vfree(session->g_tfp[idx]);
	session->g_tfp[idx] = NULL;
}

/* Function to get the number of shared clients attached */
u8 bnxt_ulp_cntxt_num_shared_clients_get(struct bnxt_ulp_context *ulp)
{
	if (!ulp || !ulp->cfg_data)
		return 0;

	return ulp->cfg_data->num_shared_clients;
}

/* Function to set the number of shared clients */
int bnxt_ulp_cntxt_num_shared_clients_set(struct bnxt_ulp_context *ulp,
					  bool incr)
{
	if (!ulp || !ulp->cfg_data)
		return 0;

	if (incr)
		ulp->cfg_data->num_shared_clients++;
	else if (ulp->cfg_data->num_shared_clients)
		ulp->cfg_data->num_shared_clients--;

	return 0;
}

static void ulp_ctx_shared_session_close(struct bnxt *bp,
					 enum bnxt_ulp_session_type
						session_type,
					 struct bnxt_ulp_session_state
						*session)
{
	struct tf *tfp;
	int rc;

	if (session_type != BNXT_ULP_SESSION_TYPE_SHARED) {
		netdev_dbg(bp->dev, "Unsupported shared session type: %d\n",
			   session_type);
		return;
	}

	tfp = bnxt_ulp_cntxt_tfp_get(bp->ulp_ctx, session_type);
	if (!tfp) {
		/* Log it under debug since this is likely a case of the
		 * shared session not being created.  For example, a failed
		 * initialization.
		 */
		netdev_dbg(bp->dev, "Failed to get shared tfp on close\n");
		return;
	}

	rc = tf_close_session(tfp);
	if (rc)
		netdev_dbg(bp->dev,
			   "Failed to close the shared session rc=%d\n", rc);

	bnxt_ulp_cntxt_tfp_set(bp->ulp_ctx, session_type, NULL);
	bnxt_ulp_session_tfp_reset(session, session_type);
}

static int ulp_ctx_shared_session_open(struct bnxt *bp,
				       enum bnxt_ulp_session_type session_type,
				       struct bnxt_ulp_session_state *session)
{
	uint ulp_dev_id = BNXT_ULP_DEVICE_ID_LAST;
	struct tf_session_resources *resources;
	struct tf_open_session_parms parms;
	struct tf *tfp;
	int rc = 0;
	size_t nb;
	u8 app_id;

	if (session_type != BNXT_ULP_SESSION_TYPE_SHARED) {
		netdev_dbg(bp->dev, "Unsupported shared session type: %d\n",
			   session_type);
		return -EINVAL;
	}

	memset(&parms, 0, sizeof(parms));
	bnxt_ulp_get_ctrl_chan_name(bp, &parms);

	resources = &parms.resources;

	/* Need to account for size of ctrl_chan_name and 1 extra for Null
	 * terminator
	 */
	nb = sizeof(parms.ctrl_chan_name) - strlen(parms.ctrl_chan_name) - 1;

	/* Build the ctrl_chan_name with shared token. */
	strncat(parms.ctrl_chan_name, "-tf_shared", nb);

	rc = bnxt_ulp_tf_shared_session_resources_get(bp->ulp_ctx,
						      session_type, resources);
	if (rc) {
		netdev_dbg(bp->dev,
			   "Failed to get shared session resources: %d\n", rc);
		return rc;
	}

	rc = bnxt_ulp_cntxt_app_id_get(bp->ulp_ctx, &app_id);
	if (rc) {
		netdev_dbg(bp->dev, "Unable to get the app id from ulp\n");
		return rc;
	}

	ulp_dev_id = bnxt_ulp_devid_get(bp);
	rc = bnxt_ulp_ctx_convert_dev_id(ulp_dev_id, &parms.device_type);
	if (rc) {
		netdev_dbg(bp->dev,
			   "Unable to get the device type from ulp\n");
		return rc;
	}

	parms.shadow_copy = true;
	parms.bp = bp;
	parms.wc_num_slices =
		bnxt_ulp_cntxt_ptr2_wc_num_slices_get(bp->ulp_ctx);

	/* Open the session here, but the collect the resources during the
	 * mapper initialization.
	 */
	tfp = bnxt_ulp_bp_tfp_get(bp, session_type);
	rc = tf_open_session(tfp, &parms);
	if (rc)
		return rc;

	if (parms.shared_session_creator)
		netdev_dbg(bp->dev, "Shared session creator\n");
	else
		netdev_dbg(bp->dev, "Shared session attached\n");

	/* Save the shared session in global data */
	rc = bnxt_ulp_session_tfp_set(session, session_type, tfp);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to add shared tfp to session\n");
		return rc;
	}

	rc = bnxt_ulp_cntxt_tfp_set(bp->ulp_ctx, session_type, tfp);
	if (rc) {
		netdev_dbg(bp->dev,
			   "Failed to add shared tfp to ulp: %d\n", rc);
		return rc;
	}

	return rc;
}

static int ulp_ctx_shared_session_attach(struct bnxt *bp,
					 struct bnxt_ulp_session_state *ses)
{
	enum bnxt_ulp_session_type type;
	struct tf *tfp;
	int rc = 0;

	/* Simply return success if shared session not enabled */
	if (bnxt_ulp_cntxt_shared_session_enabled(bp->ulp_ctx)) {
		type = BNXT_ULP_SESSION_TYPE_SHARED;
		tfp = bnxt_ulp_bp_tfp_get(bp, type);
		tfp->session = bnxt_ulp_session_tfp_get(ses, type);
		rc = ulp_ctx_shared_session_open(bp, type, ses);
	}

	if (!rc)
		bnxt_ulp_cntxt_num_shared_clients_set(bp->ulp_ctx, true);

	return rc;
}

static void ulp_ctx_shared_session_detach(struct bnxt *bp)
{
	struct tf *tfp;

	if (bnxt_ulp_cntxt_shared_session_enabled(bp->ulp_ctx)) {
		tfp = bnxt_ulp_bp_tfp_get(bp, BNXT_ULP_SESSION_TYPE_SHARED);
		if (tfp->session) {
			tf_close_session(tfp);
			tfp->session = NULL;
		}
	}
	bnxt_ulp_cntxt_num_shared_clients_set(bp->ulp_ctx, false);
}

/* Initialize an ULP session.
 * An ULP session will contain all the resources needed to support flow
 * offloads. A session is initialized as part of switchdev mode transition.
 * A single vswitch instance can have multiple uplinks which means
 * switchdev mode transitino will be called for each of these devices.
 * ULP session manager will make sure that a single ULP session is only
 * initialized once. Apart from this, it also initializes MARK database,
 * EEM table & flow database. ULP session manager also manages a list of
 * all opened ULP sessions.
 */
static int ulp_ctx_session_open(struct bnxt *bp,
				struct bnxt_ulp_session_state *session)
{
	u32 ulp_dev_id = BNXT_ULP_DEVICE_ID_LAST;
	struct tf_session_resources *resources;
	struct tf_open_session_parms params = {{ 0 }};
	struct net_device *dev = bp->dev;
	int	rc = 0;
	struct tf *tfp;

	memset(&params, 0, sizeof(params));
	memset(params.ctrl_chan_name, '\0', TF_SESSION_NAME_MAX);
	if ((strlen(dev_name(dev->dev.parent)) >= strlen(params.ctrl_chan_name)) ||
	    (strlen(dev_name(dev->dev.parent)) >= sizeof(params.ctrl_chan_name))) {
		strncpy(params.ctrl_chan_name, dev_name(dev->dev.parent), TF_SESSION_NAME_MAX - 1);
		/* Make sure the string is terminated */
		params.ctrl_chan_name[TF_SESSION_NAME_MAX - 1] = '\0';
	} else {
		strcpy(params.ctrl_chan_name, dev_name(dev->dev.parent));
	}

	ulp_dev_id = bnxt_ulp_devid_get(bp);
	rc = bnxt_ulp_ctx_convert_dev_id(ulp_dev_id, &params.device_type);
	if (rc) {
		netdev_dbg(bp->dev,
			   "Unable to get the device type from ulp\n");
		return rc;
	}

	resources = &params.resources;
	rc = bnxt_ulp_tf_resources_get(bp->ulp_ctx, resources);
	if (rc)
		return rc;

	params.bp = bp;
	params.wc_num_slices = bnxt_ulp_cntxt_ptr2_wc_num_slices_get(bp->ulp_ctx);
	tfp = bnxt_ulp_bp_tfp_get(bp, BNXT_ULP_SESSION_TYPE_DEFAULT);
	rc = tf_open_session(tfp, &params);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to open TF session - %s, rc = %d\n",
			   params.ctrl_chan_name, rc);
		return -EINVAL;
	}
	rc = bnxt_ulp_session_tfp_set(session,
				      BNXT_ULP_SESSION_TYPE_DEFAULT, tfp);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to set TF session - %s, rc = %d\n",
			   params.ctrl_chan_name, rc);
		return -EINVAL;
	}
	return rc;
}

/* Close the ULP session.
 * It takes the ulp context pointer.
 */
static void ulp_ctx_session_close(struct bnxt *bp,
				  struct bnxt_ulp_session_state *session)
{
	struct tf *tfp;

	/* close the session in the hardware */
	if (bnxt_ulp_session_is_open(session, BNXT_ULP_SESSION_TYPE_DEFAULT)) {
		tfp = bnxt_ulp_bp_tfp_get(bp, BNXT_ULP_SESSION_TYPE_DEFAULT);
		tf_close_session(tfp);
	}
	bnxt_ulp_session_tfp_reset(session, BNXT_ULP_SESSION_TYPE_DEFAULT);
}

static void
bnxt_init_tbl_scope_parms(struct bnxt *bp,
			  struct tf_alloc_tbl_scope_parms *params)
{
	struct bnxt_ulp_device_params *dparms;
	u32 dev_id;

	dev_id = bnxt_ulp_devid_get(bp);
	dparms = bnxt_ulp_device_params_get(dev_id);

	/* Set the flush timer for EEM entries. The value is in 100ms intervals,
	 * so 100 is 10s.
	 */
	params->hw_flow_cache_flush_timer = 100;

	if (!dparms) {
		params->rx_max_key_sz_in_bits = BNXT_ULP_DFLT_RX_MAX_KEY;
		params->rx_max_action_entry_sz_in_bits =
			BNXT_ULP_DFLT_RX_MAX_ACTN_ENTRY;
		params->rx_mem_size_in_mb = BNXT_ULP_DFLT_RX_MEM;
		params->rx_num_flows_in_k = BNXT_ULP_RX_NUM_FLOWS;

		params->tx_max_key_sz_in_bits = BNXT_ULP_DFLT_TX_MAX_KEY;
		params->tx_max_action_entry_sz_in_bits =
			BNXT_ULP_DFLT_TX_MAX_ACTN_ENTRY;
		params->tx_mem_size_in_mb = BNXT_ULP_DFLT_TX_MEM;
		params->tx_num_flows_in_k = BNXT_ULP_TX_NUM_FLOWS;
	} else {
		params->rx_max_key_sz_in_bits = BNXT_ULP_DFLT_RX_MAX_KEY;
		params->rx_max_action_entry_sz_in_bits =
			BNXT_ULP_DFLT_RX_MAX_ACTN_ENTRY;
		params->rx_mem_size_in_mb = BNXT_ULP_DFLT_RX_MEM;
		params->rx_num_flows_in_k =
			dparms->ext_flow_db_num_entries / 1024;

		params->tx_max_key_sz_in_bits = BNXT_ULP_DFLT_TX_MAX_KEY;
		params->tx_max_action_entry_sz_in_bits =
			BNXT_ULP_DFLT_TX_MAX_ACTN_ENTRY;
		params->tx_mem_size_in_mb = BNXT_ULP_DFLT_TX_MEM;
		params->tx_num_flows_in_k =
			dparms->ext_flow_db_num_entries / 1024;
	}
	netdev_dbg(bp->dev, "Table Scope initialized with %uK flows.\n",
		   params->rx_num_flows_in_k);
}

/* Initialize Extended Exact Match host memory. */
static int
ulp_eem_tbl_scope_init(struct bnxt *bp)
{
	struct tf_alloc_tbl_scope_parms params = {0};
	struct bnxt_ulp_device_params *dparms;
	enum bnxt_ulp_flow_mem_type mtype;
	u32 dev_id;

	dev_id = bnxt_ulp_devid_get(bp);

	dparms = bnxt_ulp_device_params_get(dev_id);
	if (!dparms) {
		netdev_dbg(bp->dev, "could not fetch the device params\n");
		return -ENODEV;
	}

	if (bnxt_ulp_cntxt_mem_type_get(bp->ulp_ctx, &mtype))
		return -EINVAL;
	if (mtype != BNXT_ULP_FLOW_MEM_TYPE_EXT) {
		netdev_dbg(bp->dev, "Table Scope alloc is not required\n");
		return 0;
	}

	bnxt_init_tbl_scope_parms(bp, &params);

	return 0;
}

/* Free Extended Exact Match host memory */
static int
ulp_eem_tbl_scope_deinit(struct bnxt *bp, struct bnxt_ulp_context *ulp_ctx)
{
	struct bnxt_ulp_device_params *dparms;
	enum bnxt_ulp_flow_mem_type mtype;
	struct tf *tfp;
	u32 dev_id;

	if (!ulp_ctx || !ulp_ctx->cfg_data)
		return -EINVAL;

	tfp = bnxt_ulp_cntxt_tfp_get(ulp_ctx, BNXT_ULP_SESSION_TYPE_DEFAULT);
	if (!tfp) {
		netdev_dbg(bp->dev, "Failed to get the truflow pointer\n");
		return -EINVAL;
	}

	dev_id = bnxt_ulp_devid_get(bp);

	dparms = bnxt_ulp_device_params_get(dev_id);
	if (!dparms) {
		netdev_dbg(bp->dev, "could not fetch the device params\n");
		return -ENODEV;
	}

	if (bnxt_ulp_cntxt_mem_type_get(ulp_ctx, &mtype))
		return -EINVAL;
	if (mtype != BNXT_ULP_FLOW_MEM_TYPE_EXT) {
		netdev_dbg(bp->dev, "Table Scope free is not required\n");
		return 0;
	}

	return 0;
}

/* The function to free and deinit the ulp context data. */
static int
ulp_ctx_deinit(struct bnxt *bp,
	       struct bnxt_ulp_session_state *session)
{
	struct bnxt_ulp_context *ulp_ctx = bp->ulp_ctx;

	/* close the tf session */
	ulp_ctx_session_close(bp, session);

	/* The shared session must be closed last. */
	if (bnxt_ulp_cntxt_shared_session_enabled(ulp_ctx))
		ulp_ctx_shared_session_close(bp, BNXT_ULP_SESSION_TYPE_SHARED,
					     session);

	bnxt_ulp_cntxt_num_shared_clients_set(ulp_ctx, false);

	/* Free the contents */
	if (session->cfg_data) {
		vfree(session->cfg_data);
		ulp_ctx->cfg_data = NULL;
		session->cfg_data = NULL;
	}

	return 0;
}

/* The function to allocate and initialize the ulp context data. */
static int
ulp_ctx_init(struct bnxt *bp,
	     struct bnxt_ulp_session_state *session)
{
	struct bnxt_ulp_context *ulp_ctx = bp->ulp_ctx;
	struct bnxt_ulp_data *ulp_data;
	enum bnxt_ulp_device_id devid;
	struct tf *tfp;
	u8 app_id = 0;
	int rc = 0;

	/* Add the context to the context entries list */
	rc = bnxt_ulp_cntxt_list_add(ulp_ctx);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to add the context list entry\n");
		return -ENOMEM;
	}

	/* Allocate memory to hold ulp context data. */
	ulp_data = vzalloc(sizeof(*ulp_data));
	if (!ulp_data)
		goto error_deinit;

	/* Increment the ulp context data reference count usage. */
	ulp_ctx->cfg_data = ulp_data;
	session->cfg_data = ulp_data;
	ulp_data->ref_cnt++;
	ulp_data->ulp_flags |= BNXT_ULP_VF_REP_ENABLED;

	devid = bnxt_ulp_devid_get(bp);

	if (!(bp->app_id & BNXT_ULP_APP_ID_SET_CONFIGURED)) {
		bp->app_id = BNXT_ULP_APP_ID_CONFIG;
		bp->app_id |= BNXT_ULP_APP_ID_SET_CONFIGURED;
	}
	app_id = bp->app_id & ~BNXT_ULP_APP_ID_SET_CONFIGURED;

	rc = bnxt_ulp_cntxt_app_id_set(ulp_ctx, app_id);
	if (rc) {
		netdev_dbg(bp->dev, "Unable to set app_id for ULP init.\n");
		goto error_deinit;
	}

	rc = bnxt_ulp_cntxt_app_caps_init(ulp_ctx, app_id, devid);
	if (rc) {
		netdev_dbg(bp->dev, "Unable to set caps for app(%x)/dev(%x)\n",
			   app_id, devid);
		goto error_deinit;
	}

	/* Shared session must be created before regular
	 * session but after the ulp_ctx is valid.
	 */
	if (bnxt_ulp_cntxt_shared_session_enabled(ulp_ctx)) {
		rc = ulp_ctx_shared_session_open(bp,
						 BNXT_ULP_SESSION_TYPE_SHARED,
						 session);
		if (rc) {
			netdev_dbg(bp->dev,
				   "Unable to open shared session: %d\n", rc);
			goto error_deinit;
		}
	}
	bnxt_ulp_cntxt_num_shared_clients_set(ulp_ctx, true);

	/* Open the ulp session. */
	rc = ulp_ctx_session_open(bp, session);
	if (rc)
		goto error_deinit;

	tfp = bnxt_ulp_bp_tfp_get(bp, BNXT_ULP_SESSION_TYPE_DEFAULT);
	bnxt_ulp_cntxt_tfp_set(ulp_ctx, BNXT_ULP_SESSION_TYPE_DEFAULT, tfp);

	return rc;

error_deinit:
	session->session_opened[BNXT_ULP_SESSION_TYPE_DEFAULT] = 1;
	(void)ulp_ctx_deinit(bp, session);
	return rc;
}

/* The function to initialize bp flags with truflow features */
static int
ulp_dparms_dev_port_intf_update(struct bnxt *bp,
				struct bnxt_ulp_context *ulp_ctx)
{
	enum bnxt_ulp_flow_mem_type mtype;

	if (bnxt_ulp_cntxt_mem_type_get(ulp_ctx, &mtype))
		return -EINVAL;
	/* Update the bp flag with gfid flag */
	if (mtype == BNXT_ULP_FLOW_MEM_TYPE_EXT)
		bp->tf_flags |= BNXT_TF_FLAG_GFID_ENABLE;

	return 0;
}

static int
ulp_ctx_attach(struct bnxt *bp,
	       struct bnxt_ulp_session_state *session)
{
	struct bnxt_ulp_context *ulp_ctx = bp->ulp_ctx;
	u32 flags, dev_id = BNXT_ULP_DEVICE_ID_LAST;
	struct tf *tfp;
	int rc = 0;
	u8 app_id;

	/* Increment the ulp context data reference count usage. */
	ulp_ctx->cfg_data = session->cfg_data;
	ulp_ctx->cfg_data->ref_cnt++;

	/* update the session details in bnxt tfp */
	tfp = bnxt_ulp_bp_tfp_get(bp, BNXT_ULP_SESSION_TYPE_DEFAULT);
	tfp->session = bnxt_ulp_session_tfp_get(session,
						BNXT_ULP_SESSION_TYPE_DEFAULT);

	/* Add the context to the context entries list */
	rc = bnxt_ulp_cntxt_list_add(ulp_ctx);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to add the context list entry\n");
		return -EINVAL;
	}

	/* The supported flag will be set during the init. Use it now to
	 * know if we should go through the attach.
	 */
	rc = bnxt_ulp_cntxt_app_id_get(ulp_ctx, &app_id);
	if (rc) {
		netdev_dbg(bp->dev, "Unable to get the app id from ulp.\n");
		return -EINVAL;
	}

	dev_id = bnxt_ulp_devid_get(bp);

	flags = ulp_ctx->cfg_data->ulp_flags;
	if (ULP_APP_DEV_UNSUPPORTED_ENABLED(flags)) {
		netdev_dbg(bp->dev,
			   "%s: APP ID %d, Device ID: 0x%x not supported.\n",
			   __func__, app_id, dev_id);
		return -EINVAL;
	}

	/* Create a TF Client */
	rc = ulp_ctx_session_open(bp, session);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to open ctxt session, rc:%d\n", rc);
		tfp->session = NULL;
		return rc;
	}

	tfp = bnxt_ulp_bp_tfp_get(bp, BNXT_ULP_SESSION_TYPE_DEFAULT);
	bnxt_ulp_cntxt_tfp_set(ulp_ctx, BNXT_ULP_SESSION_TYPE_DEFAULT, tfp);

	return rc;
}

static void
ulp_ctx_detach(struct bnxt *bp)
{
	struct tf *tfp;

	tfp = (struct tf *)bp->tfp;
	if (tfp->session) {
		tf_close_session(bp->tfp);
		tfp->session = NULL;
	}
}

/* Initialize the state of an ULP session.
 * If the state of an ULP session is not initialized, set it's state to
 * initialized. If the state is already initialized, do nothing.
 */
static bool
ulp_context_initialized(struct bnxt_ulp_session_state *session)
{
	if (!session->bnxt_ulp_init) {
		session->bnxt_ulp_init = true;
		return false;
	} else {
		return true;
	}
}

/* Check if an ULP session is already allocated for a specific PCI
 * domain & bus. If it is already allocated simply return the session
 * pointer, otherwise allocate a new session.
 */
static struct bnxt_ulp_session_state *
ulp_get_session(struct bnxt *bp)
{
	struct bnxt_ulp_session_state *session;
	struct hlist_node *node;

	hlist_for_each_entry_safe(session, node, &bnxt_ulp_session_list, next) {
		if (!memcmp(session->dsn, bp->dsn, sizeof(bp->dsn)))
			return session;
	}
	return NULL;
}

/* Allocate and Initialize an ULP session and set it's state to INITIALIZED.
 * If it's already initialized simply return the already existing session.
 */
static struct bnxt_ulp_session_state *
ulp_session_init(struct bnxt *bp,
		 bool *init)
{
	struct bnxt_ulp_session_state	*session;

	mutex_lock(&bnxt_ulp_global_mutex);
	session = ulp_get_session(bp);
	if (!session) {
		/* Not Found the session  Allocate a new one */
		session = vzalloc(sizeof(*session));
		if (!session) {
			mutex_unlock(&bnxt_ulp_global_mutex);
			return NULL;

		} else {
			/* Add it to the queue */
			memcpy(session->dsn, bp->dsn, sizeof(bp->dsn));
			hlist_add_head(&session->next, &bnxt_ulp_session_list);
		}
	}
	*init = ulp_context_initialized(session);
	mutex_unlock(&bnxt_ulp_global_mutex);
	return session;
}

/* When a device is closed, remove it's associated session from the global
 * session list.
 */
static void
ulp_session_deinit(struct bnxt_ulp_session_state *session)
{
	if (!session)
		return;

	if (!session->cfg_data) {
		mutex_lock(&bnxt_ulp_global_mutex);
		hlist_del(&session->next);
		vfree(session);
		mutex_unlock(&bnxt_ulp_global_mutex);
	}
}

/* Internal api to enable NAT feature.
 * Set set_flag to 1 to set the value or zero to reset the value.
 * returns 0 on success.
 */
static int
bnxt_ulp_global_cfg_update(struct bnxt *bp,
			   enum tf_dir dir,
			   enum tf_global_config_type type,
			   u32 offset,
			   u32 value,
			   u32 set_flag)
{
	struct tf_global_cfg_parms parms = { 0 };
	u32 global_cfg = 0;
	int rc;

	/* Initialize the params */
	parms.dir = dir,
	parms.type = type,
	parms.offset = offset,
	parms.config = (u8 *)&global_cfg,
	parms.config_sz_in_bytes = sizeof(global_cfg);

	rc = tf_get_global_cfg(bp->tfp, &parms);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to get global cfg 0x%x rc:%d\n",
			   type, rc);
		return rc;
	}

	if (set_flag)
		global_cfg |= value;
	else
		global_cfg &= ~value;

	/* SET the register RE_CFA_REG_ACT_TECT */
	rc = tf_set_global_cfg(bp->tfp, &parms);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to set global cfg 0x%x rc:%d\n",
			   type, rc);
		return rc;
	}
	return rc;
}

/* Internal function to delete all the flows belonging to the given port */
static void
bnxt_ulp_flush_port_flows(struct bnxt *bp)
{
	u16 func_id;

	/* it is assumed that port is either TVF or PF */
	if (ulp_port_db_port_func_id_get(bp->ulp_ctx,
					 bp->pf.fw_fid,
					 &func_id)) {
		netdev_dbg(bp->dev, "Invalid argument\n");
		return;
	}
	(void)ulp_flow_db_function_flow_flush(bp->ulp_ctx, func_id);
}

/* When a port is deinit'ed. This function is called
 * and this function clears the ULP context and rest of the
 * infrastructure associated with it.
 */
static void
bnxt_ulp_deinit(struct bnxt *bp,
		struct bnxt_ulp_session_state *session)
{
	struct bnxt_ulp_context *ulp_ctx = bp->ulp_ctx;

	if (!ulp_ctx || !ulp_ctx->cfg_data)
		return;

	/* clean up default flows */
	bnxt_ulp_destroy_df_rules(bp, true);

	/* clean up regular flows */
	ulp_flow_db_flush_flows(ulp_ctx, BNXT_ULP_FDB_TYPE_REGULAR);

	/* cleanup the eem table scope */
	ulp_eem_tbl_scope_deinit(bp, ulp_ctx);

	/* cleanup the flow database */
	ulp_flow_db_deinit(ulp_ctx);

	/* Delete the Mark database */
	ulp_mark_db_deinit(ulp_ctx);

	/* cleanup the ulp mapper */
	ulp_mapper_deinit(ulp_ctx);

	/* cleanup the ulp matcher */
	ulp_matcher_deinit(bp->ulp_ctx);

	/* Delete the Flow Counter Manager */
	ulp_fc_mgr_deinit(ulp_ctx);

	/* Delete the Port database */
	ulp_port_db_deinit(ulp_ctx);

	/* Disable NAT feature */
	(void)bnxt_ulp_global_cfg_update(bp, TF_DIR_RX, TF_TUNNEL_ENCAP,
					 TF_TUNNEL_ENCAP_NAT,
					 BNXT_ULP_NAT_OUTER_MOST_FLAGS, 0);

	(void)bnxt_ulp_global_cfg_update(bp, TF_DIR_TX, TF_TUNNEL_ENCAP,
					 TF_TUNNEL_ENCAP_NAT,
					 BNXT_ULP_NAT_OUTER_MOST_FLAGS, 0);

	/* Delete the ulp context and tf session and free the ulp context */
	ulp_ctx_deinit(bp, session);
	netdev_dbg(bp->dev, "ulp ctx has been deinitialized\n");
}

/* When a port is initialized. This functions is called
 * and this function initializes the ULP context and rest of the
 * infrastructure associated with it.
 */
static int
bnxt_ulp_init(struct bnxt *bp,
	      struct bnxt_ulp_session_state *session)
{
	struct bnxt_ulp_context *ulp_ctx = bp->ulp_ctx;
	int rc;

	/* Allocate and Initialize the ulp context. */
	rc = ulp_ctx_init(bp, session);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to create the ulp context\n");
		goto err;
	}

	mutex_init(&ulp_ctx->cfg_data->flow_db_lock);

	/* Defaults to Internal */
	rc = bnxt_ulp_cntxt_mem_type_set(ulp_ctx, BNXT_ULP_FLOW_MEM_TYPE_INT);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to write mem_type in ulp ctxt\n");
		goto err;
	}

	/* create the port database */
	rc = ulp_port_db_init(ulp_ctx, bp->port_count);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to create the port database\n");
		goto err;
	}

	/* Create the Mark database. */
	rc = ulp_mark_db_init(ulp_ctx);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to create the mark database\n");
		goto err;
	}

	/* Create the flow database. */
	rc = ulp_flow_db_init(ulp_ctx);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to create the flow database\n");
		goto err;
	}

	/* Create the eem table scope. */
	rc = ulp_eem_tbl_scope_init(bp);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to create the eem scope table\n");
		goto err;
	}

	rc = ulp_matcher_init(ulp_ctx);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to initialize ulp matcher\n");
		goto err;
	}

	rc = ulp_mapper_init(ulp_ctx);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to initialize ulp mapper\n");
		goto err;
	}

	rc = ulp_fc_mgr_init(ulp_ctx);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to initialize ulp flow counter mgr\n");
		goto err;
	}

	/* Enable NAT feature. Set the global configuration register
	 * Tunnel encap to enable NAT with the reuse of existing inner
	 * L2 header smac and dmac
	 */
	rc = bnxt_ulp_global_cfg_update(bp, TF_DIR_RX, TF_TUNNEL_ENCAP,
					TF_TUNNEL_ENCAP_NAT,
					BNXT_ULP_NAT_OUTER_MOST_FLAGS, 1);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to set rx global configuration\n");
		goto err;
	}

	rc = bnxt_ulp_global_cfg_update(bp, TF_DIR_TX, TF_TUNNEL_ENCAP,
					TF_TUNNEL_ENCAP_NAT,
					BNXT_ULP_NAT_OUTER_MOST_FLAGS, 1);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to set tx global configuration\n");
		goto err;
	}

	netdev_dbg(bp->dev, "ulp ctx has been initialized\n");
	return rc;

err:
	bnxt_ulp_deinit(bp, session);
	return rc;
}

/* When a port is de-initialized. This functions clears up
 * the port specific details.
 */
void
bnxt_ulp_port_deinit(struct bnxt *bp)
{
	struct bnxt_ulp_context *ulp_ctx = bp->ulp_ctx;
	struct bnxt_ulp_session_state *session;

	if (!BNXT_PF(bp) && !BNXT_VF_IS_TRUSTED(bp)) {
		netdev_dbg(bp->dev,
			   "Skip ULP deinit port:%d, not a TVF or PF\n",
			   bp->pf.fw_fid);
		return;
	}

	if (!BNXT_TRUFLOW_EN(bp)) {
		netdev_dbg(bp->dev,
			   "Skip ULP deinit for port:%d, truflow is not enabled\n",
			   bp->pf.fw_fid);
		return;
	}

	if (!ulp_ctx) {
		netdev_dbg(bp->dev, "ulp ctx already de-allocated\n");
		return;
	}

	netdev_dbg(bp->dev, "BNXT Port:%d ULP port deinit\n",
		   bp->pf.fw_fid);

	/* Free the ulp context in the context entry list */
	bnxt_ulp_cntxt_list_del(ulp_ctx);

	/* Get the session details  */
	mutex_lock(&bnxt_ulp_global_mutex);
	session = ulp_get_session(bp);
	mutex_unlock(&bnxt_ulp_global_mutex);
	/* session not found then just exit */
	if (!session) {
		/* Free the ulp context */
		vfree(bp->ulp_ctx);
		vfree(bp->tfp);

		bp->ulp_ctx = NULL;
		return;
	}

	/* Check the reference count to deinit or deattach*/
	if (ulp_ctx->cfg_data && ulp_ctx->cfg_data->ref_cnt) {
		ulp_ctx->cfg_data->ref_cnt--;
		if (ulp_ctx->cfg_data->ref_cnt) {
			/* free the port details */
			/* Free the default flow rule associated to this port */
			bnxt_ulp_destroy_df_rules(bp, false);

			/* free flows associated with this port */
			bnxt_ulp_flush_port_flows(bp);

			/* close the session associated with this port */
			ulp_ctx_detach(bp);

			/* always detach/close shared after the session. */
			ulp_ctx_shared_session_detach(bp);
		} else {
			/* Perform ulp ctx deinit */
			bnxt_ulp_deinit(bp, session);
		}
	}

	/* clean up the session */
	ulp_session_deinit(session);

	/* Free the ulp context */
	vfree(bp->ulp_ctx);
	vfree(bp->tfp);
	bp->tfp = NULL;
	bp->ulp_ctx = NULL;
}

/* Entry point for Truflow feature initialization.
 */
int
bnxt_ulp_port_init(struct bnxt *bp)
{
	struct bnxt_ulp_session_state *session;
	struct bnxt_ulp_context *ulp_ctx;
	bool initialized;
	int rc = 0;
	u32 ulp_flags;

	if (!(bp->flags & BNXT_FLAG_DSN_VALID)) {
		netdev_dbg(bp->dev, "Invalid DSN, don't create ULP session\n");
		return -EINVAL;
	}

	if (bp->ulp_ctx) {
		netdev_dbg(bp->dev, "ulp ctx already allocated\n");
		return rc;
	}

	rc = bnxt_hwrm_port_mac_qcfg(bp);
	if (rc)
		return rc;

	bp->ulp_ctx = vzalloc(sizeof(struct bnxt_ulp_context));
	if (!bp->ulp_ctx)
		return -ENOMEM;

	ulp_ctx = bp->ulp_ctx;
	ulp_ctx->bp = bp;

	bp->tfp = vzalloc(sizeof(struct tf) * BNXT_SESSION_TYPE_LAST);
	if (!bp->tfp) {
		vfree(bp->ulp_ctx);
		return -ENOMEM;
	}

	/* Multiple uplink ports can be associated with a single vswitch.
	 * Make sure only the port that is started first will initialize
	 * the TF session.
	 */
	session = ulp_session_init(bp, &initialized);
	if (!session) {
		netdev_dbg(bp->dev, "Failed to initialize the tf session\n");
		rc = -EIO;
		goto error;
	}

	if (initialized) {
		/* If ULP is already initialized for a specific domain then
		 * simply assign the ulp context to this netdev as well.
		 */
		rc = ulp_ctx_attach(bp, session);
		if (rc) {
			netdev_dbg(bp->dev, "Failed to attach the ulp context\n");
			goto error;
		}

		/*
		 * Attach to the shared session, must be called after the
		 * ulp_ctx_attach in order to ensure that ulp data is available
		 * for attaching.
		 */
		rc = ulp_ctx_shared_session_attach(bp, session);
		if (rc) {
			netdev_dbg(bp->dev,
				   "Failed attach to shared session: %d\n",
				   rc);
			goto error;
		}
	} else {
		rc = bnxt_ulp_init(bp, session);
		if (rc) {
			netdev_dbg(bp->dev, "Failed to initialize the ulp init\n");
			goto error;
		}
	}

	/* Update bnxt driver flags */
	rc = ulp_dparms_dev_port_intf_update(bp, ulp_ctx);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to update driver flags\n");
		goto error;
	}

	/* update the port database for the given interface */
	rc = ulp_port_db_dev_port_intf_update(ulp_ctx, bp, NULL);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to update port database\n");
		goto error;
	}
	/* create the default rules */
	rc = bnxt_ulp_create_df_rules(bp);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to create default flow\n");
		goto error;
	}

	netdev_dbg(bp->dev, "BNXT Port:%d ULP port init, accum_stats:%d\n",
		   bp->pf.fw_fid, ulp_ctx->cfg_data->accum_stats);

	/* set the unicast mode */
	if (bnxt_ulp_cntxt_ptr2_ulp_flags_get(ulp_ctx, &ulp_flags)) {
		netdev_dbg(bp->dev, "Error in getting ULP context flags\n");
		goto error;
	}

	return rc;

error:
	bnxt_ulp_port_deinit(bp);
	return rc;
}

/* Below are the access functions to access internal data of ulp context. */
/* Function to set the Mark DB into the context */
int
bnxt_ulp_cntxt_ptr2_mark_db_set(struct bnxt_ulp_context *ulp_ctx,
				struct bnxt_ulp_mark_tbl *mark_tbl)
{
	if (!ulp_ctx || !ulp_ctx->cfg_data)
		return -EINVAL;

	ulp_ctx->cfg_data->mark_tbl = mark_tbl;

	return 0;
}

/* Function to retrieve the Mark DB from the context. */
struct bnxt_ulp_mark_tbl *
bnxt_ulp_cntxt_ptr2_mark_db_get(struct bnxt_ulp_context *ulp_ctx)
{
	if (!ulp_ctx || !ulp_ctx->cfg_data)
		return NULL;

	return ulp_ctx->cfg_data->mark_tbl;
}

int
bnxt_ulp_cntxt_app_id_set(struct bnxt_ulp_context *ulp_ctx, u8 app_id)
{
	if (!ulp_ctx)
		return -EINVAL;
	ulp_ctx->cfg_data->app_id = app_id;
	netdev_info(ulp_ctx->bp->dev, "Truflow APP ID is %d\n", app_id);
	return 0;
}

/* Function to get the shared tfp session details from the ulp context. */
struct tf *
bnxt_ulp_cntxt_shared_tfp_get(struct bnxt_ulp_context *ulp)
{
	if (!ulp)
		return NULL;

	return ulp->g_shared_tfp;
}

int
bnxt_ulp_cntxt_app_id_get(struct bnxt_ulp_context *ulp_ctx, u8 *app_id)
{
	/* Default APP id is zero */
	if (!ulp_ctx || !app_id)
		return -EINVAL;
	*app_id = ulp_ctx->cfg_data->app_id;
	return 0;
}

int
bnxt_ulp_cntxt_mem_type_set(struct bnxt_ulp_context *ulp_ctx,
			    enum bnxt_ulp_flow_mem_type mem_type)
{
	if (ulp_ctx && ulp_ctx->cfg_data) {
		ulp_ctx->cfg_data->mem_type = mem_type;
		return 0;
	}

	return -EINVAL;
}

int
bnxt_ulp_cntxt_mem_type_get(struct bnxt_ulp_context *ulp_ctx,
			    enum bnxt_ulp_flow_mem_type *mem_type)
{
	if (ulp_ctx && ulp_ctx->cfg_data) {
		*mem_type = ulp_ctx->cfg_data->mem_type;
		return 0;
	}
	*mem_type = BNXT_ULP_FLOW_MEM_TYPE_LAST;
	return -EINVAL;
}

/* Function to get the table scope id of the EEM table. */
int
bnxt_ulp_cntxt_tbl_scope_id_get(struct bnxt_ulp_context *ulp_ctx,
				u32 *tbl_scope_id)
{
	if (ulp_ctx && ulp_ctx->cfg_data) {
		*tbl_scope_id = ulp_ctx->cfg_data->tbl_scope_id;
		return 0;
	}

	return -EINVAL;
}

/* Function to set the table scope id of the EEM table. */
int
bnxt_ulp_cntxt_tbl_scope_id_set(struct bnxt_ulp_context *ulp_ctx,
				u32 tbl_scope_id)
{
	if (ulp_ctx && ulp_ctx->cfg_data) {
		ulp_ctx->cfg_data->tbl_scope_id = tbl_scope_id;
		return 0;
	}

	return -EINVAL;
}

/* Function to set the tfp session details from the ulp context. */
int bnxt_ulp_cntxt_tfp_set(struct bnxt_ulp_context *ulp,
			   enum bnxt_ulp_session_type s_type, struct tf *tfp)
{
	u32 idx = 0;

	if (!ulp)
		return -EINVAL;

	if (s_type & BNXT_ULP_SESSION_TYPE_SHARED)
		idx = 1;

	ulp->g_tfp[idx] = tfp;
	return 0;
}

/* Function to get the tfp session details from the ulp context. */
struct tf *bnxt_ulp_cntxt_tfp_get(struct bnxt_ulp_context *ulp,
				  enum bnxt_ulp_session_type s_type)
{
	u32 idx = 0;

	if (!ulp)
		return NULL;

	if (s_type & BNXT_ULP_SESSION_TYPE_SHARED)
		idx = 1;

	return ulp->g_tfp[idx];
}

/**
 * Get the device table entry based on the device id.
 *
 * @dev_id: The device id of the hardware
 *
 * Returns the pointer to the device parameters.
 */
struct bnxt_ulp_device_params *
bnxt_ulp_device_params_get(u32 dev_id)
{
	if (dev_id < BNXT_ULP_MAX_NUM_DEVICES)
		return &ulp_device_params[dev_id];
	return NULL;
}

/* Function to set the flow database to the ulp context. */
int
bnxt_ulp_cntxt_ptr2_flow_db_set(struct bnxt_ulp_context	*ulp_ctx,
				struct bnxt_ulp_flow_db	*flow_db)
{
	if (!ulp_ctx || !ulp_ctx->cfg_data)
		return -EINVAL;

	ulp_ctx->cfg_data->flow_db = flow_db;
	return 0;
}

/* Function to get the flow database from the ulp context. */
struct bnxt_ulp_flow_db	*
bnxt_ulp_cntxt_ptr2_flow_db_get(struct bnxt_ulp_context	*ulp_ctx)
{
	if (!ulp_ctx || !ulp_ctx->cfg_data)
		return NULL;

	return ulp_ctx->cfg_data->flow_db;
}

/* Function to get the ulp context from eth device. */
struct bnxt_ulp_context	*
bnxt_ulp_bp_ptr2_cntxt_get(struct bnxt *bp)
{
	if (!bp)
		return NULL;

	return bp->ulp_ctx;
}

int
bnxt_ulp_cntxt_ptr2_mapper_data_set(struct bnxt_ulp_context *ulp_ctx,
				    void *mapper_data)
{
	if (!ulp_ctx || !ulp_ctx->cfg_data)
		return -EINVAL;

	ulp_ctx->cfg_data->mapper_data = mapper_data;
	return 0;
}

void *
bnxt_ulp_cntxt_ptr2_mapper_data_get(struct bnxt_ulp_context *ulp_ctx)
{
	if (!ulp_ctx || !ulp_ctx->cfg_data)
		return NULL;

	return ulp_ctx->cfg_data->mapper_data;
}

int
bnxt_ulp_cntxt_ptr2_matcher_data_set(struct bnxt_ulp_context *ulp_ctx,
				     void *matcher_data)
{
	if (!ulp_ctx || !ulp_ctx->cfg_data)
		return -EINVAL;

	ulp_ctx->cfg_data->matcher_data = matcher_data;
	return 0;
}

void *
bnxt_ulp_cntxt_ptr2_matcher_data_get(struct bnxt_ulp_context *ulp_ctx)
{
	if (!ulp_ctx || !ulp_ctx->cfg_data)
		return NULL;

	return ulp_ctx->cfg_data->matcher_data;
}

/* Function to set the port database to the ulp context. */
int
bnxt_ulp_cntxt_ptr2_port_db_set(struct bnxt_ulp_context	*ulp_ctx,
				struct bnxt_ulp_port_db	*port_db)
{
	if (!ulp_ctx || !ulp_ctx->cfg_data)
		return -EINVAL;

	ulp_ctx->cfg_data->port_db = port_db;
	return 0;
}

/* Function to get the port database from the ulp context. */
struct bnxt_ulp_port_db *
bnxt_ulp_cntxt_ptr2_port_db_get(struct bnxt_ulp_context	*ulp_ctx)
{
	if (!ulp_ctx || !ulp_ctx->cfg_data)
		return NULL;

	return ulp_ctx->cfg_data->port_db;
}

/* Function to set the flow counter info into the context */
int
bnxt_ulp_cntxt_ptr2_fc_info_set(struct bnxt_ulp_context *ulp_ctx,
				struct bnxt_ulp_fc_info *ulp_fc_info)
{
	if (!ulp_ctx || !ulp_ctx->cfg_data)
		return -EINVAL;

	ulp_ctx->cfg_data->fc_info = ulp_fc_info;

	return 0;
}

/* Function to retrieve the flow counter info from the context. */
struct bnxt_ulp_fc_info *
bnxt_ulp_cntxt_ptr2_fc_info_get(struct bnxt_ulp_context *ulp_ctx)
{
	if (!ulp_ctx || !ulp_ctx->cfg_data)
		return NULL;

	return ulp_ctx->cfg_data->fc_info;
}

/* Function to get the ulp flags from the ulp context. */
int
bnxt_ulp_cntxt_ptr2_ulp_flags_get(struct bnxt_ulp_context *ulp_ctx,
				  u32 *flags)
{
	if (!ulp_ctx || !ulp_ctx->cfg_data)
		return -1;

	*flags =  ulp_ctx->cfg_data->ulp_flags;
	return 0;
}

/* Function to get the ulp vfr info from the ulp context. */
struct bnxt_ulp_vfr_rule_info*
bnxt_ulp_cntxt_ptr2_ulp_vfr_info_get(struct bnxt_ulp_context *ulp_ctx,
				     u32 port_id)
{
	if (!ulp_ctx || !ulp_ctx->cfg_data || port_id >= BNXT_TC_MAX_PORTS)
		return NULL;

	return &ulp_ctx->cfg_data->vfr_rule_info[port_id];
}

static int
bnxt_ulp_cntxt_list_add(struct bnxt_ulp_context *ulp_ctx)
{
	struct ulp_context_list_entry	*entry;

	entry = vzalloc(sizeof(*entry));
	if (!entry)
		return -ENOMEM;

	mutex_lock(&bnxt_ulp_ctxt_lock);
	entry->ulp_ctx = ulp_ctx;
	hlist_add_head(&entry->next, &ulp_cntx_list);
	mutex_unlock(&bnxt_ulp_ctxt_lock);

	return 0;
}

static void
bnxt_ulp_cntxt_list_del(struct bnxt_ulp_context *ulp_ctx)
{
	struct ulp_context_list_entry	*entry;
	struct hlist_node *node;

	mutex_lock(&bnxt_ulp_ctxt_lock);
	hlist_for_each_entry_safe(entry, node, &ulp_cntx_list, next) {
		if (entry && entry->ulp_ctx == ulp_ctx) {
			hlist_del(&entry->next);
			vfree(entry);
			break;
		}
	}
	mutex_unlock(&bnxt_ulp_ctxt_lock);
}

struct bnxt_ulp_context *
bnxt_ulp_cntxt_entry_lookup(void *cfg_data)
{
	struct ulp_context_list_entry	*entry;
	struct hlist_node *node;

	/* take a lock and get the first ulp context available */
	hlist_for_each_entry_safe(entry, node, &ulp_cntx_list, next) {
		if (entry && entry->ulp_ctx &&
		    entry->ulp_ctx->cfg_data == cfg_data)
			return entry->ulp_ctx;
	}

	return NULL;
}

void
bnxt_ulp_cntxt_lock_acquire(void)
{
	mutex_lock(&bnxt_ulp_ctxt_lock);
}

void
bnxt_ulp_cntxt_lock_release(void)
{
	mutex_unlock(&bnxt_ulp_ctxt_lock);
}

int
bnxt_ulp_get_mark_from_cfacode(struct bnxt *bp, struct rx_cmp_ext *rxcmp1,
			       struct bnxt_tpa_info *tpa_info, u32 *mark_id)
{
	bool gfid = false;
	u32 vfr_flag;
	u32 cfa_code;
	u32 meta_fmt;
	u32 flags2;
	u32 meta;
	int rc;

	if (rxcmp1) {
		cfa_code = RX_CMP_CFA_CODE(rxcmp1);
		flags2 = le32_to_cpu(rxcmp1->rx_cmp_flags2);
		meta = le32_to_cpu(rxcmp1->rx_cmp_meta_data);
	} else {
		cfa_code = le16_to_cpu(tpa_info->cfa_code);
		flags2 = le32_to_cpu(tpa_info->flags2);
		meta = le32_to_cpu(tpa_info->metadata);
	}

	/* The flags field holds extra bits of info from [6:4]
	 * which indicate if the flow is in TCAM or EM or EEM
	 */
	meta_fmt = (flags2 & BNXT_CFA_META_FMT_MASK) >>
		BNXT_CFA_META_FMT_SHFT;

	switch (meta_fmt) {
	case 0:
		if (BNXT_GFID_ENABLED(bp))
			/* Not an LFID or GFID, a flush cmd. */
			goto skip_mark;
		break;
	case 4:
		fallthrough;
	case 5:
		/* EM/TCAM case
		 * Assume that EM doesn't support Mark due to GFID
		 * collisions with EEM.  Simply return without setting the mark
		 * in the mbuf.
		 */
		/* If it is not EM then it is a TCAM entry, so it is an LFID.
		 * The TCAM IDX and Mode can also be determined
		 * by decoding the meta_data. We are not
		 * using these for now.
		 */
		if (BNXT_CFA_META_EM_TEST(meta)) {
			/*This is EM hit {EM(1), GFID[27:16], 19'd0 or vtag } */
			gfid = true;
			meta >>= BNXT_RX_META_CFA_CODE_SHIFT;
			cfa_code |= meta << BNXT_CFA_CODE_META_SHIFT;
		}
		break;
	case 6:
		fallthrough;
	case 7:
		/* EEM Case, only using gfid in EEM for now. */
		gfid = true;

		/* For EEM flows, The first part of cfa_code is 16 bits.
		 * The second part is embedded in the
		 * metadata field from bit 19 onwards. The driver needs to
		 * ignore the first 19 bits of metadata and use the next 12
		 * bits as higher 12 bits of cfa_code.
		 */
		meta >>= BNXT_RX_META_CFA_CODE_SHIFT;
		cfa_code |= meta << BNXT_CFA_CODE_META_SHIFT;
		break;
	default:
		/* For other values, the cfa_code is assumed to be an LFID. */
		break;
	}

	rc = ulp_mark_db_mark_get(bp->ulp_ctx, gfid,
				  cfa_code, &vfr_flag, mark_id);
	if (!rc) {
		/* mark_id is the fw_fid of the endpoint vf's and
		 * it is used to identify the VFR.
		 */
		if (vfr_flag)
			return 0;
	}

skip_mark:
	return -EINVAL;
}

int bnxt_ulp_alloc_vf_rep(struct bnxt *bp, void *vfr)
{
	int rc;

	rc = ulp_port_db_dev_port_intf_update(bp->ulp_ctx, bp, vfr);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to update port database\n");
		return -EINVAL;
	}

	rc = bnxt_hwrm_cfa_pair_exists(bp, vfr);
	if (!rc)
		bnxt_hwrm_cfa_pair_free(bp, vfr);

	rc = bnxt_ulp_create_vfr_default_rules(vfr);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to create VFR default rules\n");
		return rc;
	}

	rc = bnxt_hwrm_cfa_pair_alloc(bp, vfr);
	if (rc) {
		netdev_dbg(bp->dev, "CFA_PAIR_ALLOC hwrm command failed\n");
		return rc;
	}

	return 0;
}

void bnxt_ulp_free_vf_rep(struct bnxt *bp, void *vfr)
{
	int rc;

	rc = bnxt_ulp_delete_vfr_default_rules(vfr);
	if (rc)
		netdev_dbg(bp->dev, "Failed to delete VFR default rules\n");

	bnxt_hwrm_cfa_pair_free(bp, vfr);
}

/* Function to set the number of slices for WC into the context */
int
bnxt_ulp_cntxt_ptr2_wc_num_slices_set(struct bnxt_ulp_context *ulp_ctx,
				      u8 wc_num_slices)
{
	if (!ulp_ctx || !ulp_ctx->cfg_data)
		return -EINVAL;

	ulp_ctx->cfg_data->wc_num_slices = wc_num_slices;

	return 0;
}

/* Function to retrieve the number of slices for WC from the context. */
unsigned int
bnxt_ulp_cntxt_ptr2_wc_num_slices_get(struct bnxt_ulp_context *ulp_ctx)
{
	if (!ulp_ctx || !ulp_ctx->cfg_data)
		return 0;

	return (unsigned int)ulp_ctx->cfg_data->wc_num_slices;
}

/* Function to check if allowing multicast and broadcast flow offload. */
bool
bnxt_ulp_validate_bcast_mcast(struct bnxt *bp)
{
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

	/* app_id=0 supports mc/bc flow offload */
	if (app_id != 0)
		return false;

	return true;
}

#endif /* CONFIG_BNXT_FLOWER_OFFLOAD */
