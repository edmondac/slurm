/*****************************************************************************\
 *  fed_mgr.c - functions for federations
 *****************************************************************************
 *  Copyright (C) 2016 SchedMD LLC.
 *  Written by Brian Christiansen <brian@schedmd.com>
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include "config.h"

#include <pthread.h>
#include <signal.h>

#if HAVE_SYS_PRCTL_H
#  include <sys/prctl.h>
#endif

#include "src/common/list.h"
#include "src/common/macros.h"
#include "src/common/parse_time.h"
#include "src/slurmctld/proc_req.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/slurmdbd_defs.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"
#include "src/slurmctld/fed_mgr.h"
#include "src/slurmctld/job_scheduler.h"
#include "src/slurmctld/locks.h"
#include "src/slurmctld/slurmctld.h"
#include "src/slurmctld/state_save.h"
#include "src/slurmdbd/read_config.h"

#define FED_MGR_STATE_FILE       "fed_mgr_state"
#define FED_MGR_CLUSTER_ID_BEGIN 26

#define FED_SIBLING_BIT(x) ((uint64_t)1 << (x - 1))

slurmdb_federation_rec_t *fed_mgr_fed_rec     = NULL;
slurmdb_cluster_rec_t    *fed_mgr_cluster_rec = NULL;

static pthread_cond_t  agent_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t agent_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       agent_thread_id = (pthread_t) 0;
static int             agent_queue_size = 0;

static pthread_cond_t  job_watch_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t job_watch_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       job_watch_thread_id = (pthread_t)0;
static bool            stop_job_watch_thread = false;

static pthread_t ping_thread  = 0;
static bool      stop_pinging = false, inited = false;
static pthread_mutex_t open_send_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t init_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t update_mutex = PTHREAD_MUTEX_INITIALIZER;

static List fed_job_list        = NULL;
static List fed_job_update_list = NULL;
static pthread_t       fed_job_update_thread_id = (pthread_t) 0;
static pthread_mutex_t fed_job_list_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  job_update_cond    = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t job_update_mutex   = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
	Buf        buffer;
	uint32_t   job_id;
	time_t     last_try;
	int        last_defer;
	uint16_t   msg_type;
} agent_queue_t;

enum fed_job_update_type {
	FED_JOB_NONE = 0,
	FED_JOB_CANCEL,
	FED_JOB_COMPLETE,
	FED_JOB_REMOVE_ACTIVE_SIB_BIT,
	FED_JOB_REQUEUE,
	FED_JOB_START,
	FED_JOB_SUBMIT_BATCH,
	FED_JOB_SUBMIT_INT,
	FED_JOB_SUBMIT_RESP,
	FED_JOB_SYNC,
	FED_JOB_UPDATE,
	FED_JOB_UPDATE_RESPONSE,
	FED_SEND_JOB_SYNC,
};

typedef struct {
	uint32_t        cluster_lock;
	uint32_t        job_id;
	job_info_msg_t *job_info_msg;
	job_step_kill_msg_t *kill_msg;
	bool            requeue;
	uint32_t        return_code;
	uint64_t        siblings_active;
	uint64_t        siblings_viable;
	char           *siblings_str;
	time_t          start_time;
	uint32_t        state;
	char           *submit_cluster;
	job_desc_msg_t *submit_desc;
	uint16_t        submit_proto_ver;
	uint32_t        type;
	uid_t           uid;
} fed_job_update_info_t;

typedef struct {
	uint32_t        cluster_lock;
	uint32_t        job_id;
	uint64_t        siblings_active;
	uint64_t        siblings_viable;
	uint32_t        updating_sibs[MAX_FED_CLUSTERS + 1];
	time_t          updating_time[MAX_FED_CLUSTERS + 1];
} fed_job_info_t;

/* Local Structs */
typedef struct {
	job_info_msg_t *job_info_msg;
	uint32_t        sibling_id;
	char           *sibling_name;
	time_t          sync_time;
} reconcile_sib_t;

/* Local Prototypes */
static int _is_fed_job(struct job_record *job_ptr, uint32_t *origin_id);
static uint64_t _get_all_sibling_bits();
static int _validate_cluster_features(char *spec_features,
				      uint64_t *cluster_bitmap);
static int _validate_cluster_names(char *clusters, uint64_t *cluster_bitmap);
static void _leave_federation(void);
static int _q_send_job_sync(char *sib_name);
static slurmdb_federation_rec_t *_state_load(char *state_save_location,
					     bool job_list_only);
static int _sync_jobs(const char *sib_name, job_info_msg_t *job_info_msg,
		      time_t sync_time);

static char *_job_update_type_str(enum fed_job_update_type type)
{
	switch (type) {
	case FED_JOB_COMPLETE:
		return "FED_JOB_COMPLETE";
	case FED_JOB_CANCEL:
		return "FED_JOB_CANCEL";
	case FED_JOB_REMOVE_ACTIVE_SIB_BIT:
		return "FED_JOB_REMOVE_ACTIVE_SIB_BIT";
	case FED_JOB_REQUEUE:
		return "FED_JOB_REQUEUE";
	case FED_JOB_START:
		return "FED_JOB_START";
	case FED_JOB_SUBMIT_BATCH:
		return "FED_JOB_SUBMIT_BATCH";
	case FED_JOB_SUBMIT_INT:
		return "FED_JOB_SUBMIT_INT";
	case FED_JOB_SUBMIT_RESP:
		return "FED_JOB_SUBMIT_RESP";
	case FED_JOB_SYNC:
		return "FED_JOB_SYNC";
	case FED_JOB_UPDATE:
		return "FED_JOB_UPDATE";
	case FED_JOB_UPDATE_RESPONSE:
		return "FED_JOB_UPDATE_RESPONSE";
	case FED_SEND_JOB_SYNC:
		return "FED_SEND_JOB_SYNC";
	default:
		return "?";
	}
}

static void _append_job_update(fed_job_update_info_t *job_update_info)
{
	list_append(fed_job_update_list, job_update_info);

	slurm_mutex_lock(&job_update_mutex);
	slurm_cond_broadcast(&job_update_cond);
	slurm_mutex_unlock(&job_update_mutex);
}

/* Return true if communication failure should be logged. Only log failures
 * every 10 minutes to avoid filling logs */
static bool _comm_fail_log(slurmdb_cluster_rec_t *cluster)
{
	time_t now = time(NULL);
	time_t old = now - 600;	/* Log failures once every 10 mins */

	if (cluster->comm_fail_time < old) {
		cluster->comm_fail_time = now;
		return true;
	}
	return false;
}

static int _close_controller_conn(slurmdb_cluster_rec_t *cluster)
{
	int rc = SLURM_SUCCESS;
//	slurm_persist_conn_t *persist_conn = NULL;

	xassert(cluster);
	slurm_mutex_lock(&cluster->lock);
	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
		info("closing sibling conn to %s", cluster->name);

	/* The recv free of this is handled directly in the persist_conn code,
	 * don't free it here */
//	slurm_persist_conn_destroy(cluster->fed.recv);
	cluster->fed.recv = NULL;
	slurm_persist_conn_destroy(cluster->fed.send);
	cluster->fed.send = NULL;

	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
		info("closed sibling conn to %s", cluster->name);
	slurm_mutex_unlock(&cluster->lock);

	return rc;
}

/* Get list of jobs that originated from this cluster and the remove sibling.
 * Originating here: so that the remote can determine if the tracker job is gone
 * Originating sib: so that the remote verify jobs are where they're supposed to
 * be. If the sibling doesn't find a job, the sibling can resubmit the job or
 * take other actions.
 *
 * Only get jobs that were submitted prior to sync_time
 */
static List _get_sync_jobid_list(uint32_t sib_id, time_t sync_time)
{
	List jobids = NULL;
	ListIterator job_itr;
	struct job_record *job_ptr;

	jobids = list_create(slurm_destroy_uint32_ptr);

	job_itr = list_iterator_create(job_list);
	while ((job_ptr = (struct job_record *) list_next(job_itr))) {
		uint32_t cluster_id = fed_mgr_get_cluster_id(job_ptr->job_id);
		if (!(IS_JOB_COMPLETED(job_ptr) ||
		      IS_JOB_COMPLETING(job_ptr)) &&
		    (job_ptr->details &&
		     (job_ptr->details->submit_time < sync_time)) &&
		    ((cluster_id == sib_id) ||
		     (cluster_id == fed_mgr_cluster_rec->fed.id))) {

		    uint32_t *tmp = xmalloc(sizeof(uint32_t));
		    *tmp = job_ptr->job_id;
		    list_append(jobids, tmp);
		}
	}
	list_iterator_destroy(job_itr);

	return jobids;
}

static int _open_controller_conn(slurmdb_cluster_rec_t *cluster, bool locked)
{
	int rc;
	slurm_persist_conn_t *persist_conn = NULL;
	static int timeout = -1;

	if (timeout < 0)
		timeout = slurm_get_msg_timeout() * 1000;

	if (cluster == fed_mgr_cluster_rec) {
		info("%s: hey! how did we get here with ourselves?", __func__);
		return SLURM_ERROR;
	}

	if (!locked)
		slurm_mutex_lock(&cluster->lock);

	if (!cluster->control_host || !cluster->control_host[0] ||
	    !cluster->control_port) {
		if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
			info("%s: Sibling cluster %s doesn't appear to be up yet, skipping",
			     __func__, cluster->name);
		if (!locked)
			slurm_mutex_unlock(&cluster->lock);
		return SLURM_ERROR;
	}

	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
		info("opening sibling conn to %s", cluster->name);

	if (!cluster->fed.send) {
		persist_conn = xmalloc(sizeof(slurm_persist_conn_t));

		cluster->fed.send = persist_conn;

		/* Since this connection is coming from us, make it so ;) */
		persist_conn->cluster_name = xstrdup(slurmctld_conf.cluster_name);
		persist_conn->my_port = slurmctld_conf.slurmctld_port;
		persist_conn->rem_host = xstrdup(cluster->control_host);
		persist_conn->rem_port = cluster->control_port;
		persist_conn->version  = cluster->rpc_version;
		persist_conn->shutdown = &slurmctld_config.shutdown_time;
		persist_conn->timeout = timeout; /* don't put this as 0 it
						  * could cause deadlock */
	} else {
		persist_conn = cluster->fed.send;

		/* Perhaps a backup came up, so don't assume it was the same
		 * host or port we had before.
		 */
		xfree(persist_conn->rem_host);
		persist_conn->rem_host = xstrdup(cluster->control_host);
		persist_conn->rem_port = cluster->control_port;
	}

	rc = slurm_persist_conn_open(persist_conn);
	if (rc != SLURM_SUCCESS) {
		if (_comm_fail_log(cluster)) {
			error("fed_mgr: Unable to open connection to cluster %s using host %s(%u)",
			      cluster->name,
			      persist_conn->rem_host, persist_conn->rem_port);
		}
	} else if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR) {
		info("opened sibling conn to %s:%d",
		     cluster->name, persist_conn->fd);
	}

	if (!locked)
		slurm_mutex_unlock(&cluster->lock);

	return rc;
}

/* The cluster->lock should be locked before this is called */
static int _check_send(slurmdb_cluster_rec_t *cluster)
{
	slurm_persist_conn_t *send = cluster->fed.send;

	if (!send || send->fd == -1) {
		return _open_controller_conn(cluster, true);
	}

	return SLURM_SUCCESS;
}

/* fed_mgr read lock needs to be set before coming in here,
 * not the write lock */
static void _open_persist_sends(void)
{
	ListIterator itr;
	slurmdb_cluster_rec_t *cluster = NULL;
	slurm_persist_conn_t *send = NULL;

	if (!fed_mgr_fed_rec || ! fed_mgr_fed_rec->cluster_list)
		return;

	/* This open_send_mutex will make this like a write lock since at the
	 * same time we are sending out these open requests the other slurmctlds
	 * will be replying and needing to get to the structures.  If we just
	 * used the fed_mgr write lock it would cause deadlock.
	 */
	slurm_mutex_lock(&open_send_mutex);
	itr = list_iterator_create(fed_mgr_fed_rec->cluster_list);
	while ((cluster = list_next(itr))) {
		if (cluster == fed_mgr_cluster_rec)
			continue;

		send = cluster->fed.send;
		if (!send || send->fd == -1)
			_open_controller_conn(cluster, false);
	}
	list_iterator_destroy(itr);
	slurm_mutex_unlock(&open_send_mutex);
}

static int _send_recv_msg(slurmdb_cluster_rec_t *cluster, slurm_msg_t *req,
			  slurm_msg_t *resp, bool locked)
{
	int rc;

	xassert(cluster);
	xassert(req);
	xassert(resp);

	slurm_msg_t_init(resp);

	if (!locked)
		slurm_mutex_lock(&cluster->lock);

	rc = _check_send(cluster);
	if ((rc == SLURM_SUCCESS) && cluster->fed.send) {
		resp->conn = req->conn = cluster->fed.send;
		rc = slurm_send_recv_msg(req->conn->fd, req, resp, 0);
	}
	if (!locked)
		slurm_mutex_unlock(&cluster->lock);

	return rc;
}

/* Free Buf record from a list */
static void _ctld_free_list_msg(void *x)
{
	agent_queue_t *agent_queue_ptr = (agent_queue_t *) x;
	if (agent_queue_ptr) {
		FREE_NULL_BUFFER(agent_queue_ptr->buffer);
		xfree(agent_queue_ptr);
	}
}

static int _queue_rpc(slurmdb_cluster_rec_t *cluster, slurm_msg_t *req,
		      uint32_t job_id, bool locked)
{
	agent_queue_t *agent_rec;
	Buf buf;

	if (!cluster->send_rpc)
		cluster->send_rpc = list_create(_ctld_free_list_msg);

	buf = init_buf(1024);
	pack16(req->msg_type, buf);
	if (pack_msg(req, buf) != SLURM_SUCCESS) {
		error("%s: failed to pack msg_type:%u",
		      __func__, req->msg_type);
		FREE_NULL_BUFFER(buf);
		return SLURM_ERROR;
	}

	/* Queue the RPC and notify the agent of new work */
	agent_rec = xmalloc(sizeof(agent_queue_t));
	agent_rec->buffer = buf;
	agent_rec->job_id = job_id;
	agent_rec->msg_type = req->msg_type;
	list_append(cluster->send_rpc, agent_rec);
	slurm_mutex_lock(&agent_mutex);
	agent_queue_size++;
	slurm_cond_broadcast(&agent_cond);
	slurm_mutex_unlock(&agent_mutex);

	return SLURM_SUCCESS;
}

static int _ping_controller(slurmdb_cluster_rec_t *cluster)
{
	int rc = SLURM_SUCCESS;
	slurm_msg_t req_msg;
	slurm_msg_t resp_msg;

	slurm_msg_t_init(&req_msg);
	req_msg.msg_type = REQUEST_PING;

	slurm_mutex_lock(&cluster->lock);

	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR) {
		debug("pinging %s(%s:%d)", cluster->name, cluster->control_host,
		      cluster->control_port);
	}

	if ((rc = _send_recv_msg(cluster, &req_msg, &resp_msg, true))) {
		if (_comm_fail_log(cluster)) {
			error("failed to ping %s(%s:%d)",
			      cluster->name, cluster->control_host,
			      cluster->control_port);
		}
	} else if ((rc = slurm_get_return_code(resp_msg.msg_type,
					       resp_msg.data))) {
		error("ping returned error from %s(%s:%d)",
		      cluster->name, cluster->control_host,
		      cluster->control_port);
	}

	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR) {
		debug("finished pinging %s(%s:%d)", cluster->name,
		      cluster->control_host, cluster->control_port);
	}

	slurm_mutex_unlock(&cluster->lock);
	slurm_free_msg_members(&req_msg);
	slurm_free_msg_members(&resp_msg);
	return rc;
}

/*
 * close all sibling conns
 * must lock before entering.
 */
static int _close_sibling_conns(void)
{
	ListIterator itr;
	slurmdb_cluster_rec_t *cluster;

	if (!fed_mgr_fed_rec || !fed_mgr_fed_rec->cluster_list)
		goto fini;

	itr = list_iterator_create(fed_mgr_fed_rec->cluster_list);
	while ((cluster = list_next(itr))) {
		if (cluster == fed_mgr_cluster_rec)
			continue;
		_close_controller_conn(cluster);
	}
	list_iterator_destroy(itr);

fini:
	return SLURM_SUCCESS;
}

static void *_ping_thread(void *arg)
{
	slurmctld_lock_t fed_read_lock = {
		NO_LOCK, NO_LOCK, NO_LOCK, NO_LOCK, READ_LOCK };

#if HAVE_SYS_PRCTL_H
	if (prctl(PR_SET_NAME, "fed_ping", NULL, NULL, NULL) < 0) {
		error("%s: cannot set my name to %s %m", __func__, "fed_ping");
	}
#endif
	while (!stop_pinging &&
	       !slurmctld_config.shutdown_time) {
		ListIterator itr;
		slurmdb_cluster_rec_t *cluster;

		lock_slurmctld(fed_read_lock);
		if (!fed_mgr_fed_rec || !fed_mgr_fed_rec->cluster_list)
			goto next;

		itr = list_iterator_create(fed_mgr_fed_rec->cluster_list);

		while ((cluster = list_next(itr))) {
			if (cluster == fed_mgr_cluster_rec)
				continue;
			_ping_controller(cluster);
		}
		list_iterator_destroy(itr);
next:
		unlock_slurmctld(fed_read_lock);

		sleep(5);
	}

	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
		info("Exiting ping thread");

	return NULL;
}

static void _create_ping_thread()
{
	pthread_attr_t attr;
	slurm_attr_init(&attr);

	stop_pinging = false;
	if (!ping_thread &&
	    (pthread_create(&ping_thread, &attr, _ping_thread, NULL) != 0)) {
		error("pthread_create of message thread: %m");
		slurm_attr_destroy(&attr);
		ping_thread = 0;
		return;
	}
	slurm_attr_destroy(&attr);
}

static void _destroy_ping_thread()
{
	stop_pinging = true;
	if (ping_thread) {
		/* can't wait for ping_thread to finish because it might be
		 * holding the read lock and we are already in the write lock.
		 * pthread_join(ping_thread, NULL);
		 */
		pthread_kill(ping_thread, SIGUSR1);
		ping_thread = 0;
	}
}

static void _mark_self_as_drained()
{
	List ret_list;
	slurmdb_cluster_cond_t cluster_cond;
	slurmdb_cluster_rec_t  cluster_rec;

	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
		info("%s: setting cluster fedstate to DRAINED", __func__);

	slurmdb_init_cluster_cond(&cluster_cond, false);
	slurmdb_init_cluster_rec(&cluster_rec, false);

	cluster_cond.cluster_list = list_create(NULL);
	list_append(cluster_cond.cluster_list, fed_mgr_cluster_rec->name);

	cluster_rec.fed.state = CLUSTER_FED_STATE_INACTIVE |
		(fed_mgr_cluster_rec->fed.state & ~CLUSTER_FED_STATE_BASE);

	ret_list = acct_storage_g_modify_clusters(acct_db_conn,
						  slurmctld_conf.slurm_user_id,
						  &cluster_cond, &cluster_rec);
	if (!ret_list || !list_count(ret_list)) {
		error("Failed to set cluster state to drained");
	}

	FREE_NULL_LIST(cluster_cond.cluster_list);
	FREE_NULL_LIST(ret_list);
}

static void _remove_self_from_federation()
{
	List ret_list;
	slurmdb_federation_cond_t fed_cond;
	slurmdb_federation_rec_t  fed_rec;
	slurmdb_cluster_rec_t     cluster_rec;

	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
		info("%s: removing self from federation %s",
		     __func__, fed_mgr_fed_rec->name);

	slurmdb_init_federation_cond(&fed_cond, false);
	slurmdb_init_federation_rec(&fed_rec, false);
	slurmdb_init_cluster_rec(&cluster_rec, false);

	fed_cond.federation_list = list_create(NULL);
	list_append(fed_cond.federation_list, fed_mgr_fed_rec->name);

	cluster_rec.name = xstrdup_printf("-%s", fed_mgr_cluster_rec->name);
	fed_rec.cluster_list = list_create(NULL);
	list_append(fed_rec.cluster_list, &cluster_rec);

	ret_list = acct_storage_g_modify_federations(
					acct_db_conn,
					slurmctld_conf.slurm_user_id, &fed_cond,
					&fed_rec);
	if (!ret_list || !list_count(ret_list)) {
		error("Failed to remove federation from list");
	}

	FREE_NULL_LIST(ret_list);
	FREE_NULL_LIST(fed_cond.federation_list);
	FREE_NULL_LIST(fed_rec.cluster_list);
	xfree(cluster_rec.name);

	slurmctld_config.scheduling_disabled = false;
	slurmctld_config.submissions_disabled = false;

	_leave_federation();
}

static void *_job_watch_thread(void *arg)
{
	struct timespec ts = {0, 0};
	slurmctld_lock_t job_read_fed_write_lock = {
		NO_LOCK, READ_LOCK, NO_LOCK, NO_LOCK, WRITE_LOCK };

#if HAVE_SYS_PRCTL_H
	if (prctl(PR_SET_NAME, "fed_jobw", NULL, NULL, NULL) < 0) {
		error("%s: cannot set my name to %s %m", __func__, "fed_jobw");
	}
#endif
	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
		info("%s: started job_watch thread", __func__);

	while (!slurmctld_config.shutdown_time && !stop_job_watch_thread) {
		int job_count = 0;

		slurm_mutex_lock(&job_watch_mutex);
		if (!slurmctld_config.shutdown_time && !stop_job_watch_thread) {
			ts.tv_sec  = time(NULL) + 30;
			pthread_cond_timedwait(&job_watch_cond,
					       &job_watch_mutex, &ts);
		}
		slurm_mutex_unlock(&job_watch_mutex);

		if (slurmctld_config.shutdown_time || stop_job_watch_thread)
			break;

		lock_slurmctld(job_read_fed_write_lock);

		if (!fed_mgr_cluster_rec) {
			/* not part of the federation anymore */
			unlock_slurmctld(job_read_fed_write_lock);
			break;
		}

		if ((job_count = list_count(job_list))) {
			if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
				info("%s: %d remaining jobs before being removed from the federation",
				     __func__, job_count);
		} else {
			if (fed_mgr_cluster_rec->fed.state &
			    CLUSTER_FED_STATE_REMOVE)
				_remove_self_from_federation();
			else if (fed_mgr_cluster_rec->fed.state &
				 CLUSTER_FED_STATE_DRAIN)
				_mark_self_as_drained();

			unlock_slurmctld(job_read_fed_write_lock);

			break;
		}

		unlock_slurmctld(job_read_fed_write_lock);
	}

	job_watch_thread_id = 0;

	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
		info("%s: exiting job watch thread", __func__);

	return NULL;
}

static void _spawn_job_watch_thread()
{
	pthread_attr_t thread_attr;

	if (!job_watch_thread_id) {
		slurm_attr_init(&thread_attr);
		/* Detach the thread since it will exit once the cluster is
		 * drained or removed. */
		pthread_attr_setdetachstate(&thread_attr,
					    PTHREAD_CREATE_DETACHED);

		slurm_mutex_lock(&job_watch_mutex);
		stop_job_watch_thread = false;
		if (pthread_create(&job_watch_thread_id, &thread_attr,
				   _job_watch_thread, NULL))
			fatal("pthread_create error %m");
		slurm_mutex_unlock(&job_watch_mutex);

		slurm_attr_destroy(&thread_attr);
	} else {
		info("a job_watch_thread already exists");
	}
}

static void _remove_job_watch_thread()
{
	if (job_watch_thread_id) {
		slurm_mutex_lock(&job_watch_mutex);
		stop_job_watch_thread = true;
		slurm_cond_broadcast(&job_watch_cond);
		slurm_mutex_unlock(&job_watch_mutex);
	}
}

static int _clear_recv_conns(void *object, void *arg)
{
	slurmdb_cluster_rec_t *cluster = (slurmdb_cluster_rec_t *)object;
	cluster->fed.recv = NULL;

	return SLURM_SUCCESS;
}

/*
 * Must have FED unlocked prior to entering
 */
static void _fed_mgr_ptr_init(slurmdb_federation_rec_t *db_fed,
			      slurmdb_cluster_rec_t *cluster)
{
	ListIterator c_itr;
	slurmdb_cluster_rec_t *tmp_cluster, *db_cluster;
	uint32_t cluster_state;
	int  base_state;
	bool drain_flag;

	slurmctld_lock_t fed_write_lock = {
		NO_LOCK, NO_LOCK, NO_LOCK, NO_LOCK, WRITE_LOCK };

	xassert(cluster);

	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
		info("Joining federation %s", db_fed->name);

	lock_slurmctld(fed_write_lock);
	if (fed_mgr_fed_rec) {
		/* we are already part of a federation, preserve existing
		 * conenctions */
		c_itr = list_iterator_create(db_fed->cluster_list);
		while ((db_cluster = list_next(c_itr))) {
			if (!xstrcmp(db_cluster->name,
				     slurmctld_conf.cluster_name)) {
				fed_mgr_cluster_rec = db_cluster;
				continue;
			}
			if (!(tmp_cluster =
			      fed_mgr_get_cluster_by_name(db_cluster->name))) {
				/* don't worry about destroying the connection
				 * here.  It will happen below when we free
				 * fed_mgr_fed_rec (automagically).
				 */
				continue;
			}
			slurm_mutex_lock(&tmp_cluster->lock);
			/* transfer over the connections we already have */
			db_cluster->fed.send = tmp_cluster->fed.send;
			tmp_cluster->fed.send = NULL;
			db_cluster->fed.recv = tmp_cluster->fed.recv;
			tmp_cluster->fed.recv = NULL;
			db_cluster->send_rpc = tmp_cluster->send_rpc;
			tmp_cluster->send_rpc = NULL;
			slurm_mutex_unlock(&tmp_cluster->lock);

			list_delete_all(fed_mgr_fed_rec->cluster_list,
					slurmdb_find_cluster_in_list,
					db_cluster->name);
		}
		list_iterator_destroy(c_itr);

		/* Remove any existing clusters that were part of the federation
		 * before and are not now. Don't free the recv connection now,
		 * it will get destroyed when the recv thread exits. */
		list_for_each(fed_mgr_fed_rec->cluster_list, _clear_recv_conns,
			      NULL);
		slurmdb_destroy_federation_rec(fed_mgr_fed_rec);
	} else
		fed_mgr_cluster_rec = cluster;

	fed_mgr_fed_rec = db_fed;

	/* Set scheduling and submissions states */
	cluster_state = fed_mgr_cluster_rec->fed.state;
	base_state  = (cluster_state & CLUSTER_FED_STATE_BASE);
	drain_flag  = (cluster_state & CLUSTER_FED_STATE_DRAIN);

	unlock_slurmctld(fed_write_lock);

	if (drain_flag) {
		slurmctld_config.scheduling_disabled = false;
		slurmctld_config.submissions_disabled = true;

		/* INACTIVE + DRAIN == DRAINED (already) */
		if (base_state == CLUSTER_FED_STATE_ACTIVE)
			_spawn_job_watch_thread();

	} else if (base_state == CLUSTER_FED_STATE_ACTIVE) {
		slurmctld_config.scheduling_disabled = false;
		slurmctld_config.submissions_disabled = false;
	} else if (base_state == CLUSTER_FED_STATE_INACTIVE) {
		slurmctld_config.scheduling_disabled = true;
		slurmctld_config.submissions_disabled = true;
	}
	if (!drain_flag && job_watch_thread_id)
		_remove_job_watch_thread();
}

/*
 * Must have FED write lock prior to entering
 */
static void _leave_federation(void)
{
	if (!fed_mgr_fed_rec)
		return;

	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
		info("Leaving federation %s", fed_mgr_fed_rec->name);

	_close_sibling_conns();
	_destroy_ping_thread();
	_remove_job_watch_thread();
	slurmdb_destroy_federation_rec(fed_mgr_fed_rec);
	fed_mgr_fed_rec = NULL;
	fed_mgr_cluster_rec = NULL;
}

static void _persist_callback_fini(void *arg)
{
	slurm_persist_conn_t *persist_conn = arg;
	slurmdb_cluster_rec_t *cluster;
	slurmctld_lock_t fed_write_lock = {
		NO_LOCK, NO_LOCK, NO_LOCK, NO_LOCK, WRITE_LOCK };

	/* If we are shutting down just return or you will get deadlock since
	 * all these locks are already locked.
	 */
	if (!persist_conn || *persist_conn->shutdown)
		return;
	lock_slurmctld(fed_write_lock);

	/* shuting down */
	if (!fed_mgr_fed_rec) {
		unlock_slurmctld(fed_write_lock);
		return;
	}

	if (!(cluster =
	      fed_mgr_get_cluster_by_name(persist_conn->cluster_name))) {
		info("Couldn't find cluster %s?",
		     persist_conn->cluster_name);
		unlock_slurmctld(fed_write_lock);
		return;
	}

	slurm_mutex_lock(&cluster->lock);

	/* This will get handled at the end of the thread, don't free it here */
	cluster->fed.recv = NULL;
//	persist_conn = cluster->fed.recv;
//	slurm_persist_conn_close(persist_conn);

	persist_conn = cluster->fed.send;
	if (persist_conn) {
		if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
			info("Closing send to sibling cluster %s",
			     cluster->name);
		slurm_persist_conn_destroy(persist_conn);
		cluster->fed.send = NULL;
	}

	slurm_mutex_unlock(&cluster->lock);
	unlock_slurmctld(fed_write_lock);
}

static void _join_federation(slurmdb_federation_rec_t *fed,
			     slurmdb_cluster_rec_t *cluster,
			     bool update)
{
	slurmctld_lock_t fed_read_lock = {
		NO_LOCK, NO_LOCK, NO_LOCK, NO_LOCK, READ_LOCK };

	_fed_mgr_ptr_init(fed, cluster);

	/* We must open the connections after we get out of the
	 * write_lock or we will end up in deadlock.
	 */
	if (!update) {
		lock_slurmctld(fed_read_lock);
		_open_persist_sends();
		unlock_slurmctld(fed_read_lock);
	}
	_create_ping_thread();
}

static int _persist_update_job(slurmdb_cluster_rec_t *conn, uint32_t job_id,
			       job_desc_msg_t *data, uid_t uid)
{
	int rc;
	slurm_msg_t req_msg, tmp_msg;
	sib_msg_t   sib_msg;
	Buf buffer;

	slurm_msg_t_init(&tmp_msg);
	tmp_msg.msg_type         = REQUEST_UPDATE_JOB;
	tmp_msg.data             = data;
	tmp_msg.protocol_version = SLURM_PROTOCOL_VERSION;

	buffer = init_buf(BUF_SIZE);
	pack_msg(&tmp_msg, buffer);

	memset(&sib_msg, 0, sizeof(sib_msg));
	sib_msg.sib_msg_type = FED_JOB_UPDATE;
	sib_msg.data_buffer  = buffer;
	sib_msg.data_type    = tmp_msg.msg_type;
	sib_msg.data_version = tmp_msg.protocol_version;
	sib_msg.req_uid      = uid;
	sib_msg.job_id       = job_id;

	slurm_msg_t_init(&req_msg);
	req_msg.msg_type = REQUEST_SIB_MSG;
	req_msg.data     = &sib_msg;

	rc = _queue_rpc(conn, &req_msg, 0, false);

	return rc;
}

static int _persist_update_job_resp(slurmdb_cluster_rec_t *conn,
				    uint32_t job_id, uint32_t return_code)
{
	int rc;
	slurm_msg_t req_msg;
	sib_msg_t   sib_msg;

	slurm_msg_t_init(&req_msg);

	memset(&sib_msg, 0, sizeof(sib_msg));
	sib_msg.sib_msg_type = FED_JOB_UPDATE_RESPONSE;
	sib_msg.job_id       = job_id;
	sib_msg.return_code  = return_code;

	slurm_msg_t_init(&req_msg);
	req_msg.msg_type = REQUEST_SIB_MSG;
	req_msg.data	 = &sib_msg;

	rc = _queue_rpc(conn, &req_msg, job_id, false);

	return rc;
}

/*
 * Remove a sibling job that won't be scheduled
 *
 * IN conn        - sibling connection
 * IN job_id      - the job's id
 * IN return_code - return code of job.
 * IN start_time  - time the fed job started
 * RET 0 on success, otherwise return -1 and set errno to indicate the error
 */
static int _persist_fed_job_revoke(slurmdb_cluster_rec_t *conn, uint32_t job_id,
				   uint32_t return_code, time_t start_time)
{
	int rc;
	slurm_msg_t req_msg;
	sib_msg_t   sib_msg;

	slurm_msg_t_init(&req_msg);

	memset(&sib_msg, 0, sizeof(sib_msg));
	sib_msg.sib_msg_type = FED_JOB_COMPLETE;
	sib_msg.job_id       = job_id;
	sib_msg.start_time   = start_time;
	sib_msg.return_code  = return_code;

	slurm_msg_t_init(&req_msg);
	req_msg.msg_type = REQUEST_SIB_MSG;
	req_msg.data	 = &sib_msg;

	rc = _queue_rpc(conn, &req_msg, job_id, false);

	return rc;
}

static int _persist_fed_job_response(slurmdb_cluster_rec_t *conn, uint32_t job_id,
				     uint32_t return_code)
{
	int rc;
	slurm_msg_t req_msg;
	sib_msg_t   sib_msg;

	slurm_msg_t_init(&req_msg);

	memset(&sib_msg, 0, sizeof(sib_msg));
	sib_msg.sib_msg_type = FED_JOB_SUBMIT_RESP;
	sib_msg.job_id       = job_id;
	sib_msg.return_code  = return_code;

	slurm_msg_t_init(&req_msg);
	req_msg.msg_type = REQUEST_SIB_MSG;
	req_msg.data	 = &sib_msg;

	rc = _queue_rpc(conn, &req_msg, job_id, false);

	return rc;
}

/*
 * Grab the fed lock on the sibling job.
 *
 * This message doesn't need to be queued because the other side just locks the
 * fed_job_list, checks and gets out -- doesn't need the internal locks.
 *
 * IN conn       - sibling connection
 * IN job_id     - the job's id
 * IN cluster_id - cluster id of the cluster locking
 * IN do_lock    - true == lock, false == unlock
 * RET 0 on success, otherwise return -1 and set errno to indicate the error
 */
static int _persist_fed_job_lock(slurmdb_cluster_rec_t *conn, uint32_t job_id,
				 uint32_t cluster_id, bool do_lock)
{
	int rc;
	slurm_msg_t req_msg, resp_msg;

	slurm_msg_t_init(&req_msg);

	sib_msg_t sib_msg;
	memset(&sib_msg, 0, sizeof(sib_msg_t));
	sib_msg.job_id     = job_id;
	sib_msg.cluster_id = cluster_id;

	if (do_lock)
		req_msg.msg_type = REQUEST_SIB_JOB_LOCK;
	else
		req_msg.msg_type = REQUEST_SIB_JOB_UNLOCK;

	req_msg.data     = &sib_msg;

	if (_send_recv_msg(conn, &req_msg, &resp_msg, false)) {
		rc = SLURM_PROTOCOL_ERROR;
		goto end_it;
	}

	switch (resp_msg.msg_type) {
	case RESPONSE_SLURM_RC:
		if ((rc = slurm_get_return_code(resp_msg.msg_type,
						resp_msg.data))) {
			slurm_seterrno(rc);
			rc = SLURM_PROTOCOL_ERROR;
		}
		break;
	default:
		slurm_seterrno(SLURM_UNEXPECTED_MSG_ERROR);
		rc = SLURM_PROTOCOL_ERROR;
		break;
	}

end_it:
	slurm_free_msg_members(&resp_msg);

	return rc;
}

/*
 * Tell the origin cluster that the job was started
 *
 * This message is queued up as an rpc and a fed_job_update so that it can
 * cancel the siblings without holding up the internal locks.
 *
 * IN conn       - sibling connection
 * IN job_id     - the job's id
 * IN cluster_id - cluster id of the cluster that started the job
 * IN start_time - time the fed job started
 * RET 0 on success, otherwise return -1 and set errno to indicate the error
 */
static int _persist_fed_job_start(slurmdb_cluster_rec_t *conn,
				  uint32_t job_id, uint32_t cluster_id,
				  time_t start_time)
{
	int rc;
	slurm_msg_t req_msg;

	slurm_msg_t_init(&req_msg);

	sib_msg_t sib_msg;
	memset(&sib_msg, 0, sizeof(sib_msg_t));
	sib_msg.sib_msg_type = FED_JOB_START;
	sib_msg.job_id       = job_id;
	sib_msg.cluster_id   = cluster_id;
	sib_msg.start_time   = start_time;

	req_msg.msg_type = REQUEST_SIB_MSG;
	req_msg.data     = &sib_msg;

	rc = _queue_rpc(conn, &req_msg, job_id, false);

	return rc;
}

/*
 * Send the specified signal to all steps of an existing job
 * IN job_id - the job's id
 * IN signal - signal number
 * IN flags  - see KILL_JOB_* flags above
 * IN uid    - uid of user making the request.
 * RET SLURM_SUCCESS on success, SLURM_ERROR otherwise.
 */
static int _persist_fed_job_cancel(slurmdb_cluster_rec_t *conn, uint32_t job_id,
				   uint16_t signal, uint16_t flags,
				   uid_t uid)
{
	int rc = SLURM_SUCCESS;
	slurm_msg_t req_msg, tmp_msg;
	sib_msg_t   sib_msg;
	job_step_kill_msg_t kill_req;
	Buf buffer;

	/* Build and pack a kill_req msg to put in a sib_msg */
	memset(&kill_req, 0, sizeof(job_step_kill_msg_t));
	kill_req.job_id      = job_id;
	kill_req.sjob_id     = NULL;
	kill_req.job_step_id = NO_VAL;
	kill_req.signal      = signal;
	kill_req.flags       = flags;

	slurm_msg_t_init(&tmp_msg);
	tmp_msg.msg_type         = REQUEST_CANCEL_JOB_STEP;
	tmp_msg.data             = &kill_req;
	tmp_msg.protocol_version = SLURM_PROTOCOL_VERSION;

	buffer = init_buf(BUF_SIZE);
	pack_msg(&tmp_msg, buffer);

	memset(&sib_msg, 0, sizeof(sib_msg));
	sib_msg.sib_msg_type = FED_JOB_CANCEL;
	sib_msg.data_buffer  = buffer;
	sib_msg.data_type    = tmp_msg.msg_type;
	sib_msg.data_version = tmp_msg.protocol_version;
	sib_msg.req_uid      = uid;

	slurm_msg_t_init(&req_msg);
	req_msg.msg_type = REQUEST_SIB_MSG;
	req_msg.data     = &sib_msg;

	rc = _queue_rpc(conn, &req_msg, job_id, false);

	free_buf(buffer);

	return rc;
}

/*
 * Tell the origin cluster to requeue the job
 *
 * IN conn       - sibling connection
 * IN job_id     - the job's id
 * IN start_time - time the fed job started
 * RET 0 on success, otherwise return -1 and set errno to indicate the error
 */
static int _persist_fed_job_requeue(slurmdb_cluster_rec_t *conn,
				    uint32_t job_id, uint32_t state)
{
	int rc;
	requeue_msg_t requeue_req;
	slurm_msg_t   req_msg, tmp_msg;
	sib_msg_t     sib_msg;
	Buf buffer;

	xassert(conn);

	requeue_req.job_id     = job_id;
	requeue_req.job_id_str = NULL;
	requeue_req.state      = state;

	slurm_msg_t_init(&tmp_msg);
	tmp_msg.msg_type         = REQUEST_JOB_REQUEUE;
	tmp_msg.data             = &requeue_req;
	tmp_msg.protocol_version = SLURM_PROTOCOL_VERSION;

	buffer = init_buf(BUF_SIZE);
	pack_msg(&tmp_msg, buffer);

	memset(&sib_msg, 0, sizeof(sib_msg));
	sib_msg.sib_msg_type = FED_JOB_REQUEUE;
	sib_msg.job_id       = job_id;
	sib_msg.data_buffer  = buffer;
	sib_msg.data_type    = tmp_msg.msg_type;
	sib_msg.data_version = tmp_msg.protocol_version;

	slurm_msg_t_init(&req_msg);
	req_msg.msg_type    = REQUEST_SIB_MSG;
	req_msg.data        = &sib_msg;

	rc = _queue_rpc(conn, &req_msg, job_id, false);

	free_buf(buffer);

	return rc;
}

static int _find_sibling_by_id(void *x, void *key)
{
	slurmdb_cluster_rec_t *object = (slurmdb_cluster_rec_t *)x;
	int id = (intptr_t)key;

	if (object->fed.id == id)
		return 1;

	return 0;
}

static void _add_fed_job_info(struct job_record *job_ptr)
{
	fed_job_info_t *job_info;

	job_info = xmalloc(sizeof(fed_job_info_t));
	memset(job_info, 0, sizeof(fed_job_info_t));
	job_info->job_id          = job_ptr->job_id;
	job_info->siblings_active = job_ptr->fed_details->siblings_active;
	job_info->siblings_viable = job_ptr->fed_details->siblings_viable;

	list_append(fed_job_list, job_info);
}

static int _delete_fed_job_info_by_id(void *object, void *arg)
{
	fed_job_info_t *job_info = (fed_job_info_t *)object;
	uint32_t job_id          = *(uint32_t *)arg;

	if (job_info->job_id == job_id)
		return true;

	return false;
}

extern void fed_mgr_remove_fed_job_info(uint32_t job_id)
{
	slurm_mutex_lock(&fed_job_list_mutex);

	if (fed_job_list)
		list_delete_all(fed_job_list, _delete_fed_job_info_by_id,
				&job_id);

	slurm_mutex_unlock(&fed_job_list_mutex);
}

static int _list_find_fed_job_info_by_jobid(void *x, void *key)
{
	fed_job_info_t *job_info = (fed_job_info_t *)x;
	uint32_t        job_id   = *(uint32_t *)key;

	if (job_info->job_id == job_id)
		return 1;

	return 0;
}

static fed_job_info_t *_find_fed_job_info(uint32_t job_id)
{
	return list_find_first(fed_job_list, _list_find_fed_job_info_by_jobid,
			       &job_id);
}

static void _destroy_fed_job_info(void *object)
{
	fed_job_info_t *job_info = (fed_job_info_t *)object;

	if (job_info) {
		xfree(job_info);
	}
}

static void _destroy_fed_job_update_info(void *object)
{
	fed_job_update_info_t *job_update_info =
		(fed_job_update_info_t *)object;

	if (job_update_info) {
		xfree(job_update_info->submit_cluster);
		slurm_free_job_info_msg(job_update_info->job_info_msg);
		slurm_free_job_desc_msg(job_update_info->submit_desc);
		xfree(job_update_info);
	}
}

extern slurmdb_cluster_rec_t *fed_mgr_get_cluster_by_id(uint32_t id)
{
	return list_find_first(fed_mgr_fed_rec->cluster_list,
			       _find_sibling_by_id, (void *)(intptr_t)id);
}

extern slurmdb_cluster_rec_t *fed_mgr_get_cluster_by_name(char *sib_name)
{
	if (!fed_mgr_fed_rec)
		return NULL;

	return list_find_first(fed_mgr_fed_rec->cluster_list,
			       slurmdb_find_cluster_in_list, sib_name);
}

/*
 * Revoke all sibling jobs except from cluster_id -- which the request came from
 *
 * IN job_ptr     - job to revoke
 * IN cluster_id  - cluster id of cluster that initiated call. Don're revoke
 * 	the job on this cluster -- it's the one that started the fed job.
 * IN revoke_sibs - bitmap of siblings to revoke.
 * IN start_time  - time the fed job started
 */
static void _revoke_sibling_jobs(uint32_t job_id, uint32_t cluster_id,
				 uint64_t revoke_sibs, time_t start_time)
{
	int id = 1;

	if (!fed_mgr_fed_rec) /* Not part of federation anymore */
		return;

	while (revoke_sibs) {
		if ((revoke_sibs & 1) &&
		    (id != fed_mgr_cluster_rec->fed.id) &&
		    (id != cluster_id)) {
			slurmdb_cluster_rec_t *cluster =
				fed_mgr_get_cluster_by_id(id);
			if (!cluster) {
				error("couldn't find cluster rec by id %d", id);
				goto next_job;
			}

			_persist_fed_job_revoke(cluster, job_id, 0, start_time);
		}

next_job:
		revoke_sibs >>= 1;
		id++;
	}
}

/* Parse a RESPONSE_CTLD_MULT_MSG message and return a bit set for every
 * successful operation */
bitstr_t *_parse_resp_ctld_mult(slurm_msg_t *resp_msg)
{
	ctld_list_msg_t *ctld_resp_msg;
	ListIterator iter = NULL;
	bitstr_t *success_bits;
	slurm_msg_t sub_msg;
	return_code_msg_t *rc_msg;
	Buf single_resp_buf = NULL;
	int resp_cnt, resp_inx = -1;

	xassert(resp_msg->msg_type == RESPONSE_CTLD_MULT_MSG);

	ctld_resp_msg = (ctld_list_msg_t *) resp_msg->data;
	if (!ctld_resp_msg->my_list) {
		error("%s: RESPONSE_CTLD_MULT_MSG has no list component",
		      __func__);
		return NULL;
	}

	resp_cnt = list_count(ctld_resp_msg->my_list);
	success_bits = bit_alloc(resp_cnt);
	iter = list_iterator_create(ctld_resp_msg->my_list);
	while ((single_resp_buf = list_next(iter))) {
		resp_inx++;
		slurm_msg_t_init(&sub_msg);
		if (unpack16(&sub_msg.msg_type, single_resp_buf) ||
		    unpack_msg(&sub_msg, single_resp_buf)) {
			error("%s: Sub-message unpack error for Message Type:%s",
			      __func__, rpc_num2string(sub_msg.msg_type));
			continue;
		}

		if (sub_msg.msg_type != RESPONSE_SLURM_RC) {
			error("%s: Unexpected Message Type:%s",
			      __func__, rpc_num2string(sub_msg.msg_type));
		} else {
			rc_msg = (return_code_msg_t *) sub_msg.data;
			if (rc_msg->return_code == SLURM_SUCCESS)
				bit_set(success_bits, resp_inx);
		}
		(void) slurm_free_msg_data(sub_msg.msg_type, sub_msg.data);

	}

	return success_bits;
}

static int _fed_mgr_job_allocate_sib(char *sib_name, job_desc_msg_t *job_desc,
				     bool interactive_job)
{
	int error_code = SLURM_SUCCESS;
	struct job_record *job_ptr = NULL;
	char *err_msg = NULL;
	bool reject_job = false;
	slurmdb_cluster_rec_t *sibling;
	uid_t uid = 0;
	slurm_msg_t response_msg;
	slurm_msg_t_init(&response_msg);

	xassert(sib_name);
	xassert(job_desc);

	if (!(sibling = fed_mgr_get_cluster_by_name(sib_name))) {
		error_code = ESLURM_INVALID_CLUSTER_NAME;
		error("Invalid sibling name");
	} else if ((job_desc->alloc_node == NULL) ||
	    (job_desc->alloc_node[0] == '\0')) {
		error_code = ESLURM_INVALID_NODE_NAME;
		error("REQUEST_SUBMIT_BATCH_JOB lacks alloc_node");
	}

	if (error_code == SLURM_SUCCESS)
		error_code = validate_job_create_req(job_desc,uid,&err_msg);

	if (error_code) {
		reject_job = true;
		goto send_msg;
	}

	/* Create new job allocation */
	error_code = job_allocate(job_desc, job_desc->immediate, false, NULL,
				  interactive_job, uid, &job_ptr, &err_msg,
				  sibling->rpc_version);
	if (!job_ptr ||
	    (error_code && job_ptr->job_state == JOB_FAILED))
		reject_job = true;

	if (job_desc->immediate &&
	    (error_code != SLURM_SUCCESS))
		error_code = ESLURM_CAN_NOT_START_IMMEDIATELY;

send_msg:
	/* Send response back about origin jobid if an error occured. */
	if (reject_job)
		_persist_fed_job_response(sibling, job_desc->job_id, error_code);
	else {
		_add_fed_job_info(job_ptr);
		schedule_job_save();	/* Has own locks */
		schedule_node_save();	/* Has own locks */
		queue_job_scheduler();
	}

	xfree(err_msg);

	return SLURM_SUCCESS;
}

static void _handle_fed_job_complete(fed_job_update_info_t *job_update_info)
{
	struct job_record *job_ptr;

	slurmctld_lock_t job_write_lock = {
		NO_LOCK, WRITE_LOCK, WRITE_LOCK, NO_LOCK, READ_LOCK };

	lock_slurmctld(job_write_lock);
	if (!(job_ptr = find_job_record(job_update_info->job_id))) {
		error("%s: failed to find job_record for fed job_id %d",
		      __func__, job_update_info->job_id);
		unlock_slurmctld(job_write_lock);
		return;
	}

	if (job_ptr->job_state & JOB_REQUEUE_FED) {
		/* Remove JOB_REQUEUE_FED and JOB_COMPLETING once
		 * sibling reports that sibling job is done. Leave other
		 * state in place. JOB_SPECIAL_EXIT may be in the
		 * states. */
		job_ptr->job_state &= ~(JOB_PENDING | JOB_COMPLETING);
		batch_requeue_fini(job_ptr);
	} else {
		fed_mgr_job_revoke(job_ptr, true, job_update_info->return_code,
				   job_update_info->start_time);
	}
	unlock_slurmctld(job_write_lock);
}

static void _handle_fed_job_cancel(fed_job_update_info_t *job_update_info)
{
	kill_job_step(job_update_info->kill_msg, job_update_info->uid);
}

static void
_handle_fed_job_remove_active_sib_bit(fed_job_update_info_t *job_update_info)
{
	fed_job_info_t *job_info;
	struct job_record *job_ptr;
	slurmdb_cluster_rec_t *sibling;

	slurmctld_lock_t job_write_lock = {
		NO_LOCK, WRITE_LOCK, NO_LOCK, NO_LOCK, READ_LOCK };

	lock_slurmctld(job_write_lock);
	if (!(job_ptr = find_job_record(job_update_info->job_id))) {
		error("%s: failed to find job_record for fed job_id %d",
		      __func__, job_update_info->job_id);
		unlock_slurmctld(job_write_lock);
		return;
	}

	slurm_mutex_lock(&fed_job_list_mutex);
	if (!(job_info = _find_fed_job_info(job_update_info->job_id))) {
		error("%s: failed to find fed job info for fed job_id %d",
		      __func__, job_update_info->job_id);
		slurm_mutex_unlock(&fed_job_list_mutex);
		unlock_slurmctld(job_write_lock);
		return;
	}

	sibling = fed_mgr_get_cluster_by_name(job_update_info->siblings_str);
	if (sibling) {
		job_info->siblings_active &=
			~(FED_SIBLING_BIT(sibling->fed.id));
		job_ptr->fed_details->siblings_active =
			job_info->siblings_active;
		update_job_fed_details(job_ptr);
	}

	slurm_mutex_unlock(&fed_job_list_mutex);
	unlock_slurmctld(job_write_lock);
}

static void _handle_fed_job_requeue(fed_job_update_info_t *job_update_info)
{
	int rc;
	slurmctld_lock_t job_write_lock = {
		NO_LOCK, WRITE_LOCK, WRITE_LOCK, NO_LOCK, READ_LOCK };

	lock_slurmctld(job_write_lock);
	if ((rc = job_requeue(job_update_info->uid, job_update_info->job_id,
			      NULL, false, job_update_info->state)))
		error("failed to requeue fed job %d - rc:%d",
		      job_update_info->job_id, rc);
	unlock_slurmctld(job_write_lock);
}

/*
 * Job has been started, revoke the sibling jobs.
 *
 * This is the common code between queued and local job starts.
 * This can be done when the job is starting on the origin cluster without
 * queueing because the cluster already has the job write lock and fed read
 * lock.
 *
 * Must have fed_job_list mutex locked and job write_lock set.
 */
static void _fed_job_start_revoke(fed_job_info_t *job_info,
				  struct job_record *job_ptr, time_t start_time)
{
	uint32_t cluster_lock;
	uint64_t old_active;
	uint64_t old_viable;

	cluster_lock = job_info->cluster_lock;
	old_active   = job_info->siblings_active;
	old_viable   = job_info->siblings_viable;

	job_ptr->fed_details->cluster_lock = cluster_lock;
	job_ptr->fed_details->siblings_active =
		job_info->siblings_active =
		FED_SIBLING_BIT(cluster_lock);
	update_job_fed_details(job_ptr);

	if (old_active & ~FED_SIBLING_BIT(cluster_lock)) {
		/* There are siblings that need to be removed */
		if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
			info("%s: %d is running on cluster id %d, revoking remote siblings (active:%"PRIu64" viable:%"PRIu64")",
			     __func__, job_info->job_id, cluster_lock,
			     old_active, old_viable);

		_revoke_sibling_jobs(job_ptr->job_id, cluster_lock,
				     old_active, start_time);
	}
}

static void _handle_fed_job_start(fed_job_update_info_t *job_update_info)
{
	fed_job_info_t *job_info;
	struct job_record *job_ptr;

	slurmctld_lock_t job_write_lock = {
		NO_LOCK, WRITE_LOCK, WRITE_LOCK, NO_LOCK, READ_LOCK };

	lock_slurmctld(job_write_lock);
	if (!(job_ptr = find_job_record(job_update_info->job_id))) {
		error("%s: failed to find job_record for fed job_id %d",
		      __func__, job_update_info->job_id);
		unlock_slurmctld(job_write_lock);
		return;
	}

	slurm_mutex_lock(&fed_job_list_mutex);
	if (!(job_info =
	      _find_fed_job_info(job_update_info->job_id))) {
		error("%s: failed to find fed job info for fed job_id %d",
		      __func__, job_update_info->job_id);
		slurm_mutex_unlock(&fed_job_list_mutex);
		unlock_slurmctld(job_write_lock);
		return;
	}

	_fed_job_start_revoke(job_info, job_ptr, job_update_info->start_time);

	slurm_mutex_unlock(&fed_job_list_mutex);

	if (job_info->cluster_lock != fed_mgr_cluster_rec->fed.id) {
		if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
			info("%s: %d is running remotely, revoking origin tracking job",
			     __func__, job_info->job_id);

		/* leave as pending so that it will stay around */
		fed_mgr_job_revoke(job_ptr, false, 0,
				   job_update_info->start_time);
	}

	unlock_slurmctld(job_write_lock);
}

static void _handle_fed_job_submission(fed_job_update_info_t *job_update_info)
{
	struct job_record *job_ptr;
	bool interactive_job =
		(job_update_info->type == FED_JOB_SUBMIT_INT) ?
		true : false;

	slurmctld_lock_t job_write_lock = {
		NO_LOCK, WRITE_LOCK, READ_LOCK, READ_LOCK, READ_LOCK };

	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
		info("%s: submitting %s sibling job %d from %s",
		     __func__, (interactive_job) ? "interactive" : "batch",
		     job_update_info->submit_desc->job_id,
		     job_update_info->submit_cluster);

	lock_slurmctld(job_write_lock);

	if ((job_ptr = find_job_record(job_update_info->job_id))) {
		info("Found existing fed job %d, going to requeue/kill it",
		     job_update_info->job_id);
		purge_job_record(job_ptr->job_id);
	}

	_fed_mgr_job_allocate_sib(job_update_info->submit_cluster,
				  job_update_info->submit_desc,
				  interactive_job);
	unlock_slurmctld(job_write_lock);
}

static void _handle_fed_job_update(fed_job_update_info_t *job_update_info)
{
	int rc;
	slurm_msg_t msg;
	slurm_msg_t_init(&msg);
	job_desc_msg_t *job_desc = job_update_info->submit_desc;
	int db_inx_max_cnt = 5, i=0;
	slurmdb_cluster_rec_t *sibling;

	slurmctld_lock_t job_write_lock = {
		NO_LOCK, WRITE_LOCK, READ_LOCK, READ_LOCK, READ_LOCK };
	slurmctld_lock_t fed_read_lock = {
		NO_LOCK, NO_LOCK, NO_LOCK, NO_LOCK, READ_LOCK };

	xassert(job_desc);

	/* scontrol always sets job_id_str */
	job_desc->job_id = job_update_info->job_id;
	msg.data = job_desc;

	rc = ESLURM_JOB_SETTING_DB_INX;
	while (rc == ESLURM_JOB_SETTING_DB_INX) {
		lock_slurmctld(job_write_lock);
		rc = update_job(&msg, job_update_info->uid, false);
		unlock_slurmctld(job_write_lock);

		if (i >= db_inx_max_cnt) {
			info("%s: can't update fed job, waited %d seconds for job %u to get a db_index, but it hasn't happened yet.  Giving up and letting the user know.",
			     __func__, db_inx_max_cnt,
			     job_update_info->job_id);
			break;
		}
		i++;
		debug("%s: We cannot update job %u at the moment, we are setting the db index, waiting",
		      __func__, job_update_info->job_id);
		sleep(1);
	}

	lock_slurmctld(fed_read_lock);
	if (!(sibling =
	      fed_mgr_get_cluster_by_name(job_update_info->submit_cluster))) {
		error("Invalid sibling name");
	} else {
		_persist_update_job_resp(sibling, job_update_info->job_id, rc);
	}
	unlock_slurmctld(fed_read_lock);
}

static void
_handle_fed_job_update_response(fed_job_update_info_t *job_update_info)
{
	fed_job_info_t *job_info;
	slurmdb_cluster_rec_t *sibling;

	slurmctld_lock_t fed_read_lock = {
		NO_LOCK, NO_LOCK, NO_LOCK, NO_LOCK, READ_LOCK };

	slurm_mutex_lock(&fed_job_list_mutex);
	if (!(job_info = _find_fed_job_info(job_update_info->job_id))) {
		error("%s: failed to find fed job info for fed job_id %d",
		      __func__, job_update_info->job_id);
		slurm_mutex_unlock(&fed_job_list_mutex);
		return;
	}

	lock_slurmctld(fed_read_lock);

	if (!(sibling =
	      fed_mgr_get_cluster_by_name(job_update_info->submit_cluster))) {
		error("Invalid sibling name");
		unlock_slurmctld(fed_read_lock);
		slurm_mutex_unlock(&fed_job_list_mutex);
		return;
	}

	if (job_info->updating_sibs[sibling->fed.id])
		job_info->updating_sibs[sibling->fed.id]--;
	else
		error("%s this should never happen", __func__);

	slurm_mutex_unlock(&fed_job_list_mutex);
	unlock_slurmctld(fed_read_lock);
}

extern int _handle_fed_job_sync(fed_job_update_info_t *job_update_info)
{
	int rc = SLURM_SUCCESS;

	slurmctld_lock_t job_write_lock = {
		NO_LOCK, WRITE_LOCK, NO_LOCK, NO_LOCK, READ_LOCK };

	lock_slurmctld(job_write_lock);

	rc = _sync_jobs(job_update_info->submit_cluster,
			job_update_info->job_info_msg,
			job_update_info->start_time);

	unlock_slurmctld(job_write_lock);

	return rc;
}

/* Have to send the job sync from the job_update thread so that it can
 * independently get the job read lock. */
extern int _handle_fed_send_job_sync(fed_job_update_info_t *job_update_info)
{
        int rc = SLURM_PROTOCOL_SUCCESS;
	List jobids;
        slurm_msg_t req_msg, job_msg;
	sib_msg_t sib_msg = {0};
	char *dump = NULL;
	int dump_size = 0;
	slurmdb_cluster_rec_t *sibling;
	Buf buffer;
	time_t sync_time = 0;
	char *sib_name = job_update_info->submit_cluster;

	slurmctld_lock_t job_read_lock = {
		NO_LOCK, READ_LOCK, NO_LOCK, NO_LOCK, READ_LOCK };

	lock_slurmctld(job_read_lock);

	if (!(sibling = fed_mgr_get_cluster_by_name(sib_name))) {
		error("%s: Invalid sibling name %s", __func__, sib_name);
		unlock_slurmctld(job_read_lock);
		return SLURM_ERROR;
	}

	sync_time = time(NULL);
	jobids = _get_sync_jobid_list(sibling->fed.id, sync_time);
	pack_spec_jobs(&dump, &dump_size, jobids, SHOW_ALL,
		       slurmctld_conf.slurm_user_id, NO_VAL,
		       sibling->rpc_version);
	FREE_NULL_LIST(jobids);

	unlock_slurmctld(job_read_lock);

	slurm_msg_t_init(&job_msg);
	job_msg.protocol_version = sibling->rpc_version;
	job_msg.msg_type         = RESPONSE_JOB_INFO;
	job_msg.data             = dump;
	job_msg.data_size        = dump_size;

	buffer = init_buf(BUF_SIZE);
	pack_msg(&job_msg, buffer);

	memset(&sib_msg, 0, sizeof(sib_msg_t));
	sib_msg.sib_msg_type = FED_JOB_SYNC;
	sib_msg.data_buffer  = buffer;
	sib_msg.data_type    = job_msg.msg_type;
	sib_msg.data_version = job_msg.protocol_version;
	sib_msg.start_time   = sync_time;

	slurm_msg_t_init(&req_msg);
	req_msg.msg_type = REQUEST_SIB_MSG;
	req_msg.data     = &sib_msg;

	rc = _queue_rpc(sibling, &req_msg, 0, false);

	return rc;
}

static int _foreach_fed_job_update_info(fed_job_update_info_t *job_update_info)
{
	if (!fed_mgr_cluster_rec) {
		info("Not part of federation anymore, not performing fed job updates");
		return SLURM_SUCCESS;
	}

	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
		info("%s: job_id:%u type:%s",
		     __func__, job_update_info->job_id,
		     _job_update_type_str(job_update_info->type));

	switch (job_update_info->type) {
	case FED_JOB_COMPLETE:
		_handle_fed_job_complete(job_update_info);
		break;
	case FED_JOB_CANCEL:
		_handle_fed_job_cancel(job_update_info);
		break;
	case FED_JOB_REMOVE_ACTIVE_SIB_BIT:
		_handle_fed_job_remove_active_sib_bit(job_update_info);
		break;
	case FED_JOB_REQUEUE:
		_handle_fed_job_requeue(job_update_info);
		break;
	case FED_JOB_START:
		_handle_fed_job_start(job_update_info);
		break;
	case FED_JOB_SUBMIT_BATCH:
	case FED_JOB_SUBMIT_INT:
		_handle_fed_job_submission(job_update_info);
		break;
	case FED_JOB_SYNC:
		_handle_fed_job_sync(job_update_info);
		break;
	case FED_JOB_UPDATE:
		_handle_fed_job_update(job_update_info);
		break;
	case FED_JOB_UPDATE_RESPONSE:
		_handle_fed_job_update_response(job_update_info);
		break;
	case FED_SEND_JOB_SYNC:
		_handle_fed_send_job_sync(job_update_info);
		break;
	default:
		error("Invalid fed_job type: %d jobid: %u",
		      job_update_info->type, job_update_info->job_id);
	}

	return SLURM_SUCCESS;
}

/* Start a thread to manage queued sibling requests */
static void *_fed_job_update_thread(void *arg)
{
	struct timespec ts = {0, 0};
	fed_job_update_info_t *job_update_info;

#if HAVE_SYS_PRCTL_H
	if (prctl(PR_SET_NAME, "fed_jobs", NULL, NULL, NULL) < 0) {
		error("%s: cannot set my name to %s %m", __func__, "fed_jobs");
	}
#endif

	while (!slurmctld_config.shutdown_time) {
		slurm_mutex_lock(&job_update_mutex);
		ts.tv_sec  = time(NULL) + 2;
		pthread_cond_timedwait(&job_update_cond,
				       &job_update_mutex, &ts);
		slurm_mutex_unlock(&job_update_mutex);

		if (slurmctld_config.shutdown_time)
			break;

		while ((job_update_info = list_pop(fed_job_update_list))) {
			_foreach_fed_job_update_info(job_update_info);
			_destroy_fed_job_update_info(job_update_info);
		}
	}

	return NULL;
}

/* Start a thread to manage queued agent requests */
static void *_agent_thread(void *arg)
{
	slurmdb_cluster_rec_t *cluster;
	struct timespec ts = {0, 0};
	ListIterator cluster_iter, rpc_iter;
	agent_queue_t *rpc_rec;
	slurm_msg_t req_msg, resp_msg;
	ctld_list_msg_t ctld_req_msg;
	bitstr_t *success_bits;
	int rc, resp_inx, success_size;

	slurmctld_lock_t fed_read_lock = {
		NO_LOCK, NO_LOCK, NO_LOCK, NO_LOCK, READ_LOCK };

#if HAVE_SYS_PRCTL_H
	if (prctl(PR_SET_NAME, "fed_agent", NULL, NULL, NULL) < 0) {
		error("%s: cannot set my name to %s %m", __func__, "fed_agent");
	}
#endif

	while (!slurmctld_config.shutdown_time) {
		/* Wait for new work or re-issue RPCs after 2 second wait */
		slurm_mutex_lock(&agent_mutex);
		if (!slurmctld_config.shutdown_time && !agent_queue_size) {
			ts.tv_sec  = time(NULL) + 2;
			pthread_cond_timedwait(&agent_cond, &agent_mutex, &ts);
		}
		agent_queue_size = 0;
		slurm_mutex_unlock(&agent_mutex);
		if (slurmctld_config.shutdown_time)
			break;

		lock_slurmctld(fed_read_lock);
		if (!fed_mgr_fed_rec || !fed_mgr_fed_rec->cluster_list) {
			unlock_slurmctld(fed_read_lock);
			continue;
		}

		/* Look for work on each cluster */
		cluster_iter = list_iterator_create(
					fed_mgr_fed_rec->cluster_list);
		while (!slurmctld_config.shutdown_time &&
		       (cluster = list_next(cluster_iter))) {
			time_t now = time(NULL);
			if ((cluster->send_rpc == NULL) ||
			   (list_count(cluster->send_rpc) == 0))
				continue;

			/* Move currently pending RPCs to new list */
			ctld_req_msg.my_list = NULL;
			rpc_iter = list_iterator_create(cluster->send_rpc);
			while ((rpc_rec = list_next(rpc_iter))) {
				if ((rpc_rec->last_try + rpc_rec->last_defer) >=
				    now)
					continue;
				if (!ctld_req_msg.my_list)
					ctld_req_msg.my_list =list_create(NULL);
				list_append(ctld_req_msg.my_list,
					    rpc_rec->buffer);
				rpc_rec->last_try = now;
				if (rpc_rec->last_defer == 128) {
					info("%s: %s %u request to cluster %s is repeatedly failing",
					     __func__,
					     rpc_num2string(rpc_rec->msg_type),
					     rpc_rec->job_id, cluster->name);
					rpc_rec->last_defer *= 2;
				} else if (rpc_rec->last_defer)
					rpc_rec->last_defer *= 2;
				else
					rpc_rec->last_defer = 2;
			}
			list_iterator_destroy(rpc_iter);
			if (!ctld_req_msg.my_list)
				continue;

			/* Build, pack and send the combined RPC */
			slurm_msg_t_init(&req_msg);
			req_msg.msg_type = REQUEST_CTLD_MULT_MSG;
			req_msg.data     = &ctld_req_msg;
			rc = _send_recv_msg(cluster, &req_msg, &resp_msg,
					    false);

			/* Process the response */
			if ((rc == SLURM_SUCCESS) &&
			    (resp_msg.msg_type == RESPONSE_CTLD_MULT_MSG)) {
				/* Remove successfully processed RPCs */
				resp_inx = 0;
				success_bits = _parse_resp_ctld_mult(&resp_msg);
				success_size = bit_size(success_bits);
				rpc_iter = list_iterator_create(cluster->
								send_rpc);
				while ((rpc_rec = list_next(rpc_iter))) {
					if (rpc_rec->last_try != now)
						continue;
					if (resp_inx >= success_size) {
						error("%s: bitmap too small (%d >= %d)",
						      __func__, resp_inx,
						      success_size);
						break;
					}
					if (bit_test(success_bits, resp_inx++))
						list_delete_item(rpc_iter);
				}
				list_iterator_destroy(rpc_iter);
				FREE_NULL_BITMAP(success_bits);
			} else {
				/* Failed to process combined RPC.
				 * Leave all RPCs on the queue. */
				if (rc != SLURM_SUCCESS) {
					if (_comm_fail_log(cluster)) {
						error("%s: Failed to send RPC: %s",
						      __func__,
						      slurm_strerror(rc));
					} else {
						debug("%s: Failed to send RPC: %s",
						      __func__,
						      slurm_strerror(rc));
					}
				} else if (resp_msg.msg_type ==
					   PERSIST_RC) {
					persist_rc_msg_t *msg;
					char *err_str;
					msg = resp_msg.data;
					if (msg->comment)
						err_str = msg->comment;
					else
						err_str=slurm_strerror(msg->rc);
					error("%s: failed to process msg: %s",
					      __func__, err_str);
				} else if (resp_msg.msg_type ==
					   RESPONSE_SLURM_RC) {
					rc = slurm_get_return_code(
						resp_msg.msg_type,
						resp_msg.data);
					error("%s: failed to process msg: %s",
					      __func__, slurm_strerror(rc));
				} else {
					error("%s: Invalid response msg_type: %u",
					      __func__, resp_msg.msg_type);
				}
			}
			(void) slurm_free_msg_data(resp_msg.msg_type,
						   resp_msg.data);

			list_destroy(ctld_req_msg.my_list);
		}
		list_iterator_destroy(cluster_iter);

		unlock_slurmctld(fed_read_lock);
	}

	/* Log the abandoned RPCs */
	lock_slurmctld(fed_read_lock);
	if (!fed_mgr_fed_rec)
		goto end_it;

	cluster_iter = list_iterator_create(fed_mgr_fed_rec->cluster_list);
	while ((cluster = list_next(cluster_iter))) {
		if (cluster->send_rpc == NULL)
			continue;

		rpc_iter = list_iterator_create(cluster->send_rpc);
		while ((rpc_rec = list_next(rpc_iter))) {
			info("%s: %s %u request to cluster %s aborted",
			     __func__, rpc_num2string(rpc_rec->msg_type),
			     rpc_rec->job_id, cluster->name);
			list_delete_item(rpc_iter);
		}
		list_iterator_destroy(rpc_iter);
		FREE_NULL_LIST(cluster->send_rpc);
	}
	list_iterator_destroy(cluster_iter);

end_it:
	unlock_slurmctld(fed_read_lock);

	return NULL;
}

static void _spawn_threads(void)
{
	pthread_attr_t thread_attr;

	slurm_attr_init(&thread_attr);

	slurm_mutex_lock(&agent_mutex);
	if (pthread_create(&agent_thread_id, &thread_attr, _agent_thread, NULL))
		fatal("pthread_create error %m");
	slurm_mutex_unlock(&agent_mutex);

	slurm_mutex_lock(&job_update_mutex);
	if (pthread_create(&fed_job_update_thread_id, &thread_attr,
			   _fed_job_update_thread, NULL))
		fatal("pthread_create error %m");
	slurm_mutex_unlock(&job_update_mutex);

	slurm_attr_destroy(&thread_attr);
}

extern int fed_mgr_init(void *db_conn)
{
	int rc = SLURM_SUCCESS;
	slurmdb_federation_cond_t fed_cond;
	List fed_list;
	slurmdb_federation_rec_t *fed = NULL;

	slurm_mutex_lock(&init_mutex);

	if (inited) {
		slurm_mutex_unlock(&init_mutex);
		return SLURM_SUCCESS;
	}

	if (!association_based_accounting)
		goto end_it;

	if (!fed_job_list)
		fed_job_list = list_create(_destroy_fed_job_info);
	if (!fed_job_update_list)
		fed_job_update_list = list_create(_destroy_fed_job_update_info);

	slurm_persist_conn_recv_server_init();
	_spawn_threads();

	if (running_cache) {
		debug("Database appears down, reading federations from state file.");
		fed = _state_load(
			slurmctld_conf.state_save_location, false);
		if (!fed) {
			debug2("No federation state");
			rc = SLURM_SUCCESS;
			goto end_it;
		}
	} else {
		/* Load fed_job_list */
		_state_load(slurmctld_conf.state_save_location, true);

		slurmdb_init_federation_cond(&fed_cond, 0);
		fed_cond.cluster_list = list_create(NULL);
		list_append(fed_cond.cluster_list, slurmctld_conf.cluster_name);

		fed_list = acct_storage_g_get_federations(
						db_conn,
						slurmctld_conf.slurm_user_id,
						&fed_cond);
		FREE_NULL_LIST(fed_cond.cluster_list);
		if (!fed_list) {
			error("failed to get a federation list");
			rc = SLURM_ERROR;
			goto end_it;
		}

		if (list_count(fed_list) == 1)
			fed = list_pop(fed_list);
		else if (list_count(fed_list) > 1) {
			error("got more federations than expected");
			rc = SLURM_ERROR;
		}
		FREE_NULL_LIST(fed_list);
	}

	if (fed) {
		slurmdb_cluster_rec_t *cluster = NULL;

		if ((cluster = list_find_first(fed->cluster_list,
					       slurmdb_find_cluster_in_list,
					       slurmctld_conf.cluster_name))) {
			_join_federation(fed, cluster, false);
		} else {
			error("failed to get cluster from federation that we requested");
			rc = SLURM_ERROR;
		}
	}

end_it:
	inited = true;
	slurm_mutex_unlock(&init_mutex);

	return rc;
}

extern int fed_mgr_fini(void)
{
	slurmctld_lock_t fed_write_lock = {
		NO_LOCK, NO_LOCK, NO_LOCK, NO_LOCK, WRITE_LOCK };

	slurm_mutex_lock(&init_mutex);
	inited = false;
	slurm_mutex_unlock(&init_mutex);

	lock_slurmctld(fed_write_lock);
	/* Call _leave_federation() before slurm_persist_conn_recv_server_fini()
	 * as this will NULL out the cluster's recv persistent connection before
	 * _server_fini() actually destroy's it. That way the cluster's recv
	 * connection won't be pointing to bad memory. */
	_leave_federation();
	unlock_slurmctld(fed_write_lock);

	slurm_persist_conn_recv_server_fini();

	if (agent_thread_id)
		pthread_join(agent_thread_id, NULL);

	_remove_job_watch_thread();

	return SLURM_SUCCESS;
}

extern int fed_mgr_update_feds(slurmdb_update_object_t *update)
{
	List feds;
	slurmdb_federation_rec_t *fed = NULL;
	slurmdb_cluster_rec_t *cluster = NULL;
	slurmctld_lock_t fed_write_lock = {
		NO_LOCK, NO_LOCK, NO_LOCK, NO_LOCK, WRITE_LOCK };

	if (!update->objects)
		return SLURM_SUCCESS;

	slurm_mutex_lock(&init_mutex);
	if (!inited) {
		slurm_mutex_unlock(&init_mutex);
		return SLURM_SUCCESS; /* we haven't started the fed mgr and we
				       * can't start it from here, don't worry
				       * all will get set up later. */
	}
	slurm_mutex_unlock(&init_mutex);
	/* we only want one update happening at a time. */
	slurm_mutex_lock(&update_mutex);
	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
		info("Got a federation update");

	feds = update->objects;

	/* find the federation that this cluster is in.
	 * if it's changed from last time then update stored information.
	 * grab other clusters in federation
	 * establish connections with each cluster in federation */

	/* what if a remote cluster is removed from federation.
	 * have to detect that and close the connection to the remote */
	while ((fed = list_pop(feds))) {
		if (fed->cluster_list &&
		    (cluster = list_find_first(fed->cluster_list,
					       slurmdb_find_cluster_in_list,
					       slurmctld_conf.cluster_name))) {
			_join_federation(fed, cluster, true);
			break;
		}

		slurmdb_destroy_federation_rec(fed);
	}

	if (!fed) {
		if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
			info("Not part of any federation");
		lock_slurmctld(fed_write_lock);
		_leave_federation();
		unlock_slurmctld(fed_write_lock);
	}
	slurm_mutex_unlock(&update_mutex);
	return SLURM_SUCCESS;
}

static void _pack_fed_job_info(fed_job_info_t *job_info, Buf buffer,
			       uint16_t protocol_version)
{
	int i;
	if (protocol_version >= SLURM_17_11_PROTOCOL_VERSION) {
		pack32(job_info->cluster_lock, buffer);
		pack32(job_info->job_id, buffer);
		pack64(job_info->siblings_active, buffer);
		pack64(job_info->siblings_viable, buffer);

		for (i = 0; i <= MAX_FED_CLUSTERS; i++)
			pack32(job_info->updating_sibs[i], buffer);
		for (i = 0; i <= MAX_FED_CLUSTERS; i++)
			pack_time(job_info->updating_time[i], buffer);
	} else {
		error("%s: protocol_version %hu not supported.",
		      __func__, protocol_version);
	}
}

static int _unpack_fed_job_info(fed_job_info_t **job_info_pptr, Buf buffer,
				 uint16_t protocol_version)
{
	int i;
	fed_job_info_t *job_info = xmalloc(sizeof(fed_job_info_t));

	*job_info_pptr = job_info;

	if (protocol_version >= SLURM_17_11_PROTOCOL_VERSION) {
		safe_unpack32(&job_info->cluster_lock, buffer);
		safe_unpack32(&job_info->job_id, buffer);
		safe_unpack64(&job_info->siblings_active, buffer);
		safe_unpack64(&job_info->siblings_viable, buffer);

		for (i = 0; i <= MAX_FED_CLUSTERS; i++)
			safe_unpack32(&job_info->updating_sibs[i], buffer);
		for (i = 0; i <= MAX_FED_CLUSTERS; i++)
			safe_unpack_time(&job_info->updating_time[i], buffer);
	} else {
		error("%s: protocol_version %hu not supported.",
		      __func__, protocol_version);
		goto unpack_error;
	}

	return SLURM_SUCCESS;

unpack_error:
	_destroy_fed_job_info(job_info);
	*job_info_pptr = NULL;
	return SLURM_ERROR;
}

static void _dump_fed_job_list(Buf buffer, uint16_t protocol_version)
{
	uint32_t count = NO_VAL;
	fed_job_info_t *fed_job_info;

	if (protocol_version <= SLURM_17_11_PROTOCOL_VERSION) {
		if (fed_job_list)
			count = list_count(fed_job_list);
		else
			count = NO_VAL;

		pack32(count, buffer);
		if (count && (count != NO_VAL)) {
			ListIterator itr = list_iterator_create(fed_job_list);
			while ((fed_job_info = list_next(itr))) {
				_pack_fed_job_info(fed_job_info, buffer,
						   protocol_version);
			}
			list_iterator_destroy(itr);
		}
	} else {
		error("%s: protocol_version %hu not supported.",
		      __func__, protocol_version);
	}
}

static List _load_fed_job_list(Buf buffer, uint16_t protocol_version)
{
	int i;
	uint32_t count;
	fed_job_info_t *tmp_job_info = NULL;
	List tmp_list = NULL;

	if (protocol_version <= SLURM_17_11_PROTOCOL_VERSION) {
		safe_unpack32(&count, buffer);
		if (count > NO_VAL32)
			goto unpack_error;
		if (count != NO_VAL32) {
			tmp_list = list_create(_destroy_fed_job_info);

			for (i = 0; i < count; i++) {
				if (_unpack_fed_job_info(&tmp_job_info, buffer,
						     protocol_version))
					goto unpack_error;
				list_append(tmp_list, tmp_job_info);
			}
		}
	} else {
		error("%s: protocol_version %hu not supported.",
		      __func__, protocol_version);
	}

	return tmp_list;

unpack_error:
	FREE_NULL_LIST(tmp_list);
	return NULL;
}

extern int fed_mgr_state_save(char *state_save_location)
{
	int error_code = 0, log_fd;
	char *old_file = NULL, *new_file = NULL, *reg_file = NULL;
	slurmctld_lock_t fed_read_lock = {
		NO_LOCK, NO_LOCK, NO_LOCK, NO_LOCK, READ_LOCK };

	Buf buffer = init_buf(0);

	DEF_TIMERS;

	START_TIMER;

	/* write header: version, time */
	pack16(SLURM_PROTOCOL_VERSION, buffer);
	pack_time(time(NULL), buffer);

	lock_slurmctld(fed_read_lock);
	slurmdb_pack_federation_rec(fed_mgr_fed_rec, SLURM_PROTOCOL_VERSION,
				    buffer);
	unlock_slurmctld(fed_read_lock);

	_dump_fed_job_list(buffer, SLURM_PROTOCOL_VERSION);

	/* write the buffer to file */
	reg_file = xstrdup_printf("%s/%s", state_save_location,
				  FED_MGR_STATE_FILE);
	old_file = xstrdup_printf("%s.old", reg_file);
	new_file = xstrdup_printf("%s.new", reg_file);

	log_fd = creat(new_file, 0600);
	if (log_fd < 0) {
		error("Can't save state, create file %s error %m", new_file);
		error_code = errno;
	} else {
		int pos = 0, nwrite = get_buf_offset(buffer), amount;
		char *data = (char *)get_buf_data(buffer);
		while (nwrite > 0) {
			amount = write(log_fd, &data[pos], nwrite);
			if ((amount < 0) && (errno != EINTR)) {
				error("Error writing file %s, %m", new_file);
				error_code = errno;
				break;
			}
			nwrite -= amount;
			pos    += amount;
		}
		fsync(log_fd);
		close(log_fd);
	}
	if (error_code)
		(void) unlink(new_file);
	else {			/* file shuffle */
		(void) unlink(old_file);
		if (link(reg_file, old_file))
			debug4("unable to create link for %s -> %s: %m",
			       reg_file, old_file);
		(void) unlink(reg_file);
		if (link(new_file, reg_file))
			debug4("unable to create link for %s -> %s: %m",
			       new_file, reg_file);
		(void) unlink(new_file);
	}
	xfree(old_file);
	xfree(reg_file);
	xfree(new_file);

	free_buf(buffer);

	END_TIMER2("fed_mgr_state_save");

	return error_code;
}

static slurmdb_federation_rec_t *_state_load(char *state_save_location,
					     bool job_list_only)
{
	Buf buffer = NULL;
	char *data = NULL, *state_file;
	time_t buf_time;
	uint16_t ver = 0;
	uint32_t data_size = 0;
	int state_fd;
	int data_allocated, data_read = 0, error_code = SLURM_SUCCESS;
	slurmdb_federation_rec_t *ret_fed = NULL;
	List tmp_list = NULL;

	state_file = xstrdup_printf("%s/%s", state_save_location,
				    FED_MGR_STATE_FILE);
	state_fd = open(state_file, O_RDONLY);
	if (state_fd < 0) {
		error("No fed_mgr state file (%s) to recover", state_file);
		xfree(state_file);
		return NULL;
	} else {
		data_allocated = BUF_SIZE;
		data = xmalloc(data_allocated);
		while (1) {
			data_read = read(state_fd, &data[data_size],
					 BUF_SIZE);
			if (data_read < 0) {
				if (errno == EINTR)
					continue;
				else {
					error("Read error on %s: %m",
					      state_file);
					break;
				}
			} else if (data_read == 0)	/* eof */
				break;
			data_size      += data_read;
			data_allocated += data_read;
			xrealloc(data, data_allocated);
		}
		close(state_fd);
	}
	xfree(state_file);

	buffer = create_buf(data, data_size);

	safe_unpack16(&ver, buffer);

	debug3("Version in fed_mgr_state header is %u", ver);
	if (ver > SLURM_PROTOCOL_VERSION || ver < SLURM_MIN_PROTOCOL_VERSION) {
		error("***********************************************");
		error("Can not recover fed_mgr state, incompatible version, "
		      "got %u need > %u <= %u", ver,
		      SLURM_MIN_PROTOCOL_VERSION, SLURM_PROTOCOL_VERSION);
		error("***********************************************");
		free_buf(buffer);
		return NULL;
	}

	safe_unpack_time(&buf_time, buffer);

	error_code = slurmdb_unpack_federation_rec((void **)&ret_fed, ver,
						   buffer);
	if (error_code != SLURM_SUCCESS)
		goto unpack_error;
	else if (job_list_only || !ret_fed || !ret_fed->name ||
		 !list_count(ret_fed->cluster_list)) {
		slurmdb_destroy_federation_rec(ret_fed);
		ret_fed = NULL;

		if (!job_list_only)
			debug("No feds to retrieve from state");
	} else {
		/* We want to free the connections here since they don't exist
		 * anymore, but they were packed when state was saved. */
		slurmdb_cluster_rec_t *cluster;
		ListIterator itr = list_iterator_create(
			ret_fed->cluster_list);
		while ((cluster = list_next(itr))) {
			slurm_persist_conn_destroy(cluster->fed.recv);
			cluster->fed.recv = NULL;
			slurm_persist_conn_destroy(cluster->fed.send);
			cluster->fed.send = NULL;
		}
		list_iterator_destroy(itr);
	}

	/* Load in fed_job_list and transfer objects to actual fed_job_list only
	 * if there is an actual job for the job */
	if ((tmp_list = _load_fed_job_list(buffer, ver))) {
		fed_job_info_t *tmp_info;

		slurm_mutex_lock(&fed_job_list_mutex);
		while ((tmp_info = list_pop(tmp_list))) {
			if (find_job_record(tmp_info->job_id))
				list_append(fed_job_list, tmp_info);
			else
				_destroy_fed_job_info(tmp_info);
		}
		slurm_mutex_unlock(&fed_job_list_mutex);

	}
	FREE_NULL_LIST(tmp_list);

	free_buf(buffer);

	return ret_fed;

unpack_error:
	free_buf(buffer);

	return NULL;
}

/*
 * Returns federated job id (<local id> + <cluster id>.
 * Bits  0-25: Local job id
 * Bits 26-31: Cluster id
 */
extern uint32_t fed_mgr_get_job_id(uint32_t orig)
{
	if (!fed_mgr_cluster_rec)
		return orig;
	return orig + (fed_mgr_cluster_rec->fed.id << FED_MGR_CLUSTER_ID_BEGIN);
}

/*
 * Returns the local job id from a federated job id.
 */
extern uint32_t fed_mgr_get_local_id(uint32_t id)
{
	return id & MAX_JOB_ID;
}

/*
 * Returns the cluster id from a federated job id.
 */
extern uint32_t fed_mgr_get_cluster_id(uint32_t id)
{
	return id >> FED_MGR_CLUSTER_ID_BEGIN;
}

extern int fed_mgr_add_sibling_conn(slurm_persist_conn_t *persist_conn,
				    char **out_buffer)
{
	slurmdb_cluster_rec_t *cluster = NULL;
	slurmctld_lock_t fed_read_lock = {
		NO_LOCK, NO_LOCK, NO_LOCK, NO_LOCK, READ_LOCK };
	int rc = SLURM_SUCCESS;

	lock_slurmctld(fed_read_lock);

	if (!fed_mgr_fed_rec) {
		unlock_slurmctld(fed_read_lock);
		*out_buffer = xstrdup_printf(
			"no fed_mgr_fed_rec on cluster %s yet.",
			slurmctld_conf.cluster_name);
		/* This really isn't an error.  If the cluster doesn't know it
		 * is in a federation this could happen on the initial
		 * connection from a sibling that found out about the addition
		 * before I did.
		 */
		debug("%s: %s", __func__, *out_buffer);
		/* The other side needs to see this as an error though or the
		 * connection won't be completely established.
		 */
		return SLURM_ERROR;
	}

	if (!fed_mgr_cluster_rec) {
		unlock_slurmctld(fed_read_lock);
		*out_buffer = xstrdup_printf(
			"no fed_mgr_cluster_rec on cluster %s?  "
			"This should never happen",
			slurmctld_conf.cluster_name);
		error("%s: %s", __func__, *out_buffer);
		return SLURM_ERROR;
	}

	if (!(cluster =
	      fed_mgr_get_cluster_by_name(persist_conn->cluster_name))) {
		unlock_slurmctld(fed_read_lock);
		*out_buffer = xstrdup_printf(
			"%s isn't a known sibling of ours, but tried to connect to cluster %s federation %s",
			persist_conn->cluster_name, slurmctld_conf.cluster_name,
			fed_mgr_fed_rec->name);
		error("%s: %s", __func__, *out_buffer);
		return SLURM_ERROR;
	}

	persist_conn->callback_fini = _persist_callback_fini;
	persist_conn->flags |= PERSIST_FLAG_ALREADY_INITED;

	slurm_mutex_lock(&cluster->lock);
	cluster->control_port = persist_conn->rem_port;
	xfree(cluster->control_host);
	cluster->control_host = xstrdup(persist_conn->rem_host);

	/* If this pointer exists it will be handled by the persist_conn code,
	 * don't free
	 */
	//slurm_persist_conn_destroy(cluster->fed.recv);

	cluster->fed.recv = persist_conn;

	slurm_mutex_unlock(&cluster->lock);

	unlock_slurmctld(fed_read_lock);

	if (rc == SLURM_SUCCESS &&
	    (rc = slurm_persist_conn_recv_thread_init(
		    persist_conn, -1, persist_conn)
	     != SLURM_SUCCESS)) {
		*out_buffer = xstrdup_printf(
			"Couldn't connect back to %s for some reason",
			persist_conn->cluster_name);
		error("%s: %s", __func__, *out_buffer);
	}

	if (rc == SLURM_SUCCESS)
		_q_send_job_sync(cluster->name);

	return rc;
}

/*
 * Convert comma separated list of cluster names to bitmap of cluster ids.
 */
static int _validate_cluster_names(char *clusters, uint64_t *cluster_bitmap)
{
	int rc = SLURM_SUCCESS;
	uint64_t cluster_ids = 0;
	List cluster_names;

	xassert(clusters);

	if (!xstrcasecmp(clusters, "all")) {
		cluster_ids = _get_all_sibling_bits();
		goto end_it;
	}

	cluster_names = list_create(slurm_destroy_char);
	if (slurm_addto_char_list(cluster_names, clusters)) {
		ListIterator itr = list_iterator_create(cluster_names);
		char *cluster_name;
		slurmdb_cluster_rec_t *sibling;

		while ((cluster_name = list_next(itr))) {
			if (!(sibling =
			     fed_mgr_get_cluster_by_name(cluster_name))) {
				error("didn't find requested cluster name %s in list of federated clusters",
				      cluster_name);
				rc = SLURM_ERROR;
				break;
			}

			cluster_ids |= FED_SIBLING_BIT(sibling->fed.id);
		}
		list_iterator_destroy(itr);
	}
	FREE_NULL_LIST(cluster_names);

end_it:
	if (cluster_bitmap)
		*cluster_bitmap = cluster_ids;

	return rc;
}

/* Update remote sibling job's viable_siblings bitmaps.
 *
 * IN job_id      - job_id of job to update.
 * IN job_specs   - job_specs to update job_id with.
 * IN viable_sibs - viable siblings bitmap to send to sibling jobs.
 * IN update_sibs - bitmap of siblings to update.
 */
extern int fed_mgr_update_job(uint32_t job_id, job_desc_msg_t *job_specs,
			      uint64_t update_sibs, uid_t uid)
{
	ListIterator sib_itr;
	slurmdb_cluster_rec_t *sibling;
	fed_job_info_t *job_info;

	if (!(job_info = _find_fed_job_info(job_id))) {
		error("Didn't find job %d in fed_job_list", job_id);
		return SLURM_ERROR;
	}

	sib_itr = list_iterator_create(fed_mgr_fed_rec->cluster_list);
	while ((sibling = list_next(sib_itr))) {
		/* Local is handled outside */
		if (sibling == fed_mgr_cluster_rec)
			continue;

		if (!(update_sibs & FED_SIBLING_BIT(sibling->fed.id)))
			continue;

		if (_persist_update_job(sibling, job_id, job_specs, uid)) {
			error("failed to update sibling job on sibling %s",
			      sibling->name);
			continue;
		}

		job_info->updating_sibs[sibling->fed.id]++;
		job_info->updating_time[sibling->fed.id] = time(NULL);
	}
	list_iterator_destroy(sib_itr);

	return SLURM_SUCCESS;
}

/*
 * Submit sibling jobs to designated siblings.
 *
 * Will update job_desc->fed_siblings_active with the successful submissions.
 * Will not send to siblings if they are in
 * job_desc->fed_details->siblings_active.
 *
 * IN job_desc - job_desc containing job_id and fed_siblings_viable of job to be
 * 	submitted.
 * IN msg - contains the original job_desc buffer to send to the siblings.
 * IN alloc_only - true if just an allocation. false if a batch job.
 * IN dest_sibs - bitmap of viable siblings to submit to.
 * RET returns SLURM_SUCCESS if all siblings recieved the job sucessfully or
 * 	SLURM_ERROR if any siblings failed to receive the job. If a sibling
 * 	fails, then the sucessful siblings will be updated with the correct
 * 	sibling bitmap.
 */
static int _submit_sibling_jobs(job_desc_msg_t *job_desc, slurm_msg_t *msg,
				bool alloc_only, uint64_t dest_sibs)
{
	int ret_rc = SLURM_SUCCESS;
	ListIterator sib_itr;
	sib_msg_t sib_msg = {0};
	slurmdb_cluster_rec_t *sibling = NULL;
        slurm_msg_t req_msg;

	xassert(job_desc);
	xassert(msg);

	sib_msg.data_buffer  = msg->buffer;
	sib_msg.data_offset  = msg->body_offset;
	sib_msg.data_type    = msg->msg_type;
	sib_msg.data_version = msg->protocol_version;
	sib_msg.fed_siblings = job_desc->fed_siblings_viable;
	sib_msg.job_id       = job_desc->job_id;
	sib_msg.resp_host    = job_desc->resp_host;

	slurm_msg_t_init(&req_msg);
	req_msg.msg_type = REQUEST_SIB_MSG;
	req_msg.data     = &sib_msg;

	sib_itr = list_iterator_create(fed_mgr_fed_rec->cluster_list);
	while ((sibling = list_next(sib_itr))) {
		int rc;
		if (sibling == fed_mgr_cluster_rec)
			continue;

		/* Only send to specific siblings */
		if (!(dest_sibs & FED_SIBLING_BIT(sibling->fed.id)))
			continue;

		/* skip sibling if the sibling already has a job */
		if (job_desc->fed_siblings_active &
		    FED_SIBLING_BIT(sibling->fed.id))
			continue;

		if (alloc_only)
			sib_msg.sib_msg_type = FED_JOB_SUBMIT_INT;
		else
			sib_msg.sib_msg_type = FED_JOB_SUBMIT_BATCH;

		if (!(rc = _queue_rpc(sibling, &req_msg, 0, false)))
			job_desc->fed_siblings_active |=
				FED_SIBLING_BIT(sibling->fed.id);
		ret_rc |= rc;
	}
	list_iterator_destroy(sib_itr);

	return ret_rc;
}

/*
 * Prepare and submit new sibling jobs built from an existing job.
 *
 * IN job_ptr - job to submit to remote siblings.
 * IN dest_sibs - bitmap of viable siblings to submit to.
 */
static int _prepare_submit_siblings(struct job_record *job_ptr,
				    uint64_t dest_sibs)
{
	int rc = SLURM_SUCCESS;
	uint32_t origin_id;
	job_desc_msg_t *job_desc;
	Buf buffer;
	slurm_msg_t msg;

	xassert(job_ptr);
	xassert(job_ptr->details);

	if (!_is_fed_job(job_ptr, &origin_id))
		return SLURM_SUCCESS;

	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
		info("submitting new siblings for job %d", job_ptr->job_id);

	if (!(job_desc = copy_job_record_to_job_desc(job_ptr)))
		return SLURM_ERROR;

	/* have to pack job_desc into a buffer */
	slurm_msg_t_init(&msg);
	msg.msg_type         = REQUEST_RESOURCE_ALLOCATION;
	msg.data             = job_desc;
	msg.protocol_version = SLURM_PROTOCOL_VERSION;

	buffer               = init_buf(BUF_SIZE);
	pack_msg(&msg, buffer);
	msg.buffer           = buffer;

	if (_submit_sibling_jobs(job_desc, &msg, false, dest_sibs))
		error("Failed to submit fed job to siblings");

	/* mark this cluster as an active sibling */
	if (job_desc->fed_siblings_viable &
	    FED_SIBLING_BIT(fed_mgr_cluster_rec->fed.id))
		job_desc->fed_siblings_active |=
			FED_SIBLING_BIT(fed_mgr_cluster_rec->fed.id);

	/* Add new active jobs to siblings_active bitmap */
	job_ptr->fed_details->siblings_active |= job_desc->fed_siblings_active;
	update_job_fed_details(job_ptr);

	free_buf(buffer);
	/* free the environment since all strings are stored in one
	 * xmalloced buffer */
	if (job_desc->environment) {
		xfree(job_desc->environment[0]);
		xfree(job_desc->environment);
		job_desc->env_size = 0;
	}
	slurm_free_job_desc_msg(job_desc);

	return rc;
}

static uint64_t _get_all_sibling_bits()
{
	ListIterator itr;
	slurmdb_cluster_rec_t *cluster;
	uint64_t sib_bits = 0;

	if (!fed_mgr_fed_rec || !fed_mgr_fed_rec->cluster_list)
		goto fini;

	itr = list_iterator_create(fed_mgr_fed_rec->cluster_list);
	while ((cluster = list_next(itr))) {
		sib_bits |= FED_SIBLING_BIT(cluster->fed.id);
	}
	list_iterator_destroy(itr);

fini:
	return sib_bits;
}

static int _remove_inactive_sibs(void *object, void *arg)
{
	slurmdb_cluster_rec_t *sibling = (slurmdb_cluster_rec_t *)object;
	uint64_t *viable_sibs = (uint64_t *)arg;
	uint32_t cluster_state = sibling->fed.state;
	int  base_state  = (cluster_state & CLUSTER_FED_STATE_BASE);
	bool drain_flag  = (cluster_state & CLUSTER_FED_STATE_DRAIN);

	if (drain_flag ||
	    (base_state == CLUSTER_FED_STATE_INACTIVE))
		*viable_sibs &= ~(FED_SIBLING_BIT(sibling->fed.id));

	return SLURM_SUCCESS;
}

static uint64_t _get_viable_sibs(char *req_clusters, uint64_t feature_sibs)
{
	uint64_t viable_sibs = 0;

	if (req_clusters)
		_validate_cluster_names(req_clusters, &viable_sibs);
	if (!viable_sibs)
		/* viable sibs could be empty if req_clusters was cleared. */
		viable_sibs = _get_all_sibling_bits();
	if (feature_sibs)
		viable_sibs &= feature_sibs;

	/* filter out clusters that are inactive or draining */
	list_for_each(fed_mgr_fed_rec->cluster_list, _remove_inactive_sibs,
		      &viable_sibs);

	return viable_sibs;
}

static void _add_remove_sibling_jobs(struct job_record *job_ptr)
{
	fed_job_info_t *job_info;
	uint32_t origin_id = 0;
	uint64_t new_sibs = 0, old_sibs = 0, add_sibs = 0,
		 rem_sibs = 0, feature_sibs = 0;

	xassert(job_ptr);

	origin_id = fed_mgr_get_cluster_id(job_ptr->job_id);

	/* if job is not pending then remove removed siblings and add
	 * new siblings. */
	old_sibs = job_ptr->fed_details->siblings_active;

	_validate_cluster_features(job_ptr->details->cluster_features,
				   &feature_sibs);

	new_sibs = _get_viable_sibs(job_ptr->clusters, feature_sibs);
	job_ptr->fed_details->siblings_viable = new_sibs;

	add_sibs =  new_sibs & ~old_sibs;
	rem_sibs = ~new_sibs &  old_sibs;

	if (rem_sibs) {
		time_t now = time(NULL);
		_revoke_sibling_jobs(job_ptr->job_id,
				     fed_mgr_cluster_rec->fed.id,
				     rem_sibs, now);
		if (fed_mgr_is_origin_job(job_ptr) &&
		    (rem_sibs & FED_SIBLING_BIT(origin_id))) {
			fed_mgr_job_revoke(job_ptr, false, 0, now);
		}

		job_ptr->fed_details->siblings_active &= ~rem_sibs;
	}

	/* Don't submit new sibilings if the job is held */
	if (job_ptr->priority != 0 && add_sibs)
		_prepare_submit_siblings(
				job_ptr,
				job_ptr->fed_details->siblings_viable);

	/* unrevoke the origin job */
	if (fed_mgr_is_origin_job(job_ptr) &&
	    (add_sibs & FED_SIBLING_BIT(origin_id)))
		job_ptr->job_state &= ~JOB_REVOKED;

	/* Can't have the mutex while calling fed_mgr_job_revoke because it will
	 * lock the mutex as well. */
	slurm_mutex_lock(&fed_job_list_mutex);
	if ((job_info = _find_fed_job_info(job_ptr->job_id))) {
		job_info->siblings_viable =
			job_ptr->fed_details->siblings_viable;
		job_info->siblings_active =
			job_ptr->fed_details->siblings_active;
	}
	slurm_mutex_unlock(&fed_job_list_mutex);

	/* Update where sibling jobs are running */
	update_job_fed_details(job_ptr);
}

static bool _job_has_pending_updates(fed_job_info_t *job_info)
{
	int i;
	xassert(job_info);
	static const int UPDATE_DELAY = 60;
	time_t now = time(NULL);

	for (i = 1; i <= MAX_FED_CLUSTERS; i++) {
		if (job_info->updating_sibs[i]) {
		    if (job_info->updating_time[i] > (now - UPDATE_DELAY)) {
			if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
				info("job %u is waiting for %d update responses from cluster id %d",
				     job_info->job_id,
				     job_info->updating_sibs[i], i);
			return true;
		    } else {
			if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
				info("job %u is had pending updates (%d) for cluster id %d, but haven't heard back from it for %ld seconds. Clearing the cluster's updating state",
				     job_info->job_id,
				     job_info->updating_sibs[i],
				     i,
				     now - job_info->updating_time[i]);
			job_info->updating_sibs[i] = 0;
		    }
		}

	}

	return false;
}

/*
 * Validate requested job cluster features against each cluster's features.
 *
 * IN  spec_features  - cluster features that the job requested.
 * OUT cluster_bitmap - bitmap of clusters that have matching features.
 * RET SLURM_ERROR if no cluster has any of the requested features,
 *     SLURM_SUCESS otherwise.
 */
static int _validate_cluster_features(char *spec_features,
				      uint64_t *cluster_bitmap)
{
	int rc = SLURM_SUCCESS;
	uint64_t feature_sibs = 0;
	char *feature = NULL;
	slurmdb_cluster_rec_t *sib;
	List req_features;
	ListIterator feature_itr, sib_itr;

	if (!spec_features || !fed_mgr_fed_rec) {
		if (cluster_bitmap)
			*cluster_bitmap = feature_sibs;
		return rc;
	}
	req_features = list_create(slurm_destroy_char);
	slurm_addto_char_list(req_features, spec_features);

	feature_itr = list_iterator_create(req_features);
	sib_itr = list_iterator_create(fed_mgr_fed_rec->cluster_list);

	while ((feature = list_next(feature_itr))) {
		bool found = false;
		while ((sib = list_next(sib_itr))) {
			if (sib->fed.feature_list &&
			    list_find_first(sib->fed.feature_list,
					    slurm_find_char_in_list, feature)) {
				feature_sibs |= FED_SIBLING_BIT(sib->fed.id);
				found = true;
			}
		}

		if (!found) {
			error("didn't find at least one cluster with the feature '%s'",
			      feature);
			rc = SLURM_ERROR;
			goto end_features;
		}
		list_iterator_reset(sib_itr);
	}
end_features:
	list_iterator_destroy(sib_itr);
	list_iterator_destroy(feature_itr);
	FREE_NULL_LIST(req_features);

	if (cluster_bitmap)
		*cluster_bitmap = feature_sibs;

	return rc;
}

/* Determine how to submit a federated a job.
 *
 * First tries to find a cluster that can start the job now. If a cluster can
 * start the job now, then a sibling job is submitted to that cluster. If no
 * cluster can start the job now, then siblings jobs are submitted to each
 * sibling.
 *
 * Does its own locking (job and fed). Doesn't have a job write lock when
 * communicating with siblings to prevent blocking on sibling communications.
 *
 * IN msg - msg that contains packed job_desc msg to send to siblings.
 * IN job_desc - original job_desc msg.
 * IN alloc_only - true if requesting just an allocation (srun/salloc).
 * IN uid - uid of user requesting allocation.
 * IN protocol_version - version of the code the caller is using
 * OUT job_id_ptr - job_id of allocated job
 * OUT alloc_code - error_code returned from job_allocate
 * OUT err_msg - error message returned if any
 * RET returns SLURM_SUCCESS if the allocation was successful, SLURM_ERROR
 * 	otherwise.
 */
extern int fed_mgr_job_allocate(slurm_msg_t *msg, job_desc_msg_t *job_desc,
				bool alloc_only, uid_t uid,
				uint16_t protocol_version,
				uint32_t *job_id_ptr, int *alloc_code,
				char **err_msg)
{
	uint64_t feature_sibs = 0;
	struct job_record *job_ptr = NULL;
	bool job_held = false;

	xassert(msg);
	xassert(job_desc);
	xassert(job_id_ptr);
	xassert(alloc_code);
	xassert(err_msg);

	if (job_desc->job_id != NO_VAL) {
		error("attempt by uid %u to set job_id to %u. "
		      "specifying a job_id is not allowed when in a federation",
		      uid, job_desc->job_id);
		*alloc_code = ESLURM_INVALID_JOB_ID;
		return SLURM_ERROR;
	}

	if (_validate_cluster_features(job_desc->cluster_features,
				       &feature_sibs)) {
		*alloc_code = ESLURM_INVALID_CLUSTER_FEATURE;
		return SLURM_ERROR;
	}

	if (job_desc->priority == 0)
		job_held = true;

	/* get job_id now. Can't submit job to get job_id as job_allocate will
	 * change the job_desc. */
	job_desc->job_id = get_next_job_id(false);

	/* Set viable siblings */
	job_desc->fed_siblings_viable = _get_viable_sibs(job_desc->clusters,
							 feature_sibs);

	/* ensure that fed_siblings_active is clear since this is a new job */
	job_desc->fed_siblings_active = 0;

	/* Submit local job first. Then submit to all siblings. If the local job
	 * fails, then don't worry about sending to the siblings. */
	*alloc_code = job_allocate(job_desc, job_desc->immediate, false, NULL,
				   alloc_only, uid, &job_ptr, err_msg,
				   protocol_version);

	if (!job_ptr || (*alloc_code && job_ptr->job_state == JOB_FAILED)) {
		/* There may be an rc but the job won't be failed. Will sit in
		 * qeueue */
		info("failed to submit federated job to local cluster");
		return SLURM_ERROR;
	}

	/* mark this cluster as an active sibling if it's in the viable list */
	if (job_desc->fed_siblings_viable &
	    FED_SIBLING_BIT(fed_mgr_cluster_rec->fed.id))
		job_desc->fed_siblings_active |=
			FED_SIBLING_BIT(fed_mgr_cluster_rec->fed.id);

	*job_id_ptr = job_ptr->job_id;

	if (job_held) {
		info("Submitted held federated job %u to %s(self)",
		     job_ptr->job_id, fed_mgr_cluster_rec->name);
	} else {
		info("Submitted %sfederated job %u to %s(self)",
		     (!(job_ptr->fed_details->siblings_viable &
			FED_SIBLING_BIT(fed_mgr_cluster_rec->fed.id)) ?
		      "tracking " : ""),
		     job_ptr->job_id, fed_mgr_cluster_rec->name);
	}

	/* Update job before submitting sibling jobs so that it will show the
	 * viable siblings and potentially active local job */
	job_ptr->fed_details->siblings_active = job_desc->fed_siblings_active;
	update_job_fed_details(job_ptr);

	if (!job_held && _submit_sibling_jobs(
				job_desc, msg, alloc_only,
				job_ptr->fed_details->siblings_viable))
		info("failed to submit sibling job to one or more siblings");

	job_ptr->fed_details->siblings_active = job_desc->fed_siblings_active;
	update_job_fed_details(job_ptr);

	/* Add record to fed job table */
	_add_fed_job_info(job_ptr);

	return SLURM_SUCCESS;
}

/* Tests if the job is a tracker only federated job.
 * Tracker only job: a job that shouldn't run on the local cluster but should be
 * kept around to facilitate communications for it's sibling jobs on other
 * clusters.
 */
extern bool fed_mgr_is_tracker_only_job(struct job_record *job_ptr)
{
	bool rc = false;
	uint32_t origin_id;

	xassert(job_ptr);

	if (!_is_fed_job(job_ptr, &origin_id))
		return rc;

	if (job_ptr->fed_details &&
	    (origin_id == fed_mgr_cluster_rec->fed.id) &&
	    job_ptr->fed_details->siblings_active &&
	    (!(job_ptr->fed_details->siblings_active &
	      FED_SIBLING_BIT(fed_mgr_cluster_rec->fed.id))))
		rc = true;

	if (job_ptr->fed_details &&
	    job_ptr->fed_details->cluster_lock &&
	    job_ptr->fed_details->cluster_lock != fed_mgr_cluster_rec->fed.id)
		rc = true;

	return rc;
}

/* Return the cluster name for the given cluster id.
 * Must xfree returned string
 */
extern char *fed_mgr_get_cluster_name(uint32_t id)
{
	slurmdb_cluster_rec_t *sibling;
	char *name = NULL;

	if ((sibling = fed_mgr_get_cluster_by_id(id))) {
		name = xstrdup(sibling->name);
	}

	return name;
}

static int _is_fed_job(struct job_record *job_ptr, uint32_t *origin_id)
{
	xassert(job_ptr);
	xassert(origin_id);

	if (!fed_mgr_cluster_rec)
		return false;

	if ((!job_ptr->fed_details) ||
	    (!(*origin_id = fed_mgr_get_cluster_id(job_ptr->job_id)))) {
		info("job %d not a federated job", job_ptr->job_id);
		return false;
	}

	return true;
}

/*
 * Attempt to grab the job's federation cluster lock so that the requesting
 * cluster can attempt to start to the job.
 *
 * IN job - job to lock
 * RET returns SLURM_SUCCESS if the lock was granted, SLURM_ERROR otherwise
 */
extern int fed_mgr_job_lock(struct job_record *job_ptr)
{
	int rc = SLURM_SUCCESS;
	uint32_t origin_id, cluster_id;

	xassert(job_ptr);

	if (!_is_fed_job(job_ptr, &origin_id))
		return SLURM_SUCCESS;

	cluster_id = fed_mgr_cluster_rec->fed.id;

	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
		info("attempting fed job lock on %d by cluster_id %d",
		     job_ptr->job_id, cluster_id);

	if (origin_id != fed_mgr_cluster_rec->fed.id) {
		slurmdb_cluster_rec_t *origin_cluster;
		if (!(origin_cluster = fed_mgr_get_cluster_by_id(origin_id))) {
			error("Unable to find origin cluster for job %d from origin id %d",
			      job_ptr->job_id, origin_id);
			return SLURM_ERROR;
		}

		if (!(rc = _persist_fed_job_lock(origin_cluster,
						 job_ptr->job_id, cluster_id,
						 true))) {
			job_ptr->fed_details->cluster_lock = cluster_id;
			fed_mgr_job_lock_set(job_ptr->job_id, cluster_id);
		}

		return rc;
	}

	/* origin cluster */
	rc = fed_mgr_job_lock_set(job_ptr->job_id, cluster_id);

	return rc;
}

extern int fed_mgr_job_lock_set(uint32_t job_id, uint32_t cluster_id)
{
	int rc = SLURM_SUCCESS;
	fed_job_info_t *job_info;

	slurm_mutex_lock(&fed_job_list_mutex);

	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
		info("%s: attempting to set fed job %d lock to %u",
		     __func__, job_id, cluster_id);

	if (!(job_info = _find_fed_job_info(job_id))) {
		error("Didn't find job %d in fed_job_list", job_id);
		rc = SLURM_ERROR;
	} else if (_job_has_pending_updates(job_info)) {
		if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
			info("%s: cluster %u can't get cluster lock for %u because it has pending updates",
			     __func__, cluster_id, job_id);
		rc = SLURM_ERROR;
	} else if (job_info->cluster_lock &&
		   job_info->cluster_lock != cluster_id) {
		if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
			info("%s: fed job %d already locked by cluster %d",
			     __func__, job_id, job_info->cluster_lock);
		rc = SLURM_ERROR;
	} else {
		if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
			info("%s: fed job %d locked by %u",
			     __func__, job_id, cluster_id);

		job_info->cluster_lock = cluster_id;
	}

	slurm_mutex_unlock(&fed_job_list_mutex);

	return rc;
}

extern bool fed_mgr_job_is_self_owned(struct job_record *job_ptr)
{
	if (!fed_mgr_cluster_rec || !job_ptr->fed_details ||
	    (job_ptr->fed_details->cluster_lock == fed_mgr_cluster_rec->fed.id))
		return true;

	return false;
}

extern bool fed_mgr_job_is_locked(struct job_record *job_ptr)
{
	if (!job_ptr->fed_details ||
	    job_ptr->fed_details->cluster_lock)
		return true;

	return false;
}

static void _q_sib_job_start(slurm_msg_t *msg)
{
	sib_msg_t *sib_msg = msg->data;
	fed_job_update_info_t *job_update_info;

	/* add todo to remove remote siblings if the origin job */
	job_update_info = xmalloc(sizeof(fed_job_update_info_t));
	job_update_info->type         = FED_JOB_START;
	job_update_info->job_id       = sib_msg->job_id;
	job_update_info->start_time   = sib_msg->start_time;
	job_update_info->cluster_lock = sib_msg->cluster_id;

	_append_job_update(job_update_info);
}

extern int fed_mgr_job_lock_unset(uint32_t job_id, uint32_t cluster_id)
{
	int rc = SLURM_SUCCESS;
	fed_job_info_t * job_info;

	slurm_mutex_lock(&fed_job_list_mutex);

	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
		info("%s: attempting to unlock fed job %d by cluster %u",
		     __func__, job_id, cluster_id);

	if (!(job_info = _find_fed_job_info(job_id))) {
		error("Didn't find job %d in fed_job_list", job_id);
		rc = SLURM_ERROR;
	} else if (job_info->cluster_lock &&
		   job_info->cluster_lock != cluster_id) {
		error("attempt to unlock sib job %d by cluster %d which doesn't have job lock",
		      job_id, cluster_id);
		rc = SLURM_ERROR;
	} else {
		if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
			info("%s: fed job %d unlocked by %u",
			     __func__, job_id, cluster_id);
		job_info->cluster_lock = 0;
	}

	slurm_mutex_unlock(&fed_job_list_mutex);

	return rc;
}

/*
 * Release the job's federation cluster lock so that other cluster's can try to
 * start the job.
 *
 * IN job        - job to unlock
 * RET returns SLURM_SUCCESS if the lock was released, SLURM_ERROR otherwise
 */
extern int fed_mgr_job_unlock(struct job_record *job_ptr)
{
	int rc = SLURM_SUCCESS;
	uint32_t origin_id, cluster_id;

	if (!_is_fed_job(job_ptr, &origin_id))
		return SLURM_SUCCESS;

	cluster_id = fed_mgr_cluster_rec->fed.id;

	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
		info("releasing fed job lock on %d by cluster_id %d",
		     job_ptr->job_id, cluster_id);

	if (origin_id != fed_mgr_cluster_rec->fed.id) {
		slurmdb_cluster_rec_t *origin_cluster;
		if (!(origin_cluster = fed_mgr_get_cluster_by_id(origin_id))) {
			error("Unable to find origin cluster for job %d from origin id %d",
			      job_ptr->job_id, origin_id);
			return SLURM_ERROR;
		}

		if (!(rc = _persist_fed_job_lock(origin_cluster, job_ptr->job_id,
					  cluster_id, false))) {
			job_ptr->fed_details->cluster_lock = 0;
			fed_mgr_job_lock_unset(job_ptr->job_id, cluster_id);
		}

		return rc;
	}

	/* Origin Cluster */
	rc = fed_mgr_job_lock_unset(job_ptr->job_id, cluster_id);

	return rc;
}

/*
 * Notify origin cluster that cluster_id started job.
 *
 * Cancels remaining sibling jobs.
 *
 * IN job_ptr    - job_ptr of job to unlock
 * IN start_time - start_time of the job.
 * RET returns SLURM_SUCCESS if the lock was released, SLURM_ERROR otherwise
 */
extern int fed_mgr_job_start(struct job_record *job_ptr, time_t start_time)
{
	int rc = SLURM_SUCCESS;
	uint32_t origin_id, cluster_id;
	fed_job_info_t *job_info;

	assert(job_ptr);

	if (!_is_fed_job(job_ptr, &origin_id))
		return SLURM_SUCCESS;

	cluster_id = fed_mgr_cluster_rec->fed.id;

	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
		info("start fed job %d by cluster_id %d",
		     job_ptr->job_id, cluster_id);

	if (origin_id != fed_mgr_cluster_rec->fed.id) {
		slurmdb_cluster_rec_t *origin_cluster;
		if (!(origin_cluster = fed_mgr_get_cluster_by_id(origin_id))) {
			error("Unable to find origin cluster for job %d from origin id %d",
			      job_ptr->job_id, origin_id);
			return SLURM_ERROR;
		}

		job_ptr->fed_details->siblings_active =
			FED_SIBLING_BIT(cluster_id);
		update_job_fed_details(job_ptr);

		return _persist_fed_job_start(origin_cluster, job_ptr->job_id,
					      cluster_id, job_ptr->start_time);
	}

	/* Origin Cluster: */
	slurm_mutex_lock(&fed_job_list_mutex);

	if (!(job_info = _find_fed_job_info(job_ptr->job_id))) {
		error("Didn't find job %d in fed_job_list", job_ptr->job_id);
		rc = SLURM_ERROR;
	} else if (!job_info->cluster_lock) {
		error("attempt to start sib job %u by cluster %u, but it's not locked",
		      job_info->job_id, cluster_id);
		rc = SLURM_ERROR;
	} else if (job_info->cluster_lock &&
		   (job_info->cluster_lock != cluster_id)) {
		error("attempt to start sib job %u by cluster %u, which doesn't have job lock",
		     job_info->job_id, cluster_id);
		rc = SLURM_ERROR;
	}

	if (!rc)
		_fed_job_start_revoke(job_info, job_ptr, start_time);

	slurm_mutex_unlock(&fed_job_list_mutex);


	return rc;
}

/*
 * Complete the federated job. If the job ran on a cluster other than the
 * origin_cluster then it notifies the origin cluster that the job finished.
 *
 * Tells the origin cluster to revoke the tracking job.
 *
 * IN job_ptr     - job_ptr of job to complete.
 * IN return_code - return code of job
 * IN start_time  - start time of the job that actually ran.
 * RET returns SLURM_SUCCESS if fed job was completed, SLURM_ERROR otherwise
 */
extern int fed_mgr_job_complete(struct job_record *job_ptr,
				uint32_t return_code, time_t start_time)
{
	uint32_t origin_id;

	if (job_ptr->bit_flags & SIB_JOB_FLUSH)
		return SLURM_SUCCESS;

	if (!_is_fed_job(job_ptr, &origin_id))
		return SLURM_SUCCESS;

	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
		info("complete fed job %d by cluster_id %d",
		     job_ptr->job_id, fed_mgr_cluster_rec->fed.id);

	if (origin_id == fed_mgr_cluster_rec->fed.id) {
		_revoke_sibling_jobs(job_ptr->job_id,
				     fed_mgr_cluster_rec->fed.id,
				     job_ptr->fed_details->siblings_active,
				     job_ptr->start_time);
		return SLURM_SUCCESS;
	}

	slurmdb_cluster_rec_t *conn = fed_mgr_get_cluster_by_id(origin_id);
	if (!conn) {
		error("Unable to find origin cluster for job %d from origin id %d",
		      job_ptr->job_id, origin_id);
		return SLURM_ERROR;
	}

	return _persist_fed_job_revoke(conn, job_ptr->job_id, return_code,
				       start_time);
}

/*
 * Revoke all sibling jobs.
 *
 * IN job_ptr - job to revoke sibling jobs from.
 * RET SLURM_SUCCESS on success, SLURM_ERROR otherwise.
 */
extern int fed_mgr_job_revoke_sibs(struct job_record *job_ptr)
{
	uint32_t origin_id;
	time_t now = time(NULL);

	if (!_is_fed_job(job_ptr, &origin_id))
		return SLURM_SUCCESS;

	if (origin_id != fed_mgr_cluster_rec->fed.id)
		return SLURM_SUCCESS;

	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
		info("revoke fed job %d's siblings", job_ptr->job_id);

	_revoke_sibling_jobs(job_ptr->job_id, fed_mgr_cluster_rec->fed.id,
			     job_ptr->fed_details->siblings_active, now);

	return SLURM_SUCCESS;
}

/*
 * Revokes the federated job.
 *
 * IN job_ptr      - job_ptr of job to revoke.
 * IN job_complete - whether the job is done or not. If completed then sets the
 * 	state to JOB_REVOKED | JOB_CANCELLED. JOB_REVOKED otherwise.
 * IN exit_code    - exit_code of job.
 * IN start_time   - start time of the job that actually ran.
 * RET returns SLURM_SUCCESS if fed job was completed, SLURM_ERROR otherwise
 */
extern int fed_mgr_job_revoke(struct job_record *job_ptr, bool job_complete,
			      uint32_t exit_code, time_t start_time)
{
	uint32_t origin_id;
	uint32_t state = JOB_REVOKED;

	if (IS_JOB_COMPLETED(job_ptr)) /* job already completed */
		return SLURM_SUCCESS;

	if (!_is_fed_job(job_ptr, &origin_id))
		return SLURM_SUCCESS;

	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
		info("revoking fed job %d (%s)",
		     job_ptr->job_id,
		     job_complete ? "REVOKED|CANCELLED" : "REVOKED");

	/* Check if the job exited with one of the configured requeue values. */
	job_ptr->exit_code = exit_code;
	if (job_hold_requeue(job_ptr)) {
		batch_requeue_fini(job_ptr);
		return SLURM_SUCCESS;
	}

	if (job_complete)
		state |= JOB_CANCELLED;

	job_ptr->job_state  = state;
	job_ptr->start_time = start_time;
	job_ptr->end_time   = start_time;
	job_ptr->state_reason = WAIT_NO_REASON;
	xfree(job_ptr->state_desc);
	job_completion_logger(job_ptr, false);

	/* remove JOB_REVOKED for completed jobs so that job shows completed on
	 * controller. */
	if (job_complete)
		job_ptr->job_state &= ~JOB_REVOKED;

	/* Don't remove the origin job */
	if (origin_id == fed_mgr_cluster_rec->fed.id)
		return SLURM_SUCCESS;

	/* Purge the revoked job -- remote only */
	purge_job_record(job_ptr->job_id);

	return SLURM_SUCCESS;
}

/* Convert cluster ids to cluster names.
 *
 * RET: return string of comma-separated cluster names.
 *      Must free returned string.
 */
extern char *fed_mgr_cluster_ids_to_names(uint64_t cluster_ids)
{
	int bit = 1;
	char *names = NULL;

	if (!fed_mgr_fed_rec || !fed_mgr_fed_rec->cluster_list)
		return names;

	while (cluster_ids) {
		if (cluster_ids & 1) {
			slurmdb_cluster_rec_t *sibling;
			if ((sibling = fed_mgr_get_cluster_by_id(bit))) {
				xstrfmtcat(names, "%s%s",
					   (names) ? "," : "", sibling->name);
			} else {
				error("Couldn't find a sibling cluster with id %d",
				      bit);
			}
		}

		cluster_ids >>= 1;
		bit++;
	}

	return names;
}

/*
 * Tests whether a federated job can be requeued.
 *
 * If called from the remote cluster (non-origin) then it will send a requeue
 * request to the origin to have the origin cancel this job. In this case, it
 * will return success and set the JOB_REQUEUE_FED flag and wait to be killed.
 *
 * If it is the origin job, it will also cancel a running remote job. New
 * federated sibling jobs will be submitted after the job has completed (e.g.
 * after epilog) in fed_mgr_job_requeue().
 *
 * IN job_ptr - job to requeue.
 * IN state   - the state of the requeue (e.g. JOB_RECONFIG_FAIL).
 * RET returns SLURM_SUCCESS if siblings submitted successfully, SLURM_ERROR
 * 	otherwise.
 */
extern int fed_mgr_job_requeue_test(struct job_record *job_ptr, uint32_t state)
{
	uint32_t origin_id;

	if (!_is_fed_job(job_ptr, &origin_id))
		return SLURM_SUCCESS;

	if (origin_id != fed_mgr_cluster_rec->fed.id) {
		slurmdb_cluster_rec_t *origin_cluster;
		if (!(origin_cluster = fed_mgr_get_cluster_by_id(origin_id))) {
			error("Unable to find origin cluster for job %d from origin id %d",
			      job_ptr->job_id, origin_id);
			return SLURM_ERROR;
		}

		if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
			info("requeueing fed job %d on origin cluster %d",
			     job_ptr->job_id, origin_id);

		_persist_fed_job_requeue(origin_cluster, job_ptr->job_id,
					 state);

		job_ptr->job_state |= JOB_REQUEUE_FED;

		return SLURM_SUCCESS;
	}

	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
		info("requeueing fed job on %d by cluster_id %d",
		     job_ptr->job_id, fed_mgr_cluster_rec->fed.id);

	/* If the job is currently running locally, then cancel the running job
	 * and set a flag that it's being requeued. Then when the epilog
	 * complete comes in submit the siblings to the other clusters.
	 * Have to check this after checking for origin else it won't get to the
	 * origin. */
	if (IS_JOB_RUNNING(job_ptr))
		return SLURM_SUCCESS;

	/* If a sibling job is running remotely, then cancel the remote job and
	 * wait till job finishes (e.g. after long epilog) and then resubmit the
	 * siblings in fed_mgr_job_requeue(). */
	if (IS_JOB_PENDING(job_ptr) && IS_JOB_REVOKED(job_ptr)) {
		slurmdb_cluster_rec_t *remote_cluster;
		if (!(remote_cluster =
		      fed_mgr_get_cluster_by_id(
					job_ptr->fed_details->cluster_lock))) {
			error("Unable to find remote cluster for job %d from cluster lock %d",
			      job_ptr->job_id,
			      job_ptr->fed_details->cluster_lock);
			return SLURM_ERROR;
		}

		if (_persist_fed_job_cancel(remote_cluster, job_ptr->job_id,
					    SIGKILL, KILL_FED_REQUEUE, 0)) {
			error("failed to kill/requeue fed job %d",
			      job_ptr->job_id);
		}
	}

	return SLURM_SUCCESS;
}

/*
 * Submits requeued sibling jobs.
 *
 * IN job_ptr - job to requeue.
 * RET returns SLURM_SUCCESS if siblings submitted successfully, SLURM_ERROR
 * 	otherwise.
 */
extern int fed_mgr_job_requeue(struct job_record *job_ptr)
{
	int rc = SLURM_SUCCESS;
	uint32_t origin_id;
	uint64_t feature_sibs = 0;
	fed_job_info_t *job_info;

	xassert(job_ptr);
	xassert(job_ptr->details);

	if (!_is_fed_job(job_ptr, &origin_id))
		return SLURM_SUCCESS;

	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
		info("requeueing fed job %d", job_ptr->job_id);

	/* clear where actual siblings were */
	job_ptr->fed_details->siblings_active = 0;

	/* don't submit siblings for jobs that are held */
	if (job_ptr->priority == 0) {
		job_ptr->job_state &= (~JOB_REQUEUE_FED);

		update_job_fed_details(job_ptr);

		/* clear cluster lock */
		job_ptr->fed_details->cluster_lock = 0;

		return SLURM_SUCCESS;
	}

	/* Don't worry about testing which clusters can start the job the
	 * soonest since they can't start the job for 120 seconds anyways. */

	/* Get new viable siblings since the job might just have one viable
	 * sibling listed if the sibling was the cluster that could start the
	 * job the soonest. */
	_validate_cluster_features(job_ptr->details->cluster_features,
				   &feature_sibs);
	job_ptr->fed_details->siblings_viable =
		_get_viable_sibs(job_ptr->clusters, feature_sibs);

	_prepare_submit_siblings(job_ptr,
				 job_ptr->fed_details->siblings_viable);

	/* clear cluster lock */
	job_ptr->fed_details->cluster_lock = 0;

	job_ptr->job_state &= (~JOB_REQUEUE_FED);
	job_ptr->job_state &= (~JOB_REVOKED);

	slurm_mutex_lock(&fed_job_list_mutex);
	if ((job_info = _find_fed_job_info(job_ptr->job_id))) {
		job_info->siblings_viable =
			job_ptr->fed_details->siblings_viable;
		job_info->siblings_active =
			job_ptr->fed_details->siblings_active;
	} else {
		error("%s: failed to find fed job info for fed job_id %d",
		      __func__, job_ptr->job_id);
	}
	slurm_mutex_unlock(&fed_job_list_mutex);

	return rc;
}

/* Cancel sibling jobs. Just send request to itself */
static int _cancel_sibling_jobs(struct job_record *job_ptr, uint16_t signal,
				uint16_t flags, uid_t uid)
{
	int id = 1;
	uint64_t tmp_sibs = job_ptr->fed_details->siblings_active;
	while (tmp_sibs) {
		if ((tmp_sibs & 1) &&
		    (id != fed_mgr_cluster_rec->fed.id)) {
			slurmdb_cluster_rec_t *cluster =
				fed_mgr_get_cluster_by_id(id);
			if (!cluster) {
				error("couldn't find cluster rec by id %d", id);
				goto next_job;
			}

			_persist_fed_job_cancel(cluster, job_ptr->job_id,
						signal, flags, uid);
		}

next_job:
		tmp_sibs >>= 1;
		id++;
	}

	return SLURM_SUCCESS;
}

/* Cancel sibling jobs of a federated job
 *
 * IN job_ptr - job to cancel
 * IN signal  - signal to send to job
 * IN flags   - KILL_.* flags
 * IN uid     - uid making request
 */
extern int fed_mgr_job_cancel(struct job_record *job_ptr, uint16_t signal,
			      uint16_t flags, uid_t uid)
{
	uint32_t origin_id;

	xassert(job_ptr);

	if (!_is_fed_job(job_ptr, &origin_id))
		return SLURM_SUCCESS;

	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
		info("cancel fed job %d by origin", job_ptr->job_id);

	_cancel_sibling_jobs(job_ptr, signal, flags, uid);

	return SLURM_SUCCESS;
}

extern int fed_mgr_is_origin_job(struct job_record *job_ptr)
{
	uint32_t origin_id;

	xassert(job_ptr);

	if (!_is_fed_job(job_ptr, &origin_id))
		return true;

	if (fed_mgr_cluster_rec->fed.id != origin_id)
		return false;

	return true;
}

/*
 * Update a job's required clusters.
 *
 * Results in siblings being removed and added.
 *
 * IN job_ptr      - job to update.
 * IN req_features - comma-separated list of cluster names.
 * RET return SLURM_SUCCESS on sucess, error code otherwise.
 */
extern int fed_mgr_update_job_clusters(struct job_record *job_ptr,
				       char *spec_clusters)
{
	int rc = SLURM_SUCCESS;
	uint32_t origin_id;

	xassert(job_ptr);
	xassert(spec_clusters);

	if (!_is_fed_job(job_ptr, &origin_id)) {
		info("sched: update_job: not a fed job");
		rc = SLURM_ERROR;
	} else if ((!IS_JOB_PENDING(job_ptr)) ||
		   job_ptr->fed_details->cluster_lock) {
		rc = ESLURM_JOB_NOT_PENDING;
	} else if (!fed_mgr_fed_rec) {
		info("sched: update_job: setting Clusters on a non-active federated cluster for job %u",
		     job_ptr->job_id);
		rc = ESLURM_JOB_NOT_FEDERATED;
	} else if (_validate_cluster_names(spec_clusters, NULL)) {
		info("sched: update_job: invalid Clusters for job %u: %s",
		     job_ptr->job_id, spec_clusters);
		rc = ESLURM_INVALID_CLUSTER_NAME;
	} else {
		xfree(job_ptr->clusters);
		if (spec_clusters[0] == '\0')
			info("sched: update_job: cleared Clusters for job %u",
			     job_ptr->job_id);
		else if (*spec_clusters)
			job_ptr->clusters =
				xstrdup(spec_clusters);

		if (fed_mgr_is_origin_job(job_ptr))
			_add_remove_sibling_jobs(job_ptr);
	}

	return rc;
}

/*
 * Update a job's cluster features.
 *
 * Results in siblings being removed and added.
 *
 * IN job_ptr      - job to update cluster features.
 * IN req_features - comma-separated list of feature names.
 * RET return SLURM_SUCCESS on sucess, error code otherwise.
 */
extern int fed_mgr_update_job_cluster_features(struct job_record *job_ptr,
					       char *req_features)
{
	int rc = SLURM_SUCCESS;
	uint32_t origin_id;

	xassert(job_ptr);
	xassert(req_features);

	if (!_is_fed_job(job_ptr, &origin_id)) {
		info("sched: update_job: not a fed job");
		rc = SLURM_ERROR;
	} else if ((!IS_JOB_PENDING(job_ptr)) ||
		   job_ptr->fed_details->cluster_lock) {
		rc = ESLURM_JOB_NOT_PENDING;
	} else if (!fed_mgr_fed_rec) {
		info("sched: update_job: setting ClusterFeatures on a non-active federated cluster for job %u",
		     job_ptr->job_id);
		rc = ESLURM_JOB_NOT_FEDERATED;
	} else if (_validate_cluster_features(req_features, NULL)) {
		info("sched: update_job: invalid ClusterFeatures for job %u",
		     job_ptr->job_id);
		rc = ESLURM_INVALID_CLUSTER_FEATURE;
	} else {
		xfree(job_ptr->details->cluster_features);
		if (req_features[0] == '\0')
			info("sched: update_job: cleared ClusterFeatures for job %u",
			     job_ptr->job_id);
		else if (*req_features)
			job_ptr->details->cluster_features =
				xstrdup(req_features);

		if (fed_mgr_is_origin_job(job_ptr))
			_add_remove_sibling_jobs(job_ptr);
	}

	return rc;
}

static int _reconcile_fed_job(void *x, void *arg)
{
	int i;
	bool found_job = false;
	struct job_record *job_ptr = (struct job_record *)x;
	reconcile_sib_t *rec_sib = (reconcile_sib_t *)arg;
	job_info_msg_t *remote_jobs_ptr = rec_sib->job_info_msg;
	uint32_t sibling_id   = rec_sib->sibling_id;
	uint64_t sibling_bit  = FED_SIBLING_BIT(sibling_id);
	char    *sibling_name = rec_sib->sibling_name;
	slurm_job_info_t *remote_job  = NULL;
	fed_job_info_t *job_info;

	xassert(job_ptr);
	xassert(remote_jobs_ptr);

	/* Only look at jobs that originate from the remote sibling and if the
	 * sibling could have the job */
	if (!job_ptr->fed_details ||
	    !job_ptr->details ||
	    (job_ptr->details->submit_time >= rec_sib->sync_time) ||
	    IS_JOB_COMPLETED(job_ptr) || IS_JOB_COMPLETING(job_ptr) ||
	    ((fed_mgr_get_cluster_id(job_ptr->job_id) != sibling_id) &&
	     (!fed_mgr_is_origin_job(job_ptr) ||
	      !(job_ptr->fed_details->siblings_viable & sibling_bit)))) {
		return SLURM_SUCCESS;
	}

	for (i = 0; i < remote_jobs_ptr->record_count; i++) {
		remote_job = &remote_jobs_ptr->job_array[i];
		if (job_ptr->job_id == remote_job->job_id) {
			found_job = true;
			break;
		}
	}

	if (fed_mgr_get_cluster_id(job_ptr->job_id) == sibling_id) {
		if (!found_job ||
		    (remote_job && IS_JOB_COMPLETED(remote_job))) {
			/* origin job is missing on remote sibling or is
			 * completed. Could have been removed from a clean
			 * start. */
			info("%s: origin job %d is missing (or completed) from origin %s. Killing this copy of the job",
			     __func__, job_ptr->job_id, sibling_name);
			job_ptr->bit_flags |= SIB_JOB_FLUSH;
			job_signal(job_ptr->job_id, SIGKILL, 0, 0, false);
		} else {
			info("%s: origin %s still has %d",
			     __func__, sibling_name, job_ptr->job_id);
		}
	} else if (!found_job) {
		info("%s: didn't find job %d on cluster %s",
		     __func__, job_ptr->job_id, sibling_name);

		/* Remove from active siblings */
		job_ptr->fed_details->siblings_active &= ~sibling_bit;

		if (!job_ptr->fed_details->cluster_lock) {
			/* If the origin job isn't locked, then submit a sibling
			 * to this cluster. */
			info("%s: %s is a viable sibling of job %d, attempting to submit new sibling job to the cluster.",
			     __func__, sibling_name, job_ptr->job_id);
			_prepare_submit_siblings(job_ptr, sibling_bit);
		} else if (job_ptr->fed_details->cluster_lock == sibling_id) {
			/* The origin thinks that the sibling was running the
			 * job. It could have completed while this cluster was
			 * down or the sibling removed it by clearing out jobs
			 * (e.g. slurmctld -c). */
			info("%s: origin job %d was running on sibling %s, but it's not there. Assuming that the job completed",
			     __func__, job_ptr->job_id, sibling_name);
			fed_mgr_job_revoke(job_ptr, true, 0,
					   job_ptr->start_time);
		} else {
			/* The origin job has a lock but it's not on the sibling
			 * being reconciled. The job could have been started by
			 * another cluster while the sibling was down. Or the
			 * original sibling job submission could have failed. Or
			 * the origin started the job on the different sibling
			 * before the sibling before the sibling went down and
			 * came back up (normal situation). */
			info("%s: origin job %d is currently locked by sibling %d, this is ok",
			     __func__, job_ptr->job_id,
			     job_ptr->fed_details->cluster_lock);
		}
	} else if (remote_job) {
		info("%s: job %d found on remote sibling %s state:%s",
		     __func__, job_ptr->job_id, sibling_name,
		     job_state_string(remote_job->job_state));

		if (job_ptr->fed_details->cluster_lock == sibling_id) {
			if (IS_JOB_COMPLETE(remote_job)) {
				info("%s: job %d on sibling %s is already completed, completing the origin job",
				     __func__, job_ptr->job_id, sibling_name);
				fed_mgr_job_revoke(job_ptr, true,
						   remote_job->exit_code,
						   job_ptr->start_time);
			} else if (!IS_JOB_RUNNING(remote_job)) {
				/* The job could be pending if it was requeued
				 * due to node failure */
				info("%s: job %d on sibling %s has job lock but job is not running (state:%s)",
				     __func__, job_ptr->job_id, sibling_name,
				     job_state_string(remote_job->job_state));
			}
		} else if (job_ptr->fed_details->cluster_lock) {
			/* The remote might have had a sibling job before it
			 * went away and the origin started another job while it
			 * was away. The remote job needs to be revoked. */
			info("%s: job %d found on sibling %s but job is locked by cluster id %d",
			     __func__, job_ptr->job_id, sibling_name,
			     job_ptr->fed_details->cluster_lock);

			if (IS_JOB_PENDING(remote_job)) {
				info("%s: job %d is on %s in a pending state but cluster %d has the lock on it -- revoking the remote sibling job",
				     __func__, job_ptr->job_id, sibling_name,
				     job_ptr->fed_details->cluster_lock);
				_revoke_sibling_jobs(
						job_ptr->job_id,
						fed_mgr_cluster_rec->fed.id,
						sibling_bit,
						job_ptr->start_time);
			} else {
				/* should this job get cancelled? Would have to
				 * check cluster_lock before cancelling it to
				 * make sure that it's not there. */
				info("%s: job %d has a lock on sibling id %d, but found a job on sibling %s.",
				     __func__, job_ptr->job_id,
				     job_ptr->fed_details->cluster_lock,
				     sibling_name);

				_revoke_sibling_jobs(
						job_ptr->job_id,
						fed_mgr_cluster_rec->fed.id,
						sibling_bit,
						job_ptr->start_time);
			}
		} else {
			if (!(job_ptr->fed_details->siblings_active &
			      sibling_bit)) {
				info("%s: job %d on sibling %s but it wasn't in the active list. Adding to active list.",
				     __func__, job_ptr->job_id, sibling_name);
				job_ptr->fed_details->siblings_active |=
					sibling_bit;
			} else if (IS_JOB_RUNNING(remote_job)) {
				info("%s: origin doesn't think that job %d should be running on sibling %s but it is. This shouldn't happen. Giving lock to sibling.",
				     __func__, job_ptr->job_id, sibling_name);
			}
			/* else all good */
		}
	}

	/* Update job_info with updated siblings */
	slurm_mutex_lock(&fed_job_list_mutex);
	if ((job_info = _find_fed_job_info(job_ptr->job_id))) {
		job_info->siblings_viable =
			job_ptr->fed_details->siblings_viable;
		job_info->siblings_active =
			job_ptr->fed_details->siblings_active;
	} else {
		error("%s: failed to find fed job info for fed job_id %d",
		      __func__, job_ptr->job_id);
	}
	slurm_mutex_unlock(&fed_job_list_mutex);

	return SLURM_SUCCESS;
}

/*
 * Sync jobs with the given sibling name.
 *
 * IN sib_name - name of the sibling to sync with.
 */
static int _sync_jobs(const char *sib_name, job_info_msg_t *job_info_msg,
		      time_t sync_time)
{
	reconcile_sib_t rec_sib = {0};
	slurmdb_cluster_rec_t *sib;

	if (!(sib = fed_mgr_get_cluster_by_name((char *)sib_name))) {
		error("Couldn't find sibling by name '%s'", sib_name);
		return SLURM_ERROR;
	}

	rec_sib.sibling_id   = sib->fed.id;
	rec_sib.sibling_name = sib->name;
	rec_sib.job_info_msg = job_info_msg;
	rec_sib.sync_time    = sync_time;

	list_for_each(job_list, _reconcile_fed_job, &rec_sib);

	return SLURM_SUCCESS;
}

/*
 * Remove active sibling from the given job.
 *
 * IN job_id   - job_id of job to remove active sibling from.
 * IN sib_name - name of sibling job to remove from active siblings.
 * RET SLURM_SUCCESS on sucess, error code on error.
 */
extern int fed_mgr_remove_active_sibling(uint32_t job_id, char *sib_name)
{
	uint32_t origin_id;
	struct job_record *job_ptr = NULL;
	slurmdb_cluster_rec_t *sibling;

	if (!(job_ptr = find_job_record(job_id)))
		return ESLURM_INVALID_JOB_ID;

	if (!_is_fed_job(job_ptr, &origin_id))
		return ESLURM_JOB_NOT_FEDERATED;

	if (job_ptr->fed_details->cluster_lock)
		return ESLURM_JOB_NOT_PENDING;

	if (!(sibling = fed_mgr_get_cluster_by_name(sib_name)))
		return ESLURM_INVALID_CLUSTER_NAME;

	if (job_ptr->fed_details->siblings_active &
	    FED_SIBLING_BIT(sibling->fed.id)) {
		time_t now = time(NULL);
		if (fed_mgr_cluster_rec == sibling)
			fed_mgr_job_revoke(job_ptr, false, 0, now);
		else
			_revoke_sibling_jobs(job_ptr->job_id,
					     fed_mgr_cluster_rec->fed.id,
					     FED_SIBLING_BIT(sibling->fed.id),
					     now);
		job_ptr->fed_details->siblings_active &=
			~(FED_SIBLING_BIT(sibling->fed.id));
		update_job_fed_details(job_ptr);
	}

	return SLURM_SUCCESS;
}

static int _q_sib_job_submission(slurm_msg_t *msg, bool interactive_job)
{
	fed_job_update_info_t *job_update_info = NULL;
	sib_msg_t *sib_msg            = msg->data;
	job_desc_msg_t *job_desc      = sib_msg->data;
	job_desc->job_id              = sib_msg->job_id;
	job_desc->fed_siblings_viable = sib_msg->fed_siblings;
	if (interactive_job)
		job_desc->resp_host = xstrdup(sib_msg->resp_host);

	/* NULL out the data pointer because we are storing the pointer on the
	 * fed job update queue to be handled later. */
	sib_msg->data = NULL;

	/* set protocol version to that of the client's version so that
	 * the job's start_protocol_version is that of the client's and
	 * not the calling controllers. */
	job_update_info = xmalloc(sizeof(fed_job_update_info_t));

	job_update_info->job_id           = job_desc->job_id;
	job_update_info->submit_cluster   = xstrdup(msg->conn->cluster_name);
	job_update_info->submit_desc      = job_desc;
	job_update_info->submit_proto_ver = msg->protocol_version;

	if (interactive_job)
		job_update_info->type     = FED_JOB_SUBMIT_INT;
	else
		job_update_info->type     = FED_JOB_SUBMIT_BATCH;

	_append_job_update(job_update_info);

	return SLURM_SUCCESS;
}

static int _q_sib_submit_response(slurm_msg_t *msg)
{
	int rc = SLURM_SUCCESS;
	sib_msg_t *sib_msg;
	fed_job_update_info_t *job_update_info = NULL;

	xassert(msg);
	xassert(msg->conn);

	sib_msg = msg->data;

	/* if failure then remove from active siblings */
	if (sib_msg && sib_msg->return_code) {
		if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
			info("%s: cluster %s failed to submit sibling job %d. Removing from active_sibs. (error:%d)",
			     __func__, msg->conn->cluster_name,
			     sib_msg->job_id,
			     sib_msg->return_code);

		job_update_info = xmalloc(sizeof(fed_job_update_info_t));
		job_update_info->job_id       = sib_msg->job_id;
		job_update_info->type         = FED_JOB_REMOVE_ACTIVE_SIB_BIT;
		job_update_info->siblings_str =
			xstrdup(msg->conn->cluster_name);
		_append_job_update(job_update_info);
	}

	return rc;
}

static int _q_sib_job_update(slurm_msg_t *msg, uint32_t uid)
{
	sib_msg_t *sib_msg = msg->data;
	job_desc_msg_t *job_desc = sib_msg->data;
	fed_job_update_info_t *job_update_info =
		xmalloc(sizeof(fed_job_update_info_t));

	/* NULL out the data pointer because we are storing the pointer on the
	 * fed job update queue to be handled later. */
	sib_msg->data = NULL;

	job_update_info->type           = FED_JOB_UPDATE;
	job_update_info->submit_desc    = job_desc;
	job_update_info->job_id         = sib_msg->job_id;
	job_update_info->uid            = uid;
	job_update_info->submit_cluster = xstrdup(msg->conn->cluster_name);

	_append_job_update(job_update_info);

	return SLURM_SUCCESS;
}

extern int _q_sib_job_cancel(slurm_msg_t *msg, uint32_t uid)
{
	int rc = SLURM_SUCCESS;
	uint32_t req_uid;
	sib_msg_t *sib_msg = msg->data;
	job_step_kill_msg_t *kill_msg = (job_step_kill_msg_t *)sib_msg->data;
	fed_job_update_info_t *job_update_info =
		xmalloc(sizeof(fed_job_update_info_t));

	/* NULL out the data pointer because we are storing the pointer on the
	 * fed job update queue to be handled later. */
	sib_msg->data = NULL;

	if (sib_msg->req_uid)
		req_uid = sib_msg->req_uid;
	else
		req_uid = uid;

	job_update_info->type     = FED_JOB_CANCEL;
	job_update_info->kill_msg = kill_msg;
	job_update_info->uid      = req_uid;

	_append_job_update(job_update_info);

	return rc;
}

static int _q_sib_job_complete(slurm_msg_t *msg)
{
	int rc = SLURM_SUCCESS;
	sib_msg_t *sib_msg = msg->data;
	fed_job_update_info_t *job_update_info =
		xmalloc(sizeof(fed_job_update_info_t));

	job_update_info->type        = FED_JOB_COMPLETE;
	job_update_info->job_id      = sib_msg->job_id;
	job_update_info->start_time  = sib_msg->start_time;
	job_update_info->return_code = sib_msg->return_code;

	_append_job_update(job_update_info);

	return rc;
}

static int _q_sib_job_update_response(slurm_msg_t *msg)
{
	int rc = SLURM_SUCCESS;
	sib_msg_t *sib_msg = msg->data;

	fed_job_update_info_t *job_update_info =
		xmalloc(sizeof(fed_job_update_info_t));

	job_update_info->type           = FED_JOB_UPDATE_RESPONSE;
	job_update_info->job_id         = sib_msg->job_id;
	job_update_info->return_code    = sib_msg->return_code;
	job_update_info->submit_cluster = xstrdup(msg->conn->cluster_name);

	_append_job_update(job_update_info);

	return rc;
}

static int _q_sib_job_requeue(slurm_msg_t *msg, uint32_t uid)
{
	int rc = SLURM_SUCCESS;
	sib_msg_t *sib_msg     = msg->data;
	requeue_msg_t *req_ptr = (requeue_msg_t *)sib_msg->data;
	fed_job_update_info_t *job_update_info =
		xmalloc(sizeof(fed_job_update_info_t));

	job_update_info->type   = FED_JOB_REQUEUE;
	job_update_info->job_id = req_ptr->job_id;
	job_update_info->state  = req_ptr->state;
	job_update_info->uid    = uid;

	_append_job_update(job_update_info);

	return rc;
}

static int _q_send_job_sync(char *sib_name)
{
	int rc = SLURM_SUCCESS;
	fed_job_update_info_t *job_update_info =
		xmalloc(sizeof(fed_job_update_info_t));

	job_update_info->type           = FED_SEND_JOB_SYNC;
	job_update_info->submit_cluster = xstrdup(sib_name);

	_append_job_update(job_update_info);

	return rc;
}

static int _q_sib_job_sync(slurm_msg_t *msg)
{
	int rc = SLURM_SUCCESS;
	sib_msg_t *sib_msg = msg->data;
	job_info_msg_t *job_info_msg = (job_info_msg_t *)sib_msg->data;
	fed_job_update_info_t *job_update_info =
		xmalloc(sizeof(fed_job_update_info_t));

	/* NULL out the data pointer because we are storing the pointer on the
	 * fed job update queue to be handled later. */
	sib_msg->data = NULL;

	job_update_info->type           = FED_JOB_SYNC;
	job_update_info->job_info_msg   = job_info_msg;
	job_update_info->start_time     = sib_msg->start_time;
	job_update_info->submit_cluster = xstrdup(msg->conn->cluster_name);

	_append_job_update(job_update_info);

	return rc;
}

extern int fed_mgr_q_sib_msg(slurm_msg_t *msg, uint32_t rpc_uid)
{
	sib_msg_t *sib_msg = msg->data;

	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
		info("%s: sib_msg_type:%s",
		     __func__, _job_update_type_str(sib_msg->sib_msg_type));

	switch (sib_msg->sib_msg_type) {
	case FED_JOB_CANCEL:
		_q_sib_job_cancel(msg, rpc_uid);
		break;
	case FED_JOB_COMPLETE:
		_q_sib_job_complete(msg);
		break;
	case FED_JOB_REQUEUE:
		_q_sib_job_requeue(msg, rpc_uid);
		break;
	case FED_JOB_START:
		_q_sib_job_start(msg);
		break;
	case FED_JOB_SUBMIT_BATCH:
		_q_sib_job_submission(msg, false);
		break;
	case FED_JOB_SUBMIT_INT:
		_q_sib_job_submission(msg, true);
		break;
	case FED_JOB_SUBMIT_RESP:
		_q_sib_submit_response(msg);
		break;
	case FED_JOB_SYNC:
		_q_sib_job_sync(msg);
		break;
	case FED_JOB_UPDATE:
		_q_sib_job_update(msg, rpc_uid);
		break;
	case FED_JOB_UPDATE_RESPONSE:
		_q_sib_job_update_response(msg);
		break;
	default:
		error("%s: invalid sib_msg_type: %d",
		      __func__, sib_msg->sib_msg_type);
		break;
	}

	return SLURM_SUCCESS;
}
