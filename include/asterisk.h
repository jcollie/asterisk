/*
 * Asterisk -- A telephony toolkit for Linux.
 *
 * General Definitions for Asterisk top level program
 * 
 * Copyright (C) 1999-2006, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License
 */

/*! \file
 * \brief Asterisk main include file. File version handling, generic pbx functions.
 */

#ifndef _ASTERISK_H
#define _ASTERISK_H

/* The include of 'autoconfig.h' is not necessary for any modules that
   are part of the Asterisk source tree, because the top-level Makefile
   will forcibly include that header in all compilations before all
   other headers (even system headers). However, leaving this here will
   help out-of-tree module builders, and doesn't cause any harm for the
   in-tree modules.
*/
#include "asterisk/autoconfig.h"

#include "asterisk/compat.h"

#include "asterisk/logger.h"

/* Default to allowing the umask or filesystem ACLs to determine actual file
 * creation permissions
 */
#ifndef AST_DIR_MODE
#define AST_DIR_MODE 0777
#endif
#ifndef AST_FILE_MODE
#define AST_FILE_MODE 0666
#endif

#define DEFAULT_LANGUAGE "en"

#define DEFAULT_SAMPLE_RATE 8000
#define DEFAULT_SAMPLES_PER_MS  ((DEFAULT_SAMPLE_RATE)/1000)
#define	setpriority	__PLEASE_USE_ast_set_priority_INSTEAD_OF_setpriority__
#define	sched_setscheduler	__PLEASE_USE_ast_set_priority_INSTEAD_OF_sched_setscheduler__

int ast_set_priority(int);			/*!< Provided by asterisk.c */

/*!
 * \brief Register a function to be executed before Asterisk exits.
 * \param func The callback function to use.
 *
 * \retval 0 on success.
 * \retval -1 on error.
 */
int ast_register_atexit(void (*func)(void));

/*!   
 * \brief Unregister a function registered with ast_register_atexit().
 * \param func The callback function to unregister.   
 */
void ast_unregister_atexit(void (*func)(void));

#if !defined(LOW_MEMORY)
/*!
 * \brief Register the version of a source code file with the core.
 * \param file the source file name
 * \param version the version string (typically a CVS revision keyword string)
 * \return nothing
 *
 * This function should not be called directly, but instead the
 * ASTERISK_FILE_VERSION macro should be used to register a file with the core.
 */
void ast_register_file_version(const char *file, const char *version);

/*!
 * \brief Unregister a source code file from the core.
 * \param file the source file name
 * \return nothing
 *
 * This function should not be called directly, but instead the
 * ASTERISK_FILE_VERSION macro should be used to automatically unregister
 * the file when the module is unloaded.
 */
void ast_unregister_file_version(const char *file);

/*!
 * \brief Register/unregister a source code file with the core.
 * \param file the source file name
 * \param version the version string (typically a CVS revision keyword string)
 *
 * This macro will place a file-scope constructor and destructor into the
 * source of the module using it; this will cause the version of this file
 * to registered with the Asterisk core (and unregistered) at the appropriate
 * times.
 *
 * Example:
 *
 * \code
 * ASTERISK_FILE_VERSION(__FILE__, "\$Revision\$")
 * \endcode
 *
 * \note The dollar signs above have been protected with backslashes to keep
 * CVS from modifying them in this file; under normal circumstances they would
 * not be present and CVS would expand the Revision keyword into the file's
 * revision number.
 */
#ifdef MTX_PROFILE
#define	HAVE_MTX_PROFILE	/* used in lock.h */
#define ASTERISK_FILE_VERSION(file, version) \
	static int mtx_prof = -1;       /* profile mutex */	\
	static void __attribute__((constructor)) __register_file_version(void) \
	{ \
		mtx_prof = ast_add_profile("mtx_lock_" file, 0);	\
		ast_register_file_version(file, version); \
	} \
	static void __attribute__((destructor)) __unregister_file_version(void) \
	{ \
		ast_unregister_file_version(file); \
	}
#else /* !MTX_PROFILE */
#define ASTERISK_FILE_VERSION(file, version) \
	static void __attribute__((constructor)) __register_file_version(void) \
	{ \
		ast_register_file_version(file, version); \
	} \
	static void __attribute__((destructor)) __unregister_file_version(void) \
	{ \
		ast_unregister_file_version(file); \
	}
#endif /* !MTX_PROFILE */
#else /* LOW_MEMORY */
#define ASTERISK_FILE_VERSION(file, x)
#endif /* LOW_MEMORY */

#if !defined(LOW_MEMORY)
/*!
 * \brief support for event profiling
 *
 * (note, this must be documented a lot more)
 * ast_add_profile allocates a generic 'counter' with a given name,
 * which can be shown with the command 'show profile <name>'
 *
 * The counter accumulates positive or negative values supplied by
 * ast_add_profile(), dividing them by the 'scale' value passed in the
 * create call, and also counts the number of 'events'.
 * Values can also be taked by the TSC counter on ia32 architectures,
 * in which case you can mark the start of an event calling ast_mark(id, 1)
 * and then the end of the event with ast_mark(id, 0).
 * For non-i386 architectures, these two calls return 0.
 */
int ast_add_profile(const char *, uint64_t scale);
int64_t ast_profile(int, int64_t);
int64_t ast_mark(int, int start1_stop0);
#else /* LOW_MEMORY */
#define ast_add_profile(a, b) 0
#define ast_profile(a, b) do { } while (0)
#define ast_mark(a, b) do { } while (0)
#endif /* LOW_MEMORY */

/*! \brief
 * Definition of various structures that many asterisk files need,
 * but only because they need to know that the type exists.
 *
 */

struct ast_channel;
struct ast_frame;
struct ast_module;

#endif /* _ASTERISK_H */
