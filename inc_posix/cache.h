/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.  All rights reserved.
 *
 * $Id$
 */

#if defined(__cplusplus)
extern "C" {
#endif

/*******************************************
 * WiredTiger general include files.
 *******************************************/
#include "wiredtiger_config.h"
#include "wiredtiger.h"
#include "decl.h"

/*******************************************
 * Internal forward declarations.
 *******************************************/
struct __wt_fh_t;	typedef struct __wt_fh_t WT_FH;

/*******************************************
 * Internal versions of the database handle structure.
 *******************************************/
struct __db_internal {
	int unused;				/* placeholder */
};

/*******************************************
 * Internal versions of the cursor handle structure.
 *******************************************/
struct __dbc_internal {
	int unused;				/* placeholder */
};

/*******************************************
 * Internal versions of the cursor database environment handle structure.
 *******************************************/
struct __env_internal {
	int unused;				/* placeholder */
};

/*******************************************
 * Internal include files.
 *******************************************/
#include "queue.h"
#include "fh.h"
#include "layout.h"

#if defined(__cplusplus)
}
#endif
