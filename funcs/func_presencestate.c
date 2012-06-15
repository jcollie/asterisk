/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2011, Digium, Inc.
 *
 * David Vossel <dvossel@digium.com> 
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

/*! \file
 *
 * \brief Custom presence provider
 * \ingroup functions
 */

/*** MODULEINFO
	<support_level>core</support_level>
 ***/

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include "asterisk/module.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/utils.h"
#include "asterisk/linkedlists.h"
#include "asterisk/presencestate.h"
#include "asterisk/cli.h"
#include "asterisk/astdb.h"
#include "asterisk/app.h"
#ifdef TEST_FRAMEWORK
#include "asterisk/test.h"
#include "asterisk/event.h"
#include <semaphore.h>
#endif

/*** DOCUMENTATION
	<function name="PRESENCE_STATE" language="en_US">
		<synopsis>
			Get or Set a presence state.
		</synopsis>
		<syntax>
			<parameter name="provider" required="true">
			  <para>The provider of the presence, such as <literal>CustomPresence</literal></para>
			</parameter>
			<parameter name="field" required="true">
			  <para>Which field of the presence state information is wanted.</para>
			  <optionlist>
				<option name="value">
				  <para>The current presence, such as <literal>away</literal></para>
				</option>
				<option name="subtype">
				  <para>Further information about the current presence</para>
				</option>
			    <option name="message">
				  <para>A custom message that may indicate further details about the presence</para>
				</option>
			  </optionlist>
			</parameter>
			<parameter name="options" required="false">
			  <optionlist>
			    <option name="e">
				  <para>Base-64 encode the data.</para>
				</option>
			  </optionlist>
			</parameter>
		</syntax>
		<description>
			<para>The PRESENCE_STATE function can be used to retrieve the presence from any
			presence provider. For example:</para>
			<para>NoOp(SIP/mypeer has presence ${PRESENCE_STATE(SIP/mypeer,value)})</para>
			<para>NoOp(Conference number 1234 has presence message ${PRESENCE_STATE(MeetMe:1234,message)})</para>
			<para>The PRESENCE_STATE function can also be used to set custom presence state from
			the dialplan.  The <literal>CustomPresence:</literal> prefix must be used. For example:</para>
			<para>Set(PRESENCE_STATE(CustomPresence:lamp1)=away,temporary,Out to lunch)</para>
			<para>Set(PRESENCE_STATE(CustomPresence:lamp2)=dnd,,Trying to get work done)</para>
			<para>You can subscribe to the status of a custom presence state using a hint in
			the dialplan:</para>
			<para>exten => 1234,hint,CustomPresence:lamp1</para>
			<para>The possible values for both uses of this function are:</para>
			<para>not_set | unavailable | available | away | xa | chat | dnd</para>
		</description>
	</function>
 ***/


static const char astdb_family[] = "CustomPresence";

static int presence_read(struct ast_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	int state;
	char *message = NULL;
	char *subtype = NULL;
	char *parse;
	int base64encode = 0;
	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(provider);
		AST_APP_ARG(field);
		AST_APP_ARG(options);
	);

	if (ast_strlen_zero(data)) {
		ast_log(LOG_WARNING, "PRESENCE_STATE reading requires an argument \n");
		return -1;
	}

	parse = ast_strdupa(data);

	AST_STANDARD_APP_ARGS(args, parse);

	if (ast_strlen_zero(args.provider) || ast_strlen_zero(args.field)) {
		ast_log(LOG_WARNING, "PRESENCE_STATE reading requires both presence provider and presence field arguments. \n");
		return -1;
	}

	state = ast_presence_state_nocache(args.provider, &subtype, &message);
	if (state == AST_PRESENCE_INVALID) {
		ast_log(LOG_WARNING, "PRESENCE_STATE unknown \n");
		return -1;
	}

	if (!(ast_strlen_zero(args.options)) && (strchr(args.options, 'e'))) {
		base64encode = 1;
	}

	if (!ast_strlen_zero(subtype) && !strcasecmp(args.field, "subtype")) {
		if (base64encode) {
			ast_base64encode(buf, (unsigned char *) subtype, strlen(subtype), len);
		} else {
			ast_copy_string(buf, subtype, len);
		}
	} else if (!ast_strlen_zero(message) && !strcasecmp(args.field, "message")) {
		if (base64encode) {
			ast_base64encode(buf, (unsigned char *) message, strlen(message), len);
		} else {
			ast_copy_string(buf, message, len);
		}

	} else if (!strcasecmp(args.field, "value")) {
		ast_copy_string(buf, ast_presence_state2str(state), len);
	}

	ast_free(message);
	ast_free(subtype);

	return 0;
}

static int parse_data(char *data, enum ast_presence_state *state, char **subtype, char **message, char **options)
{
	char *state_str;

	/* data syntax is state,subtype,message,options */
	*subtype = "";
	*message = "";
	*options = "";

	state_str = strsep(&data, ",");
	if (ast_strlen_zero(state_str)) {
		return -1; /* state is required */
	}

	*state = ast_presence_state_val(state_str);

	/* not a valid state */
	if (*state == AST_PRESENCE_INVALID) {
		ast_log(LOG_WARNING, "Unknown presence state value %s\n", state_str);
		return -1;
	}

	if (!(*subtype = strsep(&data,","))) {
		*subtype = "";
		return 0;
	}

	if (!(*message = strsep(&data, ","))) {
		*message = "";
		return 0;
	}

	if (!(*options = strsep(&data, ","))) {
		*options = "";
		return 0;
	}

	if (!ast_strlen_zero(*options) && !(strchr(*options, 'e'))) {
		ast_log(LOG_NOTICE, "Invalid options  '%s'\n", *options);
		return -1;
	}

	return 0;
}

static int presence_write(struct ast_channel *chan, const char *cmd, char *data, const char *value)
{
	size_t len = strlen("CustomPresence:");
	char *tmp = data;
	char *args = ast_strdupa(value);
	enum ast_presence_state state;
	char *options, *message, *subtype;

	if (strncasecmp(data, "CustomPresence:", len)) {
		ast_log(LOG_WARNING, "The PRESENCE_STATE function can only set CustomPresence: presence providers.\n");
		return -1;
	}
	data += len;
	if (ast_strlen_zero(data)) {
		ast_log(LOG_WARNING, "PRESENCE_STATE function called with no custom device name!\n");
		return -1;
	}

	if (parse_data(args, &state, &subtype, &message, &options)) {
		ast_log(LOG_WARNING, "Invalid arguments to PRESENCE_STATE\n");
		return -1;
	}

	ast_db_put(astdb_family, data, value);

	ast_presence_state_changed_literal(state, subtype, message, tmp);

	return 0;
}

static enum ast_presence_state custom_presence_callback(const char *data, char **subtype, char **message)
{
	char buf[1301] = "";
	enum ast_presence_state state;
	char *_options;
	char *_message;
	char *_subtype;

	ast_log(LOG_NOTICE, "TITTY BOMBS!\n");

	ast_db_get(astdb_family, data, buf, sizeof(buf));

	if (parse_data(buf, &state, &_subtype, &_message, &_options)) {
		return -1;
	}

	if ((strchr(_options, 'e'))) {
		char tmp[1301];
		if (ast_strlen_zero(_subtype)) {
			*subtype = NULL;
		} else {
			memset(tmp, 0, sizeof(tmp));
			ast_log(LOG_NOTICE, "Hey there, I'm doing some base64 decoding\n");
			ast_base64decode((unsigned char *) tmp, _subtype, sizeof(tmp) - 1);
			*subtype = ast_strdup(tmp);
		}

		if (ast_strlen_zero(_message)) {
			*message = NULL;
		} else {
			memset(tmp, 0, sizeof(tmp));
			ast_log(LOG_NOTICE, "Hey there, I'm doing some more base64 decoding\n");
			ast_base64decode((unsigned char *) tmp, _message, sizeof(tmp) - 1);
			*message = ast_strdup(tmp);
		}
	} else {
		ast_log(LOG_NOTICE, "Not doing any base64 decoding\n");
		*subtype = ast_strlen_zero(_subtype) ? NULL : ast_strdup(_subtype);
		*message = ast_strlen_zero(_message) ? NULL : ast_strdup(_message);
	}
	return state;
}

static struct ast_custom_function presence_function = {
	.name = "PRESENCE_STATE",
	.read = presence_read,
	.write = presence_write,
};

static char *handle_cli_presencestate_list(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	struct ast_db_entry *db_entry, *db_tree;

	switch (cmd) {
	case CLI_INIT:
		e->command = "presencestate list";
		e->usage =
			"Usage: presencestate list\n"
			"       List all custom presence states that have been set by using\n"
			"       the PRESENCE_STATE dialplan function.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != e->args) {
		return CLI_SHOWUSAGE;
	}

	ast_cli(a->fd, "\n"
	        "---------------------------------------------------------------------\n"
	        "--- Custom Presence States ------------------------------------------\n"
	        "---------------------------------------------------------------------\n"
	        "---\n");

	db_entry = db_tree = ast_db_gettree(astdb_family, NULL);
	if (!db_entry) {
		ast_cli(a->fd, "No custom presence states defined\n");
		return CLI_SUCCESS;
	}
	for (; db_entry; db_entry = db_entry->next) {
		const char *object_name = strrchr(db_entry->key, '/') + 1;
		char state_info[1301];
		enum ast_presence_state state;
		char *subtype;
		char *message;
		char *options;

		ast_copy_string(state_info, db_entry->data, sizeof(state_info));
		if (parse_data(state_info, &state, &subtype, &message, &options)) {
			ast_log(LOG_WARNING, "Invalid CustomPresence entry %s encountered\n", db_entry->data);
			continue;
		}

		if (object_name <= (const char *) 1) {
			continue;
		}
		ast_cli(a->fd, "--- Name: 'CustomPresence:%s'\n"
				       "    --- State: '%s'\n"
					   "    --- Subtype: '%s'\n"
					   "    --- Message: '%s'\n"
					   "    --- Base64 Encoded: '%s'\n"
		               "---\n",
					   object_name,
					   ast_presence_state2str(state),
					   subtype,
					   message,
					   AST_CLI_YESNO(strchr(options, 'e')));
	}
	ast_db_freetree(db_tree);
	db_tree = NULL;

	ast_cli(a->fd,
	        "---------------------------------------------------------------------\n"
	        "---------------------------------------------------------------------\n"
	        "\n");

	return CLI_SUCCESS;
}

static char *handle_cli_presencestate_change(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
    size_t len;
	const char *dev, *state, *full_dev;
	enum ast_presence_state state_val;
	char *message;
	char *subtype;
	char *options;
	char *args;

	switch (cmd) {
	case CLI_INIT:
		e->command = "presencestate change";
		e->usage =
			"Usage: presencestate change <entity> <state>[,<subtype>[,message[,options]]]\n"
			"       Change a custom presence to a new state.\n"
			"       The possible values for the state are:\n"
			"NOT_SET | UNAVAILABLE | AVAILABLE | AWAY | XA | CHAT | DND\n"
			"Optionally, a custom subtype and message may be provided, along with any options\n"
			"accepted by func_presencestate. If the subtype or message provided contain spaces,\n"
			"be sure to enclose the data in quotation marks (\"\")\n"
			"\n"
			"Examples:\n"
			"       presencestate change CustomPresence:mystate1 AWAY\n"
			"       presencestate change CustomPresence:mystate1 AVAILABLE\n"
			"       presencestate change CustomPresence:mystate1 \"Away,upstairs,eating lunch\"\n"
			"       \n";
		return NULL;
	case CLI_GENERATE:
	{
		static const char * const cmds[] = { "NOT_SET", "UNAVAILABLE", "AVAILABLE", "AWAY",
						     "XA", "CHAT", "DND", NULL };

		if (a->pos == e->args + 1) {
			return ast_cli_complete(a->word, cmds, a->n);
		}

		return NULL;
	}
	}

	if (a->argc != e->args + 2) {
		return CLI_SHOWUSAGE;
	}

	len = strlen("CustomPresence:");
	full_dev = dev = a->argv[e->args];
	state = a->argv[e->args + 1];

	if (strncasecmp(dev, "CustomPresence:", len)) {
		ast_cli(a->fd, "The presencestate command can only be used to set 'CustomPresence:' presence state!\n");
		return CLI_FAILURE;
	}

	dev += len;
	if (ast_strlen_zero(dev)) {
		return CLI_SHOWUSAGE;
	}

	args = ast_strdupa(state);
	if (parse_data(args, &state_val, &subtype, &message, &options)) {
		return CLI_SHOWUSAGE;
	}

	if (state_val == AST_PRESENCE_NOT_SET) {
		return CLI_SHOWUSAGE;
	}

	ast_cli(a->fd, "Changing %s to %s\n", dev, args);

	ast_db_put(astdb_family, dev, state);

	ast_presence_state_changed_literal(state_val, subtype, message, full_dev);

	return CLI_SUCCESS;
}

static struct ast_cli_entry cli_funcpresencestate[] = {
	AST_CLI_DEFINE(handle_cli_presencestate_list, "List currently know custom presence states"),
	AST_CLI_DEFINE(handle_cli_presencestate_change, "Change a custom presence state"),
};

#ifdef TEST_FRAMEWORK

struct test_string {
	char *parse_string;
	struct {
		int value;
		const char *subtype;
		const char *message;
		const char *options; 
	} outputs;
};

AST_TEST_DEFINE(test_valid_parse_data)
{
	int i;
	enum ast_presence_state state;
	char *subtype;
	char *message;
	char *options;
	enum ast_test_result_state res = AST_TEST_PASS;
	
	struct test_string tests [] = {
		{ "away",
			{ AST_PRESENCE_AWAY,
				"",
				"",
				""
			}
		},
		{ "not_set",
			{ AST_PRESENCE_NOT_SET,
				"",
				"",
				""
			}
		},
		{ "unavailable",
			{ AST_PRESENCE_UNAVAILABLE,
				"",
				"",
				""
			}
		},
		{ "available",
			{ AST_PRESENCE_AVAILABLE,
				"",
				"",
				""
			}
		},
		{ "xa",
			{ AST_PRESENCE_XA,
				"",
				"",
				""
			}
		},
		{ "chat",
			{ AST_PRESENCE_CHAT,
				"",
				"",
				""
			}
		},
		{ "dnd",
			{ AST_PRESENCE_DND,
				"",
				"",
				""
			}
		},
		{ "away,down the hall",
			{ AST_PRESENCE_AWAY,
				"down the hall",
				"",
				""
			}
		},
		{ "away,down the hall,Quarterly financial meeting",
			{ AST_PRESENCE_AWAY,
				"down the hall",
				"Quarterly financial meeting",
				""
			}
		},
		{ "away,,Quarterly financial meeting",
			{ AST_PRESENCE_AWAY,
				"",
				"Quarterly financial meeting",
				""
			}
		},
		{ "away,,,e",
			{ AST_PRESENCE_AWAY,
				"",
				"",
				"e",
			}
		},
		{ "away,down the hall,,e",
			{ AST_PRESENCE_AWAY,
				"down the hall",
				"",
				"e"
			}
		},
		{ "away,down the hall,Quarterly financial meeting,e",
			{ AST_PRESENCE_AWAY,
				"down the hall",
				"Quarterly financial meeting",
				"e"
			}
		},
		{ "away,,Quarterly financial meeting,e",
			{ AST_PRESENCE_AWAY,
				"",
				"Quarterly financial meeting",
				"e"
			}
		}
	};

	switch (cmd) {
	case TEST_INIT:
		info->name = "parse_valid_presence_data";
		info->category = "/funcs/func_presence";
		info->summary = "PRESENCESTATE parsing test";
		info->description =
			"Ensure that parsing function accepts proper values, and gives proper outputs";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	for (i = 0; i < ARRAY_LEN(tests); ++i) {
		int parse_result;
		char *parse_string = ast_strdup(tests[i].parse_string);
		if (!parse_string) {
			res = AST_TEST_FAIL;
			break;
		}
		parse_result = parse_data(parse_string, &state, &subtype, &message, &options);
		if (parse_result == -1) {
			res = AST_TEST_FAIL;
			ast_free(parse_string);
			break;
		}
		if (tests[i].outputs.value != state ||
				strcmp(tests[i].outputs.subtype, subtype) ||
				strcmp(tests[i].outputs.message, message) ||
				strcmp(tests[i].outputs.options, options)) {
			res = AST_TEST_FAIL;
			ast_free(parse_string);
			break;
		}
		ast_free(parse_string);
	}

	return res;
}

AST_TEST_DEFINE(test_invalid_parse_data)
{
	int i;
	enum ast_presence_state state;
	char *subtype;
	char *message;
	char *options;
	enum ast_test_result_state res = AST_TEST_PASS;

	char *tests[] = {
		"",
		"bored",
		"away,,,i",
		/* XXX The following actually is parsed correctly. Should that
		 * be changed?
		 * "away,,,,e",
		 */
	};

	switch (cmd) {
	case TEST_INIT:
		info->name = "parse_invalid_presence_data";
		info->category = "/funcs/func_presence";
		info->summary = "PRESENCESTATE parsing test";
		info->description =
			"Ensure that parsing function rejects improper values";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	for (i = 0; i < ARRAY_LEN(tests); ++i) {
		int parse_result;
		char *parse_string = ast_strdup(tests[i]);
		if (!parse_string) {
			res = AST_TEST_FAIL;
			break;
		}
		parse_result = parse_data(parse_string, &state, &subtype, &message, &options);
		if (parse_result == 0) {
			ast_log(LOG_WARNING, "Invalid string parsing failed on %s\n", tests[i]);
			res = AST_TEST_FAIL;
			ast_free(parse_string);
			break;
		}
		ast_free(parse_string);
	}

	return res;
}

struct test_cb_data {
	enum ast_presence_state presence;
	const char *provider;
	const char *subtype;
	const char *message;
	/* That's right. I'm using a semaphore */
	sem_t sem;
};

static void test_cb(const struct ast_event *event, void *userdata)
{
	struct test_cb_data *cb_data = userdata;
	cb_data->presence = ast_event_get_ie_uint(event, AST_EVENT_IE_PRESENCE_STATE);
	cb_data->provider = ast_strdup(ast_event_get_ie_str(event, AST_EVENT_IE_PRESENCE_PROVIDER));
	cb_data->subtype = ast_strdup(ast_event_get_ie_str(event, AST_EVENT_IE_PRESENCE_SUBTYPE));
	cb_data->message = ast_strdup(ast_event_get_ie_str(event, AST_EVENT_IE_PRESENCE_MESSAGE));
	sem_post(&cb_data->sem);
}

/* XXX This test could probably stand to be moved since
 * it does not test func_presencestate but rather code in
 * presencestate.h and presencestate.c. However, the convenience
 * of presence_write() makes this a nice location for this test.
 */
AST_TEST_DEFINE(test_presence_state_change)
{
	struct ast_event_sub *test_sub;
	struct test_cb_data *cb_data;

	switch (cmd) {
	case TEST_INIT:
		info->name = "test_presence_state_change";
		info->category = "/funcs/func_presence";
		info->summary = "presence state change subscription";
		info->description =
			"Ensure that presence state changes are communicated to subscribers";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	cb_data = ast_calloc(1, sizeof(*cb_data));
	if (!cb_data) {
		return AST_TEST_FAIL;
	}

	if (!(test_sub = ast_event_subscribe(AST_EVENT_PRESENCE_STATE,
			test_cb, "Test presence state callbacks", cb_data, AST_EVENT_IE_END))) {
		return AST_TEST_FAIL;
	}

	if (sem_init(&cb_data->sem, 0, 0)) {
		return AST_TEST_FAIL;
	}

	presence_write(NULL, "PRESENCESTATE", "CustomPresence:TestPresenceStateChange", "away,down the hall,Quarterly financial meeting");
	sem_wait(&cb_data->sem);
	if (cb_data->presence != AST_PRESENCE_AWAY ||
			strcmp(cb_data->provider, "CustomPresence:TestPresenceStateChange") ||
			strcmp(cb_data->subtype, "down the hall") ||
			strcmp(cb_data->message, "Quarterly financial meeting")) {
		return AST_TEST_FAIL;
	}

	ast_free((char *)cb_data->provider);
	ast_free((char *)cb_data->subtype);
	ast_free((char *)cb_data->message);
	ast_free((char *)cb_data);

	ast_db_del("CustomPresence", "TestPresenceStateChange");

	return AST_TEST_PASS;
}

#endif

static int unload_module(void)
{
	int res = 0;

	res |= ast_custom_function_unregister(&presence_function);
	res |= ast_presence_state_prov_del("CustomPresence");
	res |= ast_cli_unregister_multiple(cli_funcpresencestate, ARRAY_LEN(cli_funcpresencestate));
#ifdef TEST_FRAMEWORK
	AST_TEST_UNREGISTER(test_valid_parse_data);
	AST_TEST_UNREGISTER(test_invalid_parse_data);
	AST_TEST_UNREGISTER(test_presence_state_change);
#endif
	return res;
}

static int load_module(void)
{
	int res = 0;
	struct ast_db_entry *db_entry, *db_tree;

	/* Populate the presence state cache on the system with all of the currently
	 * known custom presence states. */
	db_entry = db_tree = ast_db_gettree(astdb_family, NULL);
	for (; db_entry; db_entry = db_entry->next) {
		const char *dev_name = strrchr(db_entry->key, '/') + 1;
		char state_info[1301];
		enum ast_presence_state state;
		char *message;
		char *subtype;
		char *options;
		if (dev_name <= (const char *) 1) {
			continue;
		}
		ast_copy_string(state_info, db_entry->data, sizeof(state_info));
		if (parse_data(state_info, &state, &subtype, &message, &options)) {
			ast_log(LOG_WARNING, "Invalid CustomPresence entry %s encountered\n", db_entry->data);
			continue;
		}
		ast_presence_state_changed(state, subtype, message, "CustomPresence:%s", dev_name);
	}
	ast_db_freetree(db_tree);
	db_tree = NULL;

	res |= ast_custom_function_register(&presence_function);
	res |= ast_presence_state_prov_add("CustomPresence", custom_presence_callback);
	res |= ast_cli_register_multiple(cli_funcpresencestate, ARRAY_LEN(cli_funcpresencestate));
#ifdef TEST_FRAMEWORK
	AST_TEST_REGISTER(test_valid_parse_data);
	AST_TEST_REGISTER(test_invalid_parse_data);
	AST_TEST_REGISTER(test_presence_state_change);
#endif

	return res;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER, "Gets or sets a presence state in the dialplan",
	.load = load_module,
	.unload = unload_module,
	.load_pri = AST_MODPRI_DEVSTATE_PROVIDER,
);

