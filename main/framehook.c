/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2010, Digium, Inc.
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
 * \brief FrameHooks Architecture
 *
 * \author David Vossel <dvossel@digium.com>
 */

/*** MODULEINFO
	<support_level>core</support_level>
 ***/

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include "asterisk/channel.h"
#include "asterisk/linkedlists.h"
#include "asterisk/framehook.h"
#include "asterisk/frame.h"

struct ast_framehook {
	struct ast_framehook_interface i;
	/*! This pointer to ast_channel the framehook is attached to. */
	struct ast_channel *chan;
	/*! the id representing this framehook on a channel */
	unsigned int id;
	/*! when set, this signals the read and write function to detach the hook */
	int detach_and_destroy_me;
	/*! list entry for ast_framehook_list object */
	AST_LIST_ENTRY(ast_framehook) list;
};

struct ast_framehook_list {
	/*! the number of hooks currently present */
	unsigned int count;
	/*! id for next framehook added */
	unsigned int id_count;
	AST_LIST_HEAD_NOLOCK(, ast_framehook) list;
};

static void framehook_detach_and_destroy(struct ast_framehook *framehook)
{
	struct ast_frame *frame;
	frame = framehook->i.event_cb(framehook->chan, NULL, AST_FRAMEHOOK_EVENT_DETACHED, framehook->i.data);
	/* never assume anything about this function. If you can return a frame during
	 * the detached event, then assume someone will. */
	if (frame) {
		ast_frfree(frame);
	}
	framehook->chan = NULL;

	if (framehook->i.destroy_cb) {
		framehook->i.destroy_cb(framehook->i.data);
	}
	ast_free(framehook);
}

static struct ast_frame *framehook_list_push_event(struct ast_framehook_list *framehooks, struct ast_frame *frame, enum ast_framehook_event event)
{
	struct ast_framehook *framehook;
	struct ast_frame *original_frame;
	int *skip;
	size_t skip_size;

	if (!framehooks) {
		return frame;
	}

	skip_size = sizeof(int) * framehooks->count;
	skip = alloca(skip_size);
	memset(skip, 0, skip_size);

	do {
		unsigned int num = 0;
		original_frame = frame;

		AST_LIST_TRAVERSE_SAFE_BEGIN(&framehooks->list, framehook, list) {
			if (framehook->detach_and_destroy_me) {
				/* this guy is signaled for destruction */
				AST_LIST_REMOVE_CURRENT(list);
				framehook_detach_and_destroy(framehook);
				continue;
			}

			/* If this framehook has been marked as needing to be skipped, do so */
			if (skip[num]) {
				num++;
				continue;
			}

			frame = framehook->i.event_cb(framehook->chan, frame, event, framehook->i.data);

			if (frame != original_frame) {
				/* To prevent looping we skip any framehooks that have already provided a modified frame */
				skip[num] = 1;
				break;
			}

			num++;
		}
		AST_LIST_TRAVERSE_SAFE_END;
	} while (frame != original_frame);

	return frame;
}

int ast_framehook_attach(struct ast_channel *chan, struct ast_framehook_interface *i)
{
	struct ast_framehook *framehook;
	struct ast_framehook_list *fh_list;
	struct ast_frame *frame;
	if (i->version != AST_FRAMEHOOK_INTERFACE_VERSION) {
		ast_log(LOG_ERROR, "Version '%hu' of framehook interface not what we compiled against (%hu)\n",
			i->version, AST_FRAMEHOOK_INTERFACE_VERSION);
		return -1;
	}
	if (!i->event_cb || !(framehook = ast_calloc(1, sizeof(*framehook)))) {
		return -1;
	}
	framehook->i = *i;
	framehook->chan = chan;

	/* create the framehook list if it didn't already exist */
	if (!ast_channel_framehooks(chan)) {
		if (!(fh_list = ast_calloc(1, sizeof(*ast_channel_framehooks(chan))))) {
			ast_free(framehook);
			return -1;
		}
		ast_channel_framehooks_set(chan, fh_list);
	}

	ast_channel_framehooks(chan)->count++;
	framehook->id = ++ast_channel_framehooks(chan)->id_count;
	AST_LIST_INSERT_TAIL(&ast_channel_framehooks(chan)->list, framehook, list);

	/* Tell the event callback we're live and rocking */
	frame = framehook->i.event_cb(framehook->chan, NULL, AST_FRAMEHOOK_EVENT_ATTACHED, framehook->i.data);

	/* Never assume anything about this function. If you can return a frame during
	 * the attached event, then assume someone will. */
	if (frame) {
		ast_frfree(frame);
	}

	return framehook->id;
}

int ast_framehook_detach(struct ast_channel *chan, int id)
{
	struct ast_framehook *framehook;
	int res = -1;

	if (!ast_channel_framehooks(chan)) {
		return res;
	}

	AST_LIST_TRAVERSE_SAFE_BEGIN(&ast_channel_framehooks(chan)->list, framehook, list) {
		if (framehook->id == id) {
			/* we mark for detachment rather than doing explicitly here because
			 * it needs to be safe for this function to be called within the
			 * event callback.  If we allowed the hook to actually be destroyed
			 * immediately here, the event callback would crash on exit. */
			framehook->detach_and_destroy_me = 1;
			res = 0;
			break;
		}
	}
	AST_LIST_TRAVERSE_SAFE_END;

	return res;
}

int ast_framehook_list_destroy(struct ast_channel *chan)
{
	struct ast_framehook *framehook;

	if (!ast_channel_framehooks(chan)) {
		return 0;
	}
	AST_LIST_TRAVERSE_SAFE_BEGIN(&ast_channel_framehooks(chan)->list, framehook, list) {
		AST_LIST_REMOVE_CURRENT(list);
		framehook_detach_and_destroy(framehook);
	}
	AST_LIST_TRAVERSE_SAFE_END;
	ast_free(ast_channel_framehooks(chan));
	ast_channel_framehooks_set(chan, NULL);
	return 0;
}

int ast_framehook_list_is_empty(struct ast_framehook_list *framehooks)
{
	if (!framehooks) {
		return 1;
	}
	return AST_LIST_EMPTY(&framehooks->list) ? 1 : 0;
}

int ast_framehook_list_contains_no_active(struct ast_framehook_list *framehooks)
{
	struct ast_framehook *cur;

	if (!framehooks) {
		return 1;
	}

	if (AST_LIST_EMPTY(&framehooks->list)) {
		return 1;
	}

	AST_LIST_TRAVERSE(&framehooks->list, cur, list) {
		if (cur->detach_and_destroy_me) {
			continue;
		}
		return 0;
	}

	return 1;
}

struct ast_frame *ast_framehook_list_write_event(struct ast_framehook_list *framehooks, struct ast_frame *frame)
{
	return framehook_list_push_event(framehooks, frame, AST_FRAMEHOOK_EVENT_WRITE);
}

struct ast_frame *ast_framehook_list_read_event(struct ast_framehook_list *framehooks, struct ast_frame *frame)
{
	return framehook_list_push_event(framehooks, frame, AST_FRAMEHOOK_EVENT_READ);
}
