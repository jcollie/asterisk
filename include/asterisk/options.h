/*
 * Asterisk -- A telephony toolkit for Linux.
 *
 * Options provided by main asterisk program
 * 
 * Copyright (C) 1999, Mark Spencer
 *
 * Mark Spencer <markster@linux-support.net>
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License
 */

#ifndef _ASTERISK_OPTIONS_H
#define _ASTERISK_OPTIONS_H

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

extern int option_verbose;
extern int option_debug;
extern int option_nofork;
extern int option_quiet;

#define VERBOSE_PREFIX_1 " "
#define VERBOSE_PREFIX_2 "  == "
#define VERBOSE_PREFIX_3 "    -- "

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif



#endif
