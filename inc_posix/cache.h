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
 * WiredTiger public include file, and configuration control.
 *******************************************/
#include "wiredtiger.h"
#include "wiredtiger_config.h"

/*******************************************
 * WiredTiger system include files.
 *******************************************/
#include <stdlib.h>

/*******************************************
 * Everything without the heft to have its own file.
 *******************************************/
#include <laundry.h>

/*******************************************
 * Internal forward declarations.
 *******************************************/
struct __wt_fh_t;	typedef struct __wt_fh_t WT_FH;

/*******************************************
 * Internal versions of the database handle structure.
 *******************************************/
struct __idb {
	DB *db;					/* Public object */

#define	DB_ENV_IS_PRIVATE 0x01
	u_int32_t flags;
};

/*******************************************
 * Internal versions of the cursor handle structure.
 *******************************************/
struct __idbc {
	DBC *dbc;				/* Public object */

	u_int32_t flags;
};

/*******************************************
 * Internal versions of the cursor database environment handle structure.
 *******************************************/
struct __ienv {
	ENV *env;				/* Public object */

	u_int32_t flags;
};

/*******************************************
 * Internal include files.
 *******************************************/
#include "queue.h"
#include "fh.h"
#include "layout.h"

#include "extern.h"

#if defined(__cplusplus)
}
#endif
