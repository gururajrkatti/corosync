/*
 * Copyright (c) 2009-2011 Red Hat, Inc.
 *
 * All rights reserved.
 *
 * Authors: Christine Caulfield (ccaulfie@redhat.com)
 *          Fabio M. Di Nitto   (fdinitto@redhat.com)
 *
 * This software licensed under BSD license, the text of which follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of the MontaVista Software, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <config.h>

#include <sys/types.h>
#ifdef HAVE_ALLOCA_H
#include <alloca.h>
#endif
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <sys/uio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <qb/qbipc_common.h>
#include <qb/qbdefs.h>
#include <qb/qbutil.h>

#include <corosync/corotypes.h>
#include <corosync/corodefs.h>
#include <corosync/cfg.h>
#include <corosync/list.h>
#include <corosync/lcr/lcr_comp.h>
#include <corosync/logsys.h>
#include <corosync/mar_gen.h>
#include <corosync/coroapi.h>
#include <corosync/engine/quorum.h>
#include <corosync/icmap.h>
#include <corosync/ipc_votequorum.h>

#define VOTEQUORUM_MAJOR_VERSION 7
#define VOTEQUORUM_MINOR_VERSION 0
#define VOTEQUORUM_PATCH_VERSION 0

/*
 * Silly default to prevent accidents!
 */
#define DEFAULT_EXPECTED   1024
#define DEFAULT_QDEV_POLL 10000
#define DEFAULT_LEAVE_TMO 10000
#define DEFAULT_LMS_WIN   10000

LOGSYS_DECLARE_SUBSYS ("VOTEQ");

enum quorum_message_req_types {
	MESSAGE_REQ_EXEC_VOTEQUORUM_NODEINFO  = 0,
	MESSAGE_REQ_EXEC_VOTEQUORUM_RECONFIGURE = 1,
};

#define NODE_FLAGS_BEENDOWN         1
#define NODE_FLAGS_QDISK            8
#define NODE_FLAGS_REMOVED         16
#define NODE_FLAGS_US              32

#define NODEID_US 0
#define NODEID_QDEVICE -1

typedef enum {
	NODESTATE_JOINING=1,
	NODESTATE_MEMBER,
	NODESTATE_DEAD,
	NODESTATE_LEAVING
} nodestate_t;

struct cluster_node {
	int flags;
	int node_id;
	unsigned int expected_votes;
	unsigned int votes;
	time_t join_time;
	nodestate_t state;
	unsigned long long int last_hello; /* Only used for quorum devices */
	struct list_head list;
};

static int quorum;
static int cluster_is_quorate;
static int first_trans = 1;
static unsigned int quorumdev_poll = DEFAULT_QDEV_POLL;
static unsigned int leaving_timeout = DEFAULT_LEAVE_TMO;

static uint8_t two_node = 0;
static uint8_t wait_for_all = 0;
static uint8_t wait_for_all_status = 0;
static uint8_t auto_tie_breaker = 0;
static int lowest_node_id = -1;
static uint8_t last_man_standing = 0;
static uint32_t last_man_standing_window = DEFAULT_LMS_WIN;
static int last_man_standing_timer_set = 0;
static corosync_timer_handle_t last_man_standing_timer;

static struct cluster_node *us;
static struct cluster_node *quorum_device = NULL;
static char quorum_device_name[VOTEQUORUM_MAX_QDISK_NAME_LEN];
static corosync_timer_handle_t quorum_device_timer;
static corosync_timer_handle_t leaving_timer;
static struct list_head cluster_members_list;
static struct corosync_api_v1 *corosync_api;
static struct list_head trackers_list;
static unsigned int quorum_members[PROCESSOR_COUNT_MAX+1];
static int quorum_members_entries = 0;
static struct memb_ring_id quorum_ringid;

#define max(a,b) (((a) > (b)) ? (a) : (b))
static struct cluster_node *find_node_by_nodeid(int nodeid);
static struct cluster_node *allocate_node(int nodeid);

#define list_iterate(v, head) \
	for (v = (head)->next; v != head; v = v->next)

struct quorum_pd {
	unsigned char track_flags;
	int tracking_enabled;
	uint64_t tracking_context;
	struct list_head list;
	void *conn;
};

/*
 * Service Interfaces required by service_message_handler struct
 */

static void votequorum_init(struct corosync_api_v1 *api,
			    quorum_set_quorate_fn_t report);

static void quorum_confchg_fn (
	enum totem_configuration_type configuration_type,
	const unsigned int *member_list, size_t member_list_entries,
	const unsigned int *left_list, size_t left_list_entries,
	const unsigned int *joined_list, size_t joined_list_entries,
	const struct memb_ring_id *ring_id);

static int votequorum_exec_init_fn (struct corosync_api_v1 *api);

static int quorum_lib_init_fn (void *conn);

static int quorum_lib_exit_fn (void *conn);

static void message_handler_req_exec_votequorum_nodeinfo (
	const void *message,
	unsigned int nodeid);

static void message_handler_req_exec_votequorum_reconfigure (
	const void *message,
	unsigned int nodeid);

static void message_handler_req_lib_votequorum_getinfo (void *conn,
							const void *message);

static void message_handler_req_lib_votequorum_setexpected (void *conn,
							    const void *message);

static void message_handler_req_lib_votequorum_setvotes (void *conn,
							 const void *message);

static void message_handler_req_lib_votequorum_qdisk_register (void *conn,
							       const void *message);

static void message_handler_req_lib_votequorum_qdisk_unregister (void *conn,
								 const void *message);

static void message_handler_req_lib_votequorum_qdisk_poll (void *conn,
							   const void *message);

static void message_handler_req_lib_votequorum_qdisk_getinfo (void *conn,
							      const void *message);

static void message_handler_req_lib_votequorum_leaving (void *conn,
							const void *message);
static void message_handler_req_lib_votequorum_trackstart (void *conn,
							   const void *message);
static void message_handler_req_lib_votequorum_trackstop (void *conn,
							  const void *message);

static int quorum_exec_send_nodeinfo(void);
static int quorum_exec_send_reconfigure(int param, int nodeid, int value);

static void exec_votequorum_nodeinfo_endian_convert (void *message);
static void exec_votequorum_reconfigure_endian_convert (void *message);

static void add_votequorum_config_notification(void);

static void recalculate_quorum(int allow_decrease, int by_current_nodes);

/*
 * Library Handler Definition
 */
static struct corosync_lib_handler quorum_lib_service[] =
{
	{ /* 0 */
		.lib_handler_fn		= message_handler_req_lib_votequorum_getinfo,
		.flow_control		= COROSYNC_LIB_FLOW_CONTROL_NOT_REQUIRED
	},
	{ /* 1 */
		.lib_handler_fn		= message_handler_req_lib_votequorum_setexpected,
		.flow_control		= COROSYNC_LIB_FLOW_CONTROL_NOT_REQUIRED
	},
	{ /* 2 */
		.lib_handler_fn		= message_handler_req_lib_votequorum_setvotes,
		.flow_control		= COROSYNC_LIB_FLOW_CONTROL_NOT_REQUIRED
	},
	{ /* 3 */
		.lib_handler_fn		= message_handler_req_lib_votequorum_qdisk_register,
		.flow_control		= COROSYNC_LIB_FLOW_CONTROL_NOT_REQUIRED
	},
	{ /* 4 */
		.lib_handler_fn		= message_handler_req_lib_votequorum_qdisk_unregister,
		.flow_control		= COROSYNC_LIB_FLOW_CONTROL_NOT_REQUIRED
	},
	{ /* 5 */
		.lib_handler_fn		= message_handler_req_lib_votequorum_qdisk_poll,
		.flow_control		= COROSYNC_LIB_FLOW_CONTROL_NOT_REQUIRED
	},
	{ /* 6 */
		.lib_handler_fn		= message_handler_req_lib_votequorum_qdisk_getinfo,
		.flow_control		= COROSYNC_LIB_FLOW_CONTROL_NOT_REQUIRED
	},
	{ /* 7 */
		.lib_handler_fn		= message_handler_req_lib_votequorum_leaving,
		.flow_control		= COROSYNC_LIB_FLOW_CONTROL_NOT_REQUIRED
	},
	{ /* 8 */
		.lib_handler_fn		= message_handler_req_lib_votequorum_trackstart,
		.flow_control		= COROSYNC_LIB_FLOW_CONTROL_NOT_REQUIRED
	},
	{ /* 9 */
		.lib_handler_fn		= message_handler_req_lib_votequorum_trackstop,
		.flow_control		= COROSYNC_LIB_FLOW_CONTROL_NOT_REQUIRED
	}
};

static struct corosync_exec_handler votequorum_exec_engine[] =
{
	{ /* 0 */
		.exec_handler_fn	= message_handler_req_exec_votequorum_nodeinfo,
		.exec_endian_convert_fn	= exec_votequorum_nodeinfo_endian_convert
	},
	{ /* 1 */
		.exec_handler_fn	= message_handler_req_exec_votequorum_reconfigure,
		.exec_endian_convert_fn	= exec_votequorum_reconfigure_endian_convert
	},
};

static quorum_set_quorate_fn_t set_quorum;

/*
 * lcrso object definition
 */
static struct quorum_services_api_ver1 votequorum_iface_ver0 = {
	.init				= votequorum_init
};

static struct corosync_service_engine quorum_service_handler = {
	.name					= "corosync votes quorum service v0.91",
	.id					= VOTEQUORUM_SERVICE,
	.private_data_size			= sizeof (struct quorum_pd),
	.allow_inquorate			= CS_LIB_ALLOW_INQUORATE,
	.flow_control				= COROSYNC_LIB_FLOW_CONTROL_REQUIRED,
	.lib_init_fn				= quorum_lib_init_fn,
	.lib_exit_fn				= quorum_lib_exit_fn,
	.lib_engine				= quorum_lib_service,
	.lib_engine_count			= sizeof (quorum_lib_service) / sizeof (struct corosync_lib_handler),
	.exec_init_fn				= votequorum_exec_init_fn,
	.exec_engine				= votequorum_exec_engine,
	.exec_engine_count			= sizeof (votequorum_exec_engine) / sizeof (struct corosync_exec_handler),
	.confchg_fn				= quorum_confchg_fn,
	.sync_mode				= CS_SYNC_V1
};

/*
 * Dynamic loader definition
 */
static struct corosync_service_engine *quorum_get_service_handler_ver0 (void);

static struct corosync_service_engine_iface_ver0 quorum_service_handler_iface = {
	.corosync_get_service_engine_ver0 = quorum_get_service_handler_ver0
};

static struct lcr_iface corosync_quorum_ver0[2] = {
	{
		.name				= "corosync_votequorum",
		.version			= 0,
		.versions_replace		= 0,
		.versions_replace_count		= 0,
		.dependencies			= 0,
		.dependency_count		= 0,
		.constructor			= NULL,
		.destructor			= NULL,
		.interfaces			= (void **)(void *)&votequorum_iface_ver0
	},
	{
		.name				= "corosync_votequorum_iface",
		.version			= 0,
		.versions_replace		= 0,
		.versions_replace_count		= 0,
		.dependencies			= 0,
		.dependency_count		= 0,
		.constructor			= NULL,
		.destructor			= NULL,
		.interfaces			= NULL
	}
};

static struct lcr_comp quorum_comp_ver0 = {
	.iface_count			= 2,
	.ifaces				= corosync_quorum_ver0
};


static struct corosync_service_engine *quorum_get_service_handler_ver0 (void)
{
	return (&quorum_service_handler);
}

#ifdef COROSYNC_SOLARIS
void corosync_lcr_component_register (void);
void corosync_lcr_component_register (void) {
#else
__attribute__ ((constructor)) static void corosync_lcr_component_register (void) {
#endif
	lcr_interfaces_set (&corosync_quorum_ver0[0], &votequorum_iface_ver0);
	lcr_interfaces_set (&corosync_quorum_ver0[1], &quorum_service_handler_iface);
	lcr_component_register (&quorum_comp_ver0);
}

static void votequorum_init(struct corosync_api_v1 *api,
			    quorum_set_quorate_fn_t report)
{
	ENTER();

	set_quorum = report;

	icmap_get_uint8("quorum.wait_for_all", &wait_for_all);
	icmap_get_uint8("quorum.auto_tie_breaker", &auto_tie_breaker);
	icmap_get_uint8("quorum.last_man_standing", &last_man_standing);
	icmap_get_uint32("quorum.last_man_standing_window", &last_man_standing_window);

	/*
	 * TODO: we need to know the lowest node-id in the cluster
	 * current lack of node list with node-id's requires us to see all nodes
	 * to determine which is the lowest.
	 */
	if (auto_tie_breaker) {
		wait_for_all = 1;
	}

	if (wait_for_all) {
		wait_for_all_status = 1;
	}

	/* Load the library-servicing part of this module */
	api->service_link_and_init(api, "corosync_votequorum_iface", 0);

	LEAVE();
}

struct req_exec_quorum_nodeinfo {
	struct qb_ipc_request_header header __attribute__((aligned(8)));
	unsigned int first_trans;
	unsigned int votes;
	unsigned int expected_votes;
	unsigned int major_version;	/* Not backwards compatible */
	unsigned int minor_version;	/* Backwards compatible */
	unsigned int patch_version;	/* Backwards/forwards compatible */
	unsigned int config_version;
	unsigned int flags;
	unsigned int wait_for_all_status;
	unsigned int quorate;
} __attribute__((packed));

/*
 * Parameters for RECONFIG command
 */
#define RECONFIG_PARAM_EXPECTED_VOTES 1
#define RECONFIG_PARAM_NODE_VOTES     2
#define RECONFIG_PARAM_LEAVING        3

struct req_exec_quorum_reconfigure {
	struct qb_ipc_request_header header __attribute__((aligned(8)));
	unsigned int param;
	unsigned int nodeid;
	unsigned int value;
};

static void read_quorum_config(void)
{
	int cluster_members = 0;
	struct list_head *tmp;

	ENTER();

	log_printf(LOGSYS_LEVEL_DEBUG, "Reading configuration\n");

	if (icmap_get_uint32("quorum.expected_votes", &us->expected_votes) != CS_OK) {
		us->expected_votes = DEFAULT_EXPECTED;
	}

	if (icmap_get_uint32("quorum.votes", &us->votes) != CS_OK) {
		us->votes = 1;
	}

	if (icmap_get_uint32("quorum.quorumdev_poll", &quorumdev_poll) != CS_OK) {
		quorumdev_poll = DEFAULT_QDEV_POLL;
	}

	if (icmap_get_uint32("quorum.leaving_timeout", &leaving_timeout) != CS_OK) {
		leaving_timeout = DEFAULT_LEAVE_TMO;
	}

	icmap_get_uint8("quorum.two_node", &two_node);

	/*
	 * two_node mode is invalid if there are more than 2 nodes in the cluster!
	 */
	list_iterate(tmp, &cluster_members_list) {
		cluster_members++;
        }

	if (two_node && cluster_members > 2) {
		log_printf(LOGSYS_LEVEL_WARNING, "quorum.two_node was set but there are more than 2 nodes in the cluster. It will be ignored.");
		two_node = 0;
	}

	LEAVE();
}

static int votequorum_exec_init_fn (struct corosync_api_v1 *api)
{
#ifdef COROSYNC_SOLARIS
	logsys_subsys_init();
#endif

	ENTER();

	corosync_api = api;

	list_init(&cluster_members_list);
	list_init(&trackers_list);

	/*
	 * Allocate a cluster_node for us
	 */
	us = allocate_node(corosync_api->totem_nodeid_get());
	if (!us) {
		LEAVE();
		return (1);
	}

	us->flags |= NODE_FLAGS_US;
	us->state = NODESTATE_MEMBER;
	us->expected_votes = DEFAULT_EXPECTED;
	us->votes = 1;
	time(&us->join_time);

	read_quorum_config();
	recalculate_quorum(0, 0);

	/*
	 * Listen for changes
	 */
	add_votequorum_config_notification();

	/*
	 * Start us off with one node
	 */
	quorum_exec_send_nodeinfo();

	LEAVE();

	return (0);
}

static int quorum_lib_exit_fn (void *conn)
{
	struct quorum_pd *quorum_pd = (struct quorum_pd *)corosync_api->ipc_private_data_get (conn);

	ENTER();

	if (quorum_pd->tracking_enabled) {
		list_del (&quorum_pd->list);
		list_init (&quorum_pd->list);
	}

	LEAVE();

	return (0);
}


static int send_quorum_notification(void *conn, uint64_t context)
{
	struct res_lib_votequorum_notification *res_lib_votequorum_notification;
	struct list_head *tmp;
	struct cluster_node *node;
	int cluster_members = 0;
	int i = 0;
	int size;
	char *buf;

	ENTER();

	list_iterate(tmp, &cluster_members_list) {
		node = list_entry(tmp, struct cluster_node, list);
		cluster_members++;
        }
	if (quorum_device) {
		cluster_members++;
	}

	size = sizeof(struct res_lib_votequorum_notification) + sizeof(struct votequorum_node) * cluster_members;
	buf = alloca(size);
	if (!buf) {
		LEAVE();
		return -1;
	}

	res_lib_votequorum_notification = (struct res_lib_votequorum_notification *)buf;
	res_lib_votequorum_notification->quorate = cluster_is_quorate;
	res_lib_votequorum_notification->node_list_entries = cluster_members;
	res_lib_votequorum_notification->context = context;
	list_iterate(tmp, &cluster_members_list) {
		node = list_entry(tmp, struct cluster_node, list);
		res_lib_votequorum_notification->node_list[i].nodeid = node->node_id;
		res_lib_votequorum_notification->node_list[i++].state = node->state;
        }
	if (quorum_device) {
		res_lib_votequorum_notification->node_list[i].nodeid = 0;
		res_lib_votequorum_notification->node_list[i++].state = quorum_device->state | 0x80;
	}
	res_lib_votequorum_notification->header.id = MESSAGE_RES_VOTEQUORUM_NOTIFICATION;
	res_lib_votequorum_notification->header.size = size;
	res_lib_votequorum_notification->header.error = CS_OK;

	/* Send it to all interested parties */
	if (conn) {
		int ret = corosync_api->ipc_dispatch_send(conn, buf, size);
		LEAVE();
		return ret;
	} else {
		struct quorum_pd *qpd;

		list_iterate(tmp, &trackers_list) {
			qpd = list_entry(tmp, struct quorum_pd, list);
			res_lib_votequorum_notification->context = qpd->tracking_context;
			corosync_api->ipc_dispatch_send(qpd->conn, buf, size);
		}
	}

	LEAVE();

	return 0;
}

static void send_expectedvotes_notification(void)
{
	struct res_lib_votequorum_expectedvotes_notification res_lib_votequorum_expectedvotes_notification;
	struct quorum_pd *qpd;
	struct list_head *tmp;

	ENTER();

	log_printf(LOGSYS_LEVEL_DEBUG, "Sending expected votes callback\n");

	res_lib_votequorum_expectedvotes_notification.header.id = MESSAGE_RES_VOTEQUORUM_EXPECTEDVOTES_NOTIFICATION;
	res_lib_votequorum_expectedvotes_notification.header.size = sizeof(res_lib_votequorum_expectedvotes_notification);
	res_lib_votequorum_expectedvotes_notification.header.error = CS_OK;
	res_lib_votequorum_expectedvotes_notification.expected_votes = us->expected_votes;

	list_iterate(tmp, &trackers_list) {
		qpd = list_entry(tmp, struct quorum_pd, list);
		res_lib_votequorum_expectedvotes_notification.context = qpd->tracking_context;
		corosync_api->ipc_dispatch_send(qpd->conn, &res_lib_votequorum_expectedvotes_notification,
						sizeof(struct res_lib_votequorum_expectedvotes_notification));
	}

	LEAVE();
}

static void get_lowest_node_id(void)
{
	struct cluster_node *node = NULL;
	struct list_head *tmp;

	ENTER();

	lowest_node_id = us->node_id;

	list_iterate(tmp, &cluster_members_list) {
		node = list_entry(tmp, struct cluster_node, list);
		if (node->node_id < lowest_node_id) {
			lowest_node_id = node->node_id;
		}
	}
	log_printf(LOGSYS_LEVEL_DEBUG, "lowest node id: %d us: %d\n", lowest_node_id, us->node_id);

	LEAVE();
}

static int check_low_node_id_partition(void)
{
	struct cluster_node *node = NULL;
	struct list_head *tmp;
	int found = 0;

	ENTER();

	list_iterate(tmp, &cluster_members_list) {
		node = list_entry(tmp, struct cluster_node, list);
		if (node->state == NODESTATE_MEMBER) {
			if (node->node_id == lowest_node_id) {
				found = 1;
			}
		}
	}

	LEAVE();
	return found;
}

static void set_quorate(int total_votes)
{
	int quorate;
	int quorum_change = 0;

	ENTER();

	/*
	 * wait for all nodes to show up before granting quorum
	 */

	if ((wait_for_all) && (wait_for_all_status)) {
		if (total_votes != us->expected_votes) {
			log_printf(LOGSYS_LEVEL_NOTICE,
				   "Waiting for all cluster members. "
				   "Current votes: %d expected_votes: %d\n",
				   total_votes, us->expected_votes);
			cluster_is_quorate = 0;
			return;
		}
		wait_for_all_status = 0;
		get_lowest_node_id();
	}

	if (quorum > total_votes) {
		quorate = 0;
	} else {
		quorate = 1;
	}

	if ((auto_tie_breaker) &&
	    (total_votes == (us->expected_votes / 2)) &&
	    (check_low_node_id_partition() == 1)) {
		quorate = 1;
	}

	if (cluster_is_quorate && !quorate) {
		quorum_change = 1;
		log_printf(LOGSYS_LEVEL_INFO, "quorum lost, blocking activity\n");
	}
	if (!cluster_is_quorate && quorate) {
		quorum_change = 1;
		log_printf(LOGSYS_LEVEL_INFO, "quorum regained, resuming activity\n");
	}

	cluster_is_quorate = quorate;

	if (wait_for_all) {
		if (quorate) {
			wait_for_all_status = 0;
		} else {
			wait_for_all_status = 1;
		}
	}

	if (quorum_change) {
		set_quorum(quorum_members, quorum_members_entries,
			   cluster_is_quorate, &quorum_ringid);
	}

	LEAVE();
}

static int calculate_quorum(int allow_decrease, int max_expected, unsigned int *ret_total_votes)
{
	struct list_head *nodelist;
	struct cluster_node *node;
	unsigned int total_votes = 0;
	unsigned int highest_expected = 0;
	unsigned int newquorum, q1, q2;
	unsigned int total_nodes = 0;

	ENTER();

	list_iterate(nodelist, &cluster_members_list) {
		node = list_entry(nodelist, struct cluster_node, list);

		log_printf(LOGSYS_LEVEL_DEBUG, "node %x state=%d, votes=%d, expected=%d\n",
			   node->node_id, node->state, node->votes, node->expected_votes);

		if (node->state == NODESTATE_MEMBER) {
			if (max_expected) {
				node->expected_votes = max_expected;
			} else {
				highest_expected = max(highest_expected, node->expected_votes);
			}
			total_votes += node->votes;
			total_nodes++;
		}
	}

	if (quorum_device && quorum_device->state == NODESTATE_MEMBER) {
		total_votes += quorum_device->votes;
	}

	if (max_expected > 0) {
		highest_expected = max_expected;
	}

	/*
	 * This quorum calculation is taken from the OpenVMS Cluster Systems
	 * manual, but, then, you guessed that didn't you
	 */
	q1 = (highest_expected + 2) / 2;
	q2 = (total_votes + 2) / 2;
	newquorum = max(q1, q2);

	/*
	 * Normally quorum never decreases but the system administrator can
	 * force it down by setting expected votes to a maximum value
	 */
	if (!allow_decrease) {
		newquorum = max(quorum, newquorum);
	}

	/*
	 * The special two_node mode allows each of the two nodes to retain
	 * quorum if the other fails.  Only one of the two should live past
	 * fencing (as both nodes try to fence each other in split-brain.)
	 * Also: if there are more than two nodes, force us inquorate to avoid
	 * any damage or confusion.
	 */
	if (two_node && total_nodes <= 2) {
		newquorum = 1;
	}

	if (ret_total_votes) {
		*ret_total_votes = total_votes;
	}

	LEAVE();
	return newquorum;
}

/* Recalculate cluster quorum, set quorate and notify changes */
static void recalculate_quorum(int allow_decrease, int by_current_nodes)
{
	unsigned int total_votes = 0;
	int cluster_members = 0;
	struct list_head *nodelist;
	struct cluster_node *node;

	ENTER();

	list_iterate(nodelist, &cluster_members_list) {
		node = list_entry(nodelist, struct cluster_node, list);
		if (node->state == NODESTATE_MEMBER) {
			if (by_current_nodes) {
				cluster_members++;
			}
			total_votes += node->votes;
		}
	}

	/*
	 * Keep expected_votes at the highest number of votes in the cluster
	 */
	log_printf(LOGSYS_LEVEL_DEBUG, "total_votes=%d, expected_votes=%d\n", total_votes, us->expected_votes);
	if (total_votes > us->expected_votes) {
		us->expected_votes = total_votes;
		send_expectedvotes_notification();
	}

	quorum = calculate_quorum(allow_decrease, cluster_members, &total_votes);
	set_quorate(total_votes);

	send_quorum_notification(NULL, 0L);

	LEAVE();
}

static void node_add_ordered(struct cluster_node *newnode)
{
	struct cluster_node *node = NULL;
	struct list_head *tmp;
	struct list_head *newlist = &newnode->list;

	ENTER();

	list_iterate(tmp, &cluster_members_list) {
		node = list_entry(tmp, struct cluster_node, list);
		if (newnode->node_id < node->node_id) {
			break;
		}
	}

	if (!node) {
		list_add(&newnode->list, &cluster_members_list);
	} else {
		newlist->prev = tmp->prev;
		newlist->next = tmp;
		tmp->prev->next = newlist;
		tmp->prev = newlist;
	}

	LEAVE();
}

static struct cluster_node *allocate_node(int nodeid)
{
	struct cluster_node *cl;

	ENTER();

	cl = malloc(sizeof(struct cluster_node));
	if (cl) {
		memset(cl, 0, sizeof(struct cluster_node));
		cl->node_id = nodeid;
		if (nodeid) {
			node_add_ordered(cl);
		}
	}

	LEAVE();

	return cl;
}

static struct cluster_node *find_node_by_nodeid(int nodeid)
{
	struct cluster_node *node;
	struct list_head *tmp;

	ENTER();

	if (nodeid == NODEID_US) {
		LEAVE();
		return us;
	}

	if (nodeid == NODEID_QDEVICE) {
		LEAVE();
		return quorum_device;
	}

	list_iterate(tmp, &cluster_members_list) {
		node = list_entry(tmp, struct cluster_node, list);
		if (node->node_id == nodeid) {
			LEAVE();
			return node;
		}
	}

	LEAVE();
	return NULL;
}


static int quorum_exec_send_nodeinfo()
{
	struct req_exec_quorum_nodeinfo req_exec_quorum_nodeinfo;
	struct iovec iov[1];
	int ret;

	ENTER();

	req_exec_quorum_nodeinfo.expected_votes = us->expected_votes;
	req_exec_quorum_nodeinfo.votes = us->votes;
	req_exec_quorum_nodeinfo.major_version = VOTEQUORUM_MAJOR_VERSION;
	req_exec_quorum_nodeinfo.minor_version = VOTEQUORUM_MINOR_VERSION;
	req_exec_quorum_nodeinfo.patch_version = VOTEQUORUM_PATCH_VERSION;
	req_exec_quorum_nodeinfo.flags = us->flags;
	req_exec_quorum_nodeinfo.first_trans = first_trans;
	req_exec_quorum_nodeinfo.wait_for_all_status = wait_for_all_status;
	req_exec_quorum_nodeinfo.quorate = cluster_is_quorate;

	req_exec_quorum_nodeinfo.header.id = SERVICE_ID_MAKE(VOTEQUORUM_SERVICE, MESSAGE_REQ_EXEC_VOTEQUORUM_NODEINFO);
	req_exec_quorum_nodeinfo.header.size = sizeof(req_exec_quorum_nodeinfo);

	iov[0].iov_base = (void *)&req_exec_quorum_nodeinfo;
	iov[0].iov_len = sizeof(req_exec_quorum_nodeinfo);

	ret = corosync_api->totem_mcast (iov, 1, TOTEM_AGREED);

	LEAVE();
	return ret;
}


static int quorum_exec_send_reconfigure(int param, int nodeid, int value)
{
	struct req_exec_quorum_reconfigure req_exec_quorum_reconfigure;
	struct iovec iov[1];
	int ret;

	ENTER();

	req_exec_quorum_reconfigure.param = param;
	req_exec_quorum_reconfigure.nodeid = nodeid;
	req_exec_quorum_reconfigure.value = value;

	req_exec_quorum_reconfigure.header.id = SERVICE_ID_MAKE(VOTEQUORUM_SERVICE, MESSAGE_REQ_EXEC_VOTEQUORUM_RECONFIGURE);
	req_exec_quorum_reconfigure.header.size = sizeof(req_exec_quorum_reconfigure);

	iov[0].iov_base = (void *)&req_exec_quorum_reconfigure;
	iov[0].iov_len = sizeof(req_exec_quorum_reconfigure);

	ret = corosync_api->totem_mcast (iov, 1, TOTEM_AGREED);

	LEAVE();
	return ret;
}

static void lms_timer_fn(void *arg)
{
	ENTER();

	last_man_standing_timer_set = 0;
	if (cluster_is_quorate) {
		recalculate_quorum(1,1);
	}

	LEAVE();
}

static void quorum_confchg_fn (
	enum totem_configuration_type configuration_type,
	const unsigned int *member_list, size_t member_list_entries,
	const unsigned int *left_list, size_t left_list_entries,
	const unsigned int *joined_list, size_t joined_list_entries,
	const struct memb_ring_id *ring_id)
{
	int i;
	int leaving = 0;
	struct cluster_node *node;

	ENTER();

	if (member_list_entries > 1) {
		first_trans = 0;
	}

	if (left_list_entries) {
		for (i = 0; i< left_list_entries; i++) {
			node = find_node_by_nodeid(left_list[i]);
			if (node) {
				if (node->state == NODESTATE_LEAVING) {
					leaving = 1;
				}
				node->state = NODESTATE_DEAD;
				node->flags |= NODE_FLAGS_BEENDOWN;
			}
		}
	}

	if (last_man_standing) {
		if (((member_list_entries >= quorum) && (left_list_entries)) ||
		    ((member_list_entries <= quorum) && (auto_tie_breaker) && (check_low_node_id_partition() == 1))) {
			if (last_man_standing_timer_set) {
				corosync_api->timer_delete(last_man_standing_timer);
				last_man_standing_timer_set = 0;
			}
			corosync_api->timer_add_duration((unsigned long long)last_man_standing_window*1000000, NULL, lms_timer_fn, &last_man_standing_timer);
			last_man_standing_timer_set = 1;
		}
	}

	if (member_list_entries) {
		memcpy(quorum_members, member_list, sizeof(unsigned int) * member_list_entries);
		quorum_members_entries = member_list_entries;
		if (quorum_device) {
			quorum_members[quorum_members_entries++] = 0;
		}
		quorum_exec_send_nodeinfo();
	}

	if (left_list_entries) {
		recalculate_quorum(leaving, leaving);
	}

	memcpy(&quorum_ringid, ring_id, sizeof(*ring_id));

	if (configuration_type == TOTEM_CONFIGURATION_REGULAR) {
		set_quorum(quorum_members, quorum_members_entries,
			   cluster_is_quorate, &quorum_ringid);
	}

	LEAVE();
}

static void exec_votequorum_nodeinfo_endian_convert (void *message)
{
	struct req_exec_quorum_nodeinfo *nodeinfo = message;

	ENTER();

	nodeinfo->votes = swab32(nodeinfo->votes);
	nodeinfo->expected_votes = swab32(nodeinfo->expected_votes);
	nodeinfo->major_version = swab32(nodeinfo->major_version);
	nodeinfo->minor_version = swab32(nodeinfo->minor_version);
	nodeinfo->patch_version = swab32(nodeinfo->patch_version);
	nodeinfo->config_version = swab32(nodeinfo->config_version);
	nodeinfo->flags = swab32(nodeinfo->flags);
	nodeinfo->wait_for_all_status = swab32(nodeinfo->wait_for_all_status);
	nodeinfo->quorate = swab32(nodeinfo->quorate);

	LEAVE();
}

static void exec_votequorum_reconfigure_endian_convert (void *message)
{
	struct req_exec_quorum_reconfigure *reconfigure = message;

	ENTER();

	reconfigure->nodeid = swab32(reconfigure->nodeid);
	reconfigure->value = swab32(reconfigure->value);

	LEAVE();
}

static void message_handler_req_exec_votequorum_nodeinfo (
	const void *message,
	unsigned int nodeid)
{
	const struct req_exec_quorum_nodeinfo *req_exec_quorum_nodeinfo = message;
	struct cluster_node *node;
	int old_votes;
	int old_expected;
	nodestate_t old_state;
	int new_node = 0;

	ENTER();

	log_printf(LOGSYS_LEVEL_DEBUG, "got nodeinfo message from cluster node %d\n", nodeid);

	node = find_node_by_nodeid(nodeid);
	if (!node) {
		node = allocate_node(nodeid);
		new_node = 1;
	}
	if (!node) {
		corosync_api->error_memory_failure();
		LEAVE();
		return;
	}

	old_votes = node->votes;
	old_expected = node->expected_votes;
	old_state = node->state;

	/* Update node state */
	node->votes = req_exec_quorum_nodeinfo->votes;
	node->expected_votes = req_exec_quorum_nodeinfo->expected_votes;
	node->state = NODESTATE_MEMBER;

	log_printf(LOGSYS_LEVEL_DEBUG, "nodeinfo message: votes: %d, expected: %d wfa: %d quorate: %d\n",
					req_exec_quorum_nodeinfo->votes,
					req_exec_quorum_nodeinfo->expected_votes,
					req_exec_quorum_nodeinfo->wait_for_all_status,
					req_exec_quorum_nodeinfo->quorate);

	if ((last_man_standing) && (req_exec_quorum_nodeinfo->votes > 1)) {
		log_printf(LOGSYS_LEVEL_WARNING, "Last Man Standing feature is supported only when all"
						 "cluster nodes votes are set to 1. Disabling LMS.");
		last_man_standing = 0;
		if (last_man_standing_timer_set) {
			corosync_api->timer_delete(last_man_standing_timer);
			last_man_standing_timer_set = 0;
		}
	}

	node->flags &= ~NODE_FLAGS_BEENDOWN;

	if (new_node ||
	    req_exec_quorum_nodeinfo->first_trans || 
	    old_votes != node->votes ||
	    old_expected != node->expected_votes ||
	    old_state != node->state) {
		recalculate_quorum(0, 0);
	}

	if (!nodeid) {
		free(node);
	}

	if ((wait_for_all) &&
	    (!req_exec_quorum_nodeinfo->wait_for_all_status) &&
	    (req_exec_quorum_nodeinfo->quorate)) {
		wait_for_all_status = 0;
	}

	LEAVE();
}

static void message_handler_req_exec_votequorum_reconfigure (
	const void *message,
	unsigned int nodeid)
{
	const struct req_exec_quorum_reconfigure *req_exec_quorum_reconfigure = message;
	struct cluster_node *node;
	struct list_head *nodelist;

	ENTER();

	log_printf(LOGSYS_LEVEL_DEBUG, "got reconfigure message from cluster node %d\n", nodeid);

	node = find_node_by_nodeid(req_exec_quorum_reconfigure->nodeid);
	if (!node) {
		LEAVE();
		return;
	}

	switch(req_exec_quorum_reconfigure->param)
	{
	case RECONFIG_PARAM_EXPECTED_VOTES:
		list_iterate(nodelist, &cluster_members_list) {
			node = list_entry(nodelist, struct cluster_node, list);
			if (node->state == NODESTATE_MEMBER &&
			    node->expected_votes > req_exec_quorum_reconfigure->value) {
				node->expected_votes = req_exec_quorum_reconfigure->value;
			}
		}
		send_expectedvotes_notification();
		recalculate_quorum(1, 0);  /* Allow decrease */
		break;

	case RECONFIG_PARAM_NODE_VOTES:
		node->votes = req_exec_quorum_reconfigure->value;
		recalculate_quorum(1, 0);  /* Allow decrease */
		break;

	case RECONFIG_PARAM_LEAVING:
		if (req_exec_quorum_reconfigure->value == 1 && node->state == NODESTATE_MEMBER) {
			node->state = NODESTATE_LEAVING;
		}
		if (req_exec_quorum_reconfigure->value == 0 && node->state == NODESTATE_LEAVING) {
			node->state = NODESTATE_MEMBER;
		}
		break;
	}

	LEAVE();
}

static int quorum_lib_init_fn (void *conn)
{
	struct quorum_pd *pd = (struct quorum_pd *)corosync_api->ipc_private_data_get (conn);

	ENTER();

	list_init (&pd->list);
	pd->conn = conn;

	LEAVE();
	return (0);
}

/*
 * Someone called votequorum_leave AGES ago!
 * Assume they forgot to shut down the node.
 */
static void leaving_timer_fn(void *arg)
{
	ENTER();

	if (us->state == NODESTATE_LEAVING) {
		us->state = NODESTATE_MEMBER;
	}

	/*
	 * Tell everyone else we made a mistake
	 */
	quorum_exec_send_reconfigure(RECONFIG_PARAM_LEAVING, us->node_id, 0);

	LEAVE();
}

/*
 * Message from the library
 */
static void message_handler_req_lib_votequorum_getinfo (void *conn, const void *message)
{
	const struct req_lib_votequorum_getinfo *req_lib_votequorum_getinfo = message;
	struct res_lib_votequorum_getinfo res_lib_votequorum_getinfo;
	struct cluster_node *node;
	unsigned int highest_expected = 0;
	unsigned int total_votes = 0;
	cs_error_t error = CS_OK;

	ENTER();

	log_printf(LOGSYS_LEVEL_DEBUG, "got getinfo request on %p for node %d\n", conn, req_lib_votequorum_getinfo->nodeid);

	node = find_node_by_nodeid(req_lib_votequorum_getinfo->nodeid);
	if (node) {
		struct cluster_node *iternode;
		struct list_head *nodelist;

		list_iterate(nodelist, &cluster_members_list) {
			iternode = list_entry(nodelist, struct cluster_node, list);

			if (iternode->state == NODESTATE_MEMBER) {
				highest_expected =
					max(highest_expected, iternode->expected_votes);
				total_votes += iternode->votes;
			}
		}

		if (quorum_device && quorum_device->state == NODESTATE_MEMBER) {
			total_votes += quorum_device->votes;
		}

		res_lib_votequorum_getinfo.votes = us->votes;
		res_lib_votequorum_getinfo.expected_votes = us->expected_votes;
		res_lib_votequorum_getinfo.highest_expected = highest_expected;

		res_lib_votequorum_getinfo.quorum = quorum;
		res_lib_votequorum_getinfo.total_votes = total_votes;
		res_lib_votequorum_getinfo.flags = 0;
		res_lib_votequorum_getinfo.nodeid = node->node_id;

		if (two_node) {
			res_lib_votequorum_getinfo.flags |= VOTEQUORUM_INFO_FLAG_TWONODE;
		}
		if (cluster_is_quorate) {
			res_lib_votequorum_getinfo.flags |= VOTEQUORUM_INFO_FLAG_QUORATE;
		}
	} else {
		error = CS_ERR_NOT_EXIST;
	}

	res_lib_votequorum_getinfo.header.size = sizeof(res_lib_votequorum_getinfo);
	res_lib_votequorum_getinfo.header.id = MESSAGE_RES_VOTEQUORUM_GETINFO;
	res_lib_votequorum_getinfo.header.error = error;
	corosync_api->ipc_response_send(conn, &res_lib_votequorum_getinfo, sizeof(res_lib_votequorum_getinfo));
	log_printf(LOGSYS_LEVEL_DEBUG, "getinfo response error: %d\n", error);

	LEAVE();
}

/*
 * Message from the library
 */
static void message_handler_req_lib_votequorum_setexpected (void *conn, const void *message)
{
	const struct req_lib_votequorum_setexpected *req_lib_votequorum_setexpected = message;
	struct res_lib_votequorum_status res_lib_votequorum_status;
	cs_error_t error = CS_OK;
	unsigned int newquorum;
	unsigned int total_votes;

	ENTER();

	/*
	 * Validate new expected votes
	 */
	newquorum = calculate_quorum(1, req_lib_votequorum_setexpected->expected_votes, &total_votes);
	if (newquorum < total_votes / 2 ||
	    newquorum > total_votes) {
		error = CS_ERR_INVALID_PARAM;
		goto error_exit;
	}

	quorum_exec_send_reconfigure(RECONFIG_PARAM_EXPECTED_VOTES, us->node_id,
				     req_lib_votequorum_setexpected->expected_votes);

	/*
	 * send status
	 */
error_exit:
	res_lib_votequorum_status.header.size = sizeof(res_lib_votequorum_status);
	res_lib_votequorum_status.header.id = MESSAGE_RES_VOTEQUORUM_STATUS;
	res_lib_votequorum_status.header.error = error;
	corosync_api->ipc_response_send(conn, &res_lib_votequorum_status, sizeof(res_lib_votequorum_status));

	LEAVE();
}

/*
 * Message from the library
 */
static void message_handler_req_lib_votequorum_setvotes (void *conn, const void *message)
{
	const struct req_lib_votequorum_setvotes *req_lib_votequorum_setvotes = message;
	struct res_lib_votequorum_status res_lib_votequorum_status;
	struct cluster_node *node;
	unsigned int newquorum;
	unsigned int total_votes;
	unsigned int saved_votes;
	cs_error_t error = CS_OK;
	unsigned int nodeid;

	ENTER();

	nodeid = req_lib_votequorum_setvotes->nodeid;
	node = find_node_by_nodeid(nodeid);
	if (!node) {
		error = CS_ERR_NAME_NOT_FOUND;
		goto error_exit;
	}

	/*
	 * Check votes is valid
	 */
	saved_votes = node->votes;
	node->votes = req_lib_votequorum_setvotes->votes;

	newquorum = calculate_quorum(1, 0, &total_votes);

	if (newquorum < total_votes / 2 ||
	    newquorum > total_votes) {
		node->votes = saved_votes;
		error = CS_ERR_INVALID_PARAM;
		goto error_exit;
	}

	if (!nodeid) {
		nodeid = corosync_api->totem_nodeid_get();
	}

	quorum_exec_send_reconfigure(RECONFIG_PARAM_NODE_VOTES, nodeid,
				     req_lib_votequorum_setvotes->votes);

	/*
	 * send status
	 */
error_exit:
	res_lib_votequorum_status.header.size = sizeof(res_lib_votequorum_status);
	res_lib_votequorum_status.header.id = MESSAGE_RES_VOTEQUORUM_STATUS;
	res_lib_votequorum_status.header.error = error;
	corosync_api->ipc_response_send(conn, &res_lib_votequorum_status, sizeof(res_lib_votequorum_status));

	LEAVE();
}

static void message_handler_req_lib_votequorum_leaving (void *conn, const void *message)
{
	struct res_lib_votequorum_status res_lib_votequorum_status;
	cs_error_t error = CS_OK;

	ENTER();

	quorum_exec_send_reconfigure(RECONFIG_PARAM_LEAVING, us->node_id, 1);

	/*
	 * If we don't shut down in a sensible amount of time then cancel the
	 * leave status.
	 */
	if (leaving_timeout) {
		corosync_api->timer_add_duration((unsigned long long)leaving_timeout*1000000, NULL,
						 leaving_timer_fn, &leaving_timer);
	}

	res_lib_votequorum_status.header.size = sizeof(res_lib_votequorum_status);
	res_lib_votequorum_status.header.id = MESSAGE_RES_VOTEQUORUM_STATUS;
	res_lib_votequorum_status.header.error = error;
	corosync_api->ipc_response_send(conn, &res_lib_votequorum_status, sizeof(res_lib_votequorum_status));

	LEAVE();
}

static void quorum_device_timer_fn(void *arg)
{
	ENTER();

	if (!quorum_device || quorum_device->state == NODESTATE_DEAD) {
		LEAVE();
		return;
	}

	if ((quorum_device->last_hello / QB_TIME_NS_IN_SEC) + quorumdev_poll/1000 <
	    (qb_util_nano_current_get () / QB_TIME_NS_IN_SEC)) {
		quorum_device->state = NODESTATE_DEAD;
		log_printf(LOGSYS_LEVEL_INFO, "lost contact with quorum device\n");
		recalculate_quorum(0, 0);
	} else {
		corosync_api->timer_add_duration((unsigned long long)quorumdev_poll*1000000, quorum_device,
						 quorum_device_timer_fn, &quorum_device_timer);
	}

	LEAVE();
}

static void message_handler_req_lib_votequorum_qdisk_register (void *conn,
							       const void *message)
{
	const struct req_lib_votequorum_qdisk_register *req_lib_votequorum_qdisk_register = message;
	struct res_lib_votequorum_status res_lib_votequorum_status;
	cs_error_t error = CS_OK;

	ENTER();

	if (quorum_device) {
		error = CS_ERR_EXIST;
	} else {
		quorum_device = allocate_node(0);
		quorum_device->state = NODESTATE_DEAD;
		quorum_device->votes = req_lib_votequorum_qdisk_register->votes;
		strcpy(quorum_device_name, req_lib_votequorum_qdisk_register->name);
		list_add(&quorum_device->list, &cluster_members_list);
	}

	/*
	 * send status
	 */
	res_lib_votequorum_status.header.size = sizeof(res_lib_votequorum_status);
	res_lib_votequorum_status.header.id = MESSAGE_RES_VOTEQUORUM_STATUS;
	res_lib_votequorum_status.header.error = error;
	corosync_api->ipc_response_send(conn, &res_lib_votequorum_status, sizeof(res_lib_votequorum_status));

	LEAVE();
}

static void message_handler_req_lib_votequorum_qdisk_unregister (void *conn,
								 const void *message)
{
	struct res_lib_votequorum_status res_lib_votequorum_status;
	cs_error_t error = CS_OK;

	ENTER();

	if (quorum_device) {
		struct cluster_node *node = quorum_device;

		quorum_device = NULL;
		list_del(&node->list);
		free(node);
		recalculate_quorum(0, 0);
	} else {
		error = CS_ERR_NOT_EXIST;
	}

	/*
	 * send status
	 */
	res_lib_votequorum_status.header.size = sizeof(res_lib_votequorum_status);
	res_lib_votequorum_status.header.id = MESSAGE_RES_VOTEQUORUM_STATUS;
	res_lib_votequorum_status.header.error = error;
	corosync_api->ipc_response_send(conn, &res_lib_votequorum_status, sizeof(res_lib_votequorum_status));

	LEAVE();
}

static void message_handler_req_lib_votequorum_qdisk_poll (void *conn,
							   const void *message)
{
	const struct req_lib_votequorum_qdisk_poll *req_lib_votequorum_qdisk_poll = message;
	struct res_lib_votequorum_status res_lib_votequorum_status;
	cs_error_t error = CS_OK;

	ENTER();

	if (quorum_device) {
		if (req_lib_votequorum_qdisk_poll->state) {
			quorum_device->last_hello = qb_util_nano_current_get ();
			if (quorum_device->state == NODESTATE_DEAD) {
				quorum_device->state = NODESTATE_MEMBER;
				recalculate_quorum(0, 0);

				corosync_api->timer_add_duration((unsigned long long)quorumdev_poll*1000000, quorum_device,
								 quorum_device_timer_fn, &quorum_device_timer);
			}
		} else {
			if (quorum_device->state == NODESTATE_MEMBER) {
				quorum_device->state = NODESTATE_DEAD;
				recalculate_quorum(0, 0);
				corosync_api->timer_delete(quorum_device_timer);
			}
		}
	} else {
		error = CS_ERR_NOT_EXIST;
	}

	/*
	 * send status
	 */
	res_lib_votequorum_status.header.size = sizeof(res_lib_votequorum_status);
	res_lib_votequorum_status.header.id = MESSAGE_RES_VOTEQUORUM_STATUS;
	res_lib_votequorum_status.header.error = error;
	corosync_api->ipc_response_send(conn, &res_lib_votequorum_status, sizeof(res_lib_votequorum_status));

	LEAVE();
}

static void message_handler_req_lib_votequorum_qdisk_getinfo (void *conn,
							      const void *message)
{
	struct res_lib_votequorum_qdisk_getinfo res_lib_votequorum_qdisk_getinfo;
	cs_error_t error = CS_OK;

	ENTER();

	if (quorum_device) {
		log_printf(LOGSYS_LEVEL_DEBUG, "got qdisk_getinfo state %d\n", quorum_device->state);
		res_lib_votequorum_qdisk_getinfo.votes = quorum_device->votes;
		if (quorum_device->state == NODESTATE_MEMBER) {
			res_lib_votequorum_qdisk_getinfo.state = 1;
		} else {
			res_lib_votequorum_qdisk_getinfo.state = 0;
		}
		strcpy(res_lib_votequorum_qdisk_getinfo.name, quorum_device_name);
	} else {
		error = CS_ERR_NOT_EXIST;
	}

	/*
	 * send status
	 */
	res_lib_votequorum_qdisk_getinfo.header.size = sizeof(res_lib_votequorum_qdisk_getinfo);
	res_lib_votequorum_qdisk_getinfo.header.id = MESSAGE_RES_VOTEQUORUM_GETINFO;
	res_lib_votequorum_qdisk_getinfo.header.error = error;
	corosync_api->ipc_response_send(conn, &res_lib_votequorum_qdisk_getinfo, sizeof(res_lib_votequorum_qdisk_getinfo));

	LEAVE();
}

static void message_handler_req_lib_votequorum_trackstart (void *conn,
							   const void *message)
{
	const struct req_lib_votequorum_trackstart *req_lib_votequorum_trackstart = message;
	struct res_lib_votequorum_status res_lib_votequorum_status;
	struct quorum_pd *quorum_pd = (struct quorum_pd *)corosync_api->ipc_private_data_get (conn);

	ENTER();
	/*
	 * If an immediate listing of the current cluster membership
	 * is requested, generate membership list
	 */
	if (req_lib_votequorum_trackstart->track_flags & CS_TRACK_CURRENT ||
	    req_lib_votequorum_trackstart->track_flags & CS_TRACK_CHANGES) {
		log_printf(LOGSYS_LEVEL_DEBUG, "sending initial status to %p\n", conn);
		send_quorum_notification(conn, req_lib_votequorum_trackstart->context);
	}

	/*
	 * Record requests for tracking
	 */
	if (req_lib_votequorum_trackstart->track_flags & CS_TRACK_CHANGES ||
	    req_lib_votequorum_trackstart->track_flags & CS_TRACK_CHANGES_ONLY) {

		quorum_pd->track_flags = req_lib_votequorum_trackstart->track_flags;
		quorum_pd->tracking_enabled = 1;
		quorum_pd->tracking_context = req_lib_votequorum_trackstart->context;

		list_add (&quorum_pd->list, &trackers_list);
	}

	/*
	 * Send status
	 */
	res_lib_votequorum_status.header.size = sizeof(res_lib_votequorum_status);
	res_lib_votequorum_status.header.id = MESSAGE_RES_VOTEQUORUM_STATUS;
	res_lib_votequorum_status.header.error = CS_OK;
	corosync_api->ipc_response_send(conn, &res_lib_votequorum_status, sizeof(res_lib_votequorum_status));

	LEAVE();
}

static void message_handler_req_lib_votequorum_trackstop (void *conn,
							  const void *message)
{
	struct res_lib_votequorum_status res_lib_votequorum_status;
	struct quorum_pd *quorum_pd = (struct quorum_pd *)corosync_api->ipc_private_data_get (conn);
	int error = CS_OK;

	ENTER();

	if (quorum_pd->tracking_enabled) {
		error = CS_OK;
		quorum_pd->tracking_enabled = 0;
		list_del (&quorum_pd->list);
		list_init (&quorum_pd->list);
	} else {
		error = CS_ERR_NOT_EXIST;
	}

	/*
	 * send status
	 */
	res_lib_votequorum_status.header.size = sizeof(res_lib_votequorum_status);
	res_lib_votequorum_status.header.id = MESSAGE_RES_VOTEQUORUM_STATUS;
	res_lib_votequorum_status.header.error = error;
	corosync_api->ipc_response_send(conn, &res_lib_votequorum_status, sizeof(res_lib_votequorum_status));

	LEAVE();
}

static void reread_config(void)
{
	unsigned int old_votes;
	unsigned int old_expected;

	ENTER();

	old_votes = us->votes;
	old_expected = us->expected_votes;

	/*
	 * Reload the configuration
	 */
	read_quorum_config();

	/*
	 * Check for fundamental changes that we need to propogate
	 */
	if (old_votes != us->votes) {
		quorum_exec_send_reconfigure(RECONFIG_PARAM_NODE_VOTES, us->node_id, us->votes);
	}
	if (old_expected != us->expected_votes) {
		quorum_exec_send_reconfigure(RECONFIG_PARAM_EXPECTED_VOTES, us->node_id, us->expected_votes);
	}

	LEAVE();
}

static void key_change_quorum(
	int32_t event,
	const char *key_name,
	struct icmap_notify_value new_val,
	struct icmap_notify_value old_val,
	void *user_data)
{
	ENTER();

	reread_config();

	LEAVE();
}

static void add_votequorum_config_notification(void)
{
	icmap_track_t icmap_track;

	ENTER();

	icmap_track_add("quorum.",
		ICMAP_TRACK_ADD | ICMAP_TRACK_DELETE | ICMAP_TRACK_MODIFY | ICMAP_TRACK_PREFIX,
		key_change_quorum,
		NULL,
		&icmap_track);

	LEAVE();
}
