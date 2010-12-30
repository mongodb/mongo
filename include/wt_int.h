/* Copyright (c) 2010 WiredTiger, Inc.  All rights reserved. */

#include "wiredtiger.h"

#include "cur_std.h"
#include "extern.h"

#define	F_SET(s, f)	((s)->flags |= f)
#define	F_ISSET(s, f)	(((s)->flags & f) != 0)
