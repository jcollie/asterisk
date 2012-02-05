/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2007, Digium, Inc.
 * Copyright (C) 2012, Russell Bryant
 *
 * Russell Bryant <russell@russellbryant.net>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*!
 * \file
 * \author Russell Bryant <russell@russellbryant.net>
 *
 * This module is based on and replaces the previous res_ais module.
 */

/*** MODULEINFO
	<depend>corosync</depend>
	<support_level>extended</support_level>
 ***/

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$");

#include <corosync/cpg.h>
#include <corosync/cfg.h>

#include "asterisk/module.h"
#include "asterisk/logger.h"
#include "asterisk/poll-compat.h"
#include "asterisk/config.h"
#include "asterisk/event.h"
#include "asterisk/cli.h"
#include "asterisk/devicestate.h"

AST_RWLOCK_DEFINE_STATIC(event_types_lock);

static struct {
	const char *name;
	struct ast_event_sub *sub;
	unsigned char publish;
	unsigned char subscribe;
} event_types[] = {
	[AST_EVENT_MWI] = { .name = "mwi", },
	[AST_EVENT_DEVICE_STATE_CHANGE] = { .name = "device_state", },
};

static struct {
	pthread_t id;
	int alert_pipe[2];
	unsigned int stop:1;
} dispatch_thread = {
	.id = AST_PTHREADT_NULL,
	.alert_pipe = { -1, -1 },
};

static cpg_handle_t cpg_handle;
static corosync_cfg_handle_t cfg_handle;

static void cfg_state_track_cb(
		corosync_cfg_state_notification_buffer_t *notification_buffer,
		cs_error_t error);

static void cfg_shutdown_cb(corosync_cfg_handle_t cfg_handle,
		corosync_cfg_shutdown_flags_t flags);

static corosync_cfg_callbacks_t cfg_callbacks = {
	.corosync_cfg_state_track_callback = cfg_state_track_cb,
	.corosync_cfg_shutdown_callback = cfg_shutdown_cb,
};

static void cpg_deliver_cb(cpg_handle_t handle, const struct cpg_name *group_name,
		uint32_t nodeid, uint32_t pid, void *msg, size_t msg_len);

static void cpg_confchg_cb(cpg_handle_t handle, const struct cpg_name *group_name,
		const struct cpg_address *member_list, size_t member_list_entries,
		const struct cpg_address *left_list, size_t left_list_entries,
		const struct cpg_address *joined_list, size_t joined_list_entries);

static cpg_callbacks_t cpg_callbacks = {
	.cpg_deliver_fn = cpg_deliver_cb,
	.cpg_confchg_fn = cpg_confchg_cb,
};

static void ast_event_cb(const struct ast_event *event, void *data);

static void cfg_state_track_cb(
		corosync_cfg_state_notification_buffer_t *notification_buffer,
		cs_error_t error)
{
}

static void cfg_shutdown_cb(corosync_cfg_handle_t cfg_handle,
		corosync_cfg_shutdown_flags_t flags)
{
}

static void cpg_deliver_cb(cpg_handle_t handle, const struct cpg_name *group_name,
		uint32_t nodeid, uint32_t pid, void *msg, size_t msg_len)
{
	struct ast_event *event;

	if (msg_len < ast_event_minimum_length()) {
		ast_debug(1, "Ignoring event that's too small. %u < %u\n",
			(unsigned int) msg_len,
			(unsigned int) ast_event_minimum_length());
		return;
	}

	if (!ast_eid_cmp(&ast_eid_default, ast_event_get_ie_raw(msg, AST_EVENT_IE_EID))) {
		/* Don't feed events back in that originated locally. */
		return;
	}

	ast_rwlock_rdlock(&event_types_lock);
	if (!event_types[ast_event_get_type(msg)].subscribe) {
		/* We are not configured to subscribe to these events. */
		ast_rwlock_unlock(&event_types_lock);
		return;
	}
	ast_rwlock_unlock(&event_types_lock);

	if (!(event = ast_malloc(msg_len))) {
		return;
	}

	memcpy(event, msg, msg_len);

	ast_event_queue_and_cache(event);
}

static void cpg_confchg_cb(cpg_handle_t handle, const struct cpg_name *group_name,
		const struct cpg_address *member_list, size_t member_list_entries,
		const struct cpg_address *left_list, size_t left_list_entries,
		const struct cpg_address *joined_list, size_t joined_list_entries)
{
	unsigned int i;

	/* If any new nodes have joined, dump our cache of events we are publishing
	 * that originated from this server. */

	if (!joined_list_entries) {
		return;
	}

	for (i = 0; i < ARRAY_LEN(event_types); i++) {
		struct ast_event_sub *event_sub;

		ast_rwlock_rdlock(&event_types_lock);
		if (!event_types[i].publish) {
			ast_rwlock_unlock(&event_types_lock);
			continue;
		}
		ast_rwlock_unlock(&event_types_lock);

		event_sub = ast_event_subscribe_new(i, ast_event_cb, NULL);
		ast_event_sub_append_ie_raw(event_sub, AST_EVENT_IE_EID,
					&ast_eid_default, sizeof(ast_eid_default));
		ast_event_dump_cache(event_sub);
		ast_event_sub_destroy(event_sub);
	}
}

static void *dispatch_thread_handler(void *data)
{
	cs_error_t cs_err;
	struct pollfd pfd[3] = {
		{ .events = POLLIN, },
		{ .events = POLLIN, },
		{ .events = POLLIN, },
	};

	if ((cs_err = cpg_fd_get(cpg_handle, &pfd[0].fd)) != CS_OK) {
		ast_log(LOG_ERROR, "Failed to get CPG fd.  This module is now broken.\n");
		return NULL;
	}

	if ((cs_err = corosync_cfg_fd_get(cfg_handle, &pfd[1].fd)) != CS_OK) {
		ast_log(LOG_ERROR, "Failed to get CFG fd.  This module is now broken.\n");
		return NULL;
	}

	pfd[2].fd = dispatch_thread.alert_pipe[0];

	while (!dispatch_thread.stop) {
		int res;

		pfd[0].revents = 0;
		pfd[1].revents = 0;
		pfd[2].revents = 0;

		res = ast_poll(pfd, ARRAY_LEN(pfd), -1);
		if (res == -1 && errno != EINTR && errno != EAGAIN) {
			ast_log(LOG_ERROR, "poll() error: %s (%d)\n", strerror(errno), errno);
			continue;
		}

		if (pfd[0].revents & POLLIN) {
			if ((cs_err = cpg_dispatch(cpg_handle, CS_DISPATCH_ALL)) != CS_OK) {
				ast_log(LOG_WARNING, "Failed CPG dispatch: %d\n", cs_err);
			}
		}

		if (pfd[1].revents & POLLIN) {
			if ((cs_err = corosync_cfg_dispatch(cfg_handle, CS_DISPATCH_ALL)) != CS_OK) {
				ast_log(LOG_WARNING, "Failed CFG dispatch: %d\n", cs_err);
			}
		}
	}

	return NULL;
}

static void ast_event_cb(const struct ast_event *event, void *data)
{
	cs_error_t cs_err;
	struct iovec iov = {
		.iov_base = (void *) event,
		.iov_len = ast_event_get_size(event),
	};

	if (ast_eid_cmp(&ast_eid_default,
			ast_event_get_ie_raw(event, AST_EVENT_IE_EID))) {
		/* If the event didn't originate from this server, don't send it back out. */
		return;
	}

	/* The ast_event subscription will only exist if we are configured to publish
	 * these events, so just send away. */

	if ((cs_err = cpg_mcast_joined(cpg_handle, CPG_TYPE_FIFO, &iov, 1)) != CS_OK) {
		ast_log(LOG_WARNING, "CPG mcast failed (%d)\n", cs_err);
	}
}

static char *corosync_show_members(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	cs_error_t cs_err;
	struct cpg_name name;
	struct cpg_address member_list[CPG_MEMBERS_MAX] =  { { 0, }, };
	int num_members = CPG_MEMBERS_MAX;
	unsigned int i;

	switch (cmd) {
	case CLI_INIT:
		e->command = "corosync show members";
		e->usage =
			"Usage: corosync show members\n"
			"       Show corosync cluster members\n";
		return NULL;

	case CLI_GENERATE:
		return NULL;	/* no completion */
	}

	if (a->argc != e->args) {
		return CLI_SHOWUSAGE;
	}

	ast_copy_string(name.value, "asterisk", sizeof(name.value));
	name.length = strlen(name.value);

	cs_err = cpg_membership_get(cpg_handle, &name, member_list, &num_members);

	if (cs_err != CS_OK) {
		ast_cli(a->fd, "Failed to get membership list\n");
		return CLI_FAILURE;
	}

	ast_cli(a->fd, "\n"
	            "=============================================================\n"
	            "=== Cluster members =========================================\n"
	            "=============================================================\n"
	            "===\n"
		    "=== Number of members: %d\n"
		    "===\n", num_members);

	for (i = 0; i < num_members; i++) {
		corosync_cfg_node_address_t addrs[8];
		int num_addrs = 0;
		unsigned int j;

		cs_err = corosync_cfg_get_node_addrs(cfg_handle, member_list[i].nodeid,
				ARRAY_LEN(addrs), &num_addrs, addrs);
		if (cs_err != CS_OK) {
			ast_log(LOG_WARNING, "Failed to get node addresses\n");
			continue;
		}

		ast_cli(a->fd, "=== Node %d\n", i + 1);

		for (j = 0; j < num_addrs; j++) {
			struct sockaddr *sa = (struct sockaddr *) addrs[j].address;
			size_t sa_len = (size_t) addrs[j].address_length;
			char buf[128];

			getnameinfo(sa, sa_len, buf, sizeof(buf), NULL, 0, NI_NUMERICHOST);

			ast_cli(a->fd, "=== --> Address %d: %s\n", j + 1, buf);
		}
	}

	ast_cli(a->fd, "===\n"
	               "=============================================================\n"
	               "\n");

	return CLI_SUCCESS;
}

static char *corosync_show_config(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	unsigned int i;

	switch (cmd) {
	case CLI_INIT:
		e->command = "corosync show config";
		e->usage =
			"Usage: corosync show config\n"
			"       Show configuration loaded from res_corosync.conf\n";
		return NULL;

	case CLI_GENERATE:
		return NULL;	/* no completion */
	}

	if (a->argc != e->args) {
		return CLI_SHOWUSAGE;
	}

	ast_cli(a->fd, "\n"
	            "=============================================================\n"
	            "=== res_corosync config =====================================\n"
	            "=============================================================\n"
	            "===\n");

	ast_rwlock_rdlock(&event_types_lock);
	for (i = 0; i < ARRAY_LEN(event_types); i++) {
		if (event_types[i].publish) {
			ast_cli(a->fd, "=== ==> Publishing Event Type: %s\n",
					event_types[i].name);
		}
		if (event_types[i].subscribe) {
			ast_cli(a->fd, "=== ==> Subscribing to Event Type: %s\n",
					event_types[i].name);
		}
	}
	ast_rwlock_unlock(&event_types_lock);

	ast_cli(a->fd, "===\n"
	               "=============================================================\n"
	               "\n");

	return CLI_SUCCESS;
}

static struct ast_cli_entry corosync_cli[] = {
	AST_CLI_DEFINE(corosync_show_config, "Show configuration"),
	AST_CLI_DEFINE(corosync_show_members, "Show cluster members"),
};

enum {
	PUBLISH,
	SUBSCRIBE,
};

static int set_event(const char *event_type, int pubsub)
{
	unsigned int i;

	for (i = 0; i < ARRAY_LEN(event_types); i++) {
		if (!event_types[i].name || strcasecmp(event_type, event_types[i].name)) {
			continue;
		}

		switch (pubsub) {
		case PUBLISH:
			event_types[i].publish = 1;
			break;
		case SUBSCRIBE:
			event_types[i].subscribe = 1;
			break;
		}

		break;
	}

	return (i == ARRAY_LEN(event_types)) ? -1 : 0;
}

static int load_general_config(struct ast_config *cfg)
{
	struct ast_variable *v;
	int res = 0;
	unsigned int i;

	ast_rwlock_wrlock(&event_types_lock);

	for (i = 0; i < ARRAY_LEN(event_types); i++) {
		event_types[i].publish = 0;
		event_types[i].subscribe = 0;
	}

	for (v = ast_variable_browse(cfg, "general"); v && !res; v = v->next) {
		if (!strcasecmp(v->name, "publish_event")) {
			res = set_event(v->value, PUBLISH);
		} else if (!strcasecmp(v->name, "subscribe_event")) {
			res = set_event(v->value, SUBSCRIBE);
		} else {
			ast_log(LOG_WARNING, "Unknown option '%s'\n", v->name);
		}
	}

	for (i = 0; i < ARRAY_LEN(event_types); i++) {
		if (event_types[i].publish && !event_types[i].sub) {
			event_types[i].sub = ast_event_subscribe(i,
						ast_event_cb, "Corosync", NULL,
						AST_EVENT_IE_END);
		} else if (!event_types[i].publish && event_types[i].sub) {
			event_types[i].sub = ast_event_unsubscribe(event_types[i].sub);
		}
	}

	ast_rwlock_unlock(&event_types_lock);

	return res;
}

static int load_config(unsigned int reload)
{
	static const char filename[] = "res_corosync.conf";
	struct ast_config *cfg;
	const char *cat = NULL;
	struct ast_flags config_flags = { 0 };
	int res = 0;

	cfg = ast_config_load(filename, config_flags);

	if (cfg == CONFIG_STATUS_FILEMISSING || cfg == CONFIG_STATUS_FILEINVALID) {
		return -1;
	}

	while ((cat = ast_category_browse(cfg, cat))) {
		if (!strcasecmp(cat, "general")) {
			res = load_general_config(cfg);
		} else {
			ast_log(LOG_WARNING, "Unknown configuration section '%s'\n", cat);
		}
	}

	ast_config_destroy(cfg);

	return res;
}

static void cleanup_module(void)
{
	cs_error_t cs_err;
	unsigned int i;

	for (i = 0; i < ARRAY_LEN(event_types); i++) {
		if (event_types[i].sub) {
			event_types[i].sub = ast_event_unsubscribe(event_types[i].sub);
		}
		event_types[i].publish = 0;
		event_types[i].subscribe = 0;
	}

	if (dispatch_thread.id != AST_PTHREADT_NULL) {
		char meepmeep = 'x';
		dispatch_thread.stop = 1;
		if (ast_carefulwrite(dispatch_thread.alert_pipe[1], &meepmeep, 1,
					5000) == -1) {
			ast_log(LOG_ERROR, "Failed to write to pipe: %s (%d)\n",
					strerror(errno), errno);
		}
		pthread_join(dispatch_thread.id, NULL);
	}

	if (dispatch_thread.alert_pipe[0] != -1) {
		close(dispatch_thread.alert_pipe[0]);
		dispatch_thread.alert_pipe[0] = -1;
	}

	if (dispatch_thread.alert_pipe[1] != -1) {
		close(dispatch_thread.alert_pipe[1]);
		dispatch_thread.alert_pipe[1] = -1;
	}

	if (cpg_handle && (cs_err = cpg_finalize(cpg_handle) != CS_OK)) {
		ast_log(LOG_ERROR, "Failed to finalize cpg (%d)\n", (int) cs_err);
	}
	cpg_handle = 0;

	if (cfg_handle && (cs_err = corosync_cfg_finalize(cfg_handle) != CS_OK)) {
		ast_log(LOG_ERROR, "Failed to finalize cfg (%d)\n", (int) cs_err);
	}
	cfg_handle = 0;
}

static int load_module(void)
{
	cs_error_t cs_err;
	enum ast_module_load_result res = AST_MODULE_LOAD_FAILURE;
	struct cpg_name name;

	if ((cs_err = corosync_cfg_initialize(&cfg_handle, &cfg_callbacks) != CS_OK)) {
		ast_log(LOG_ERROR, "Failed to initialize cfg (%d)\n", (int) cs_err);
		return AST_MODULE_LOAD_DECLINE;
	}

	if ((cs_err = cpg_initialize(&cpg_handle, &cpg_callbacks) != CS_OK)) {
		ast_log(LOG_ERROR, "Failed to initialize cpg (%d)\n", (int) cs_err);
		goto failed;
	}

	ast_copy_string(name.value, "asterisk", sizeof(name.value));
	name.length = strlen(name.value);

	if ((cs_err = cpg_join(cpg_handle, &name)) != CS_OK) {
		ast_log(LOG_ERROR, "Failed to join (%d)\n", (int) cs_err);
		goto failed;
	}

	if (pipe(dispatch_thread.alert_pipe) == -1) {
		ast_log(LOG_ERROR, "Failed to create alert pipe: %s (%d)\n",
				strerror(errno), errno);
		goto failed;
	}

	if (ast_pthread_create_background(&dispatch_thread.id, NULL,
			dispatch_thread_handler, NULL)) {
		ast_log(LOG_ERROR, "Error starting CPG dispatch thread.\n");
		goto failed;
	}

	if (load_config(0)) {
		/* simply not configured is not a fatal error */
		res = AST_MODULE_LOAD_DECLINE;
		goto failed;
	}

	ast_cli_register_multiple(corosync_cli, ARRAY_LEN(corosync_cli));

	ast_enable_distributed_devstate();

	return AST_MODULE_LOAD_SUCCESS;

failed:
	cleanup_module();

	return res;
}

static int unload_module(void)
{
	ast_cli_unregister_multiple(corosync_cli, ARRAY_LEN(corosync_cli));

	cleanup_module();

	return 0;
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "Corosync");
