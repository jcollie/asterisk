/*
 * Asterisk -- A telephony toolkit for Linux.
 *
 * ReadFile application -- Reads in a File for you.
 *
 * Copyright (C) 2005 Digium, Inc.
 *
 * Matt O'Gorman <mogorman@digium.com>
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <asterisk/file.h>
#include <asterisk/logger.h>
#include <asterisk/options.h>
#include <asterisk/channel.h>
#include <asterisk/pbx.h>
#include <asterisk/app.h>
#include <asterisk/module.h>

static char *tdesc = "Stores output of file into a variable";

static char *app_readfile = "ReadFile";

static char *readfile_synopsis = "ReadFile(varname=file,length)";

static char *readfile_descrip =
"ReadFile(varname=file,length)\n"
"  Varname - Result stored here.\n"
"  File - The name of the file to read.\n"
"  Length   - Maximum number of lines to capture.\n";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;


static int readfile_exec(struct ast_channel *chan, void *data)
{
	int res=0;
	struct localuser *u;
	char *s, *varname=NULL, *file=NULL, *length=NULL, *returnvar=NULL;
	int len=0;


	s = ast_strdupa(data);
	if (!s) {
		ast_log(LOG_ERROR, "Out of memory\n");
		return -1;
	}

	varname = strsep(&s, "=");
	file = strsep(&s, "|");
	length = s;

	if (!varname || !file) {
		ast_log(LOG_ERROR, "No file or variable specified!\n");
		return -1;
	}

	LOCAL_USER_ADD(u);
	if (length) {
		if ((sscanf(length, "%d", &len) != 1) || (len < 0)) {
			ast_log(LOG_WARNING, "%s is not a positive number, defaulting length to max\n", length);
			len = 0;
		}
	}
	
	
	returnvar = ast_read_textfile(file);
	if(len > 0){
		if(len < strlen(returnvar))
			returnvar[len]='\0';
		else
			ast_log(LOG_WARNING,"%s is longer than %i, and %i \n",file,len,strlen(returnvar));
	}
	pbx_builtin_setvar_helper(chan, varname, returnvar);
	free(returnvar);
	LOCAL_USER_REMOVE(u);
	return res;
}


int unload_module(void)
{
	STANDARD_HANGUP_LOCALUSERS;
	return ast_unregister_application(app_readfile);
}

int load_module(void)
{
	return ast_register_application(app_readfile, readfile_exec, readfile_synopsis, readfile_descrip);
}

char *description(void)
{
	return tdesc;
}

int usecount(void)
{
	int res;
	STANDARD_USECOUNT(res);
	return res;
}

char *key()
{
	return ASTERISK_GPL_KEY;
}
