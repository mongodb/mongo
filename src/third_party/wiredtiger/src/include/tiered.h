/*-
 * Copyright (c) 2014-2020 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * WT_CURSOR_TIERED --
 *	An tiered cursor.
 */
struct __wt_cursor_tiered {
    WT_CURSOR iface;

    WT_TIERED *tiered;

    WT_CURSOR **cursors;
    WT_CURSOR *current; /* The current cursor for iteration */
    WT_CURSOR *primary; /* The current primary */

/* AUTOMATIC FLAG VALUE GENERATION START */
#define WT_CURTIERED_ACTIVE 0x1u       /* Incremented the session count */
#define WT_CURTIERED_ITERATE_NEXT 0x2u /* Forward iteration */
#define WT_CURTIERED_ITERATE_PREV 0x4u /* Backward iteration */
                                       /* AUTOMATIC FLAG VALUE GENERATION STOP */
    uint32_t flags;
};

/*
 * WT_TIERED --
 *	Handle for a tiered data source.
 */
struct __wt_tiered {
    WT_DATA_HANDLE iface;

    const char *name, *config, *filename;
    const char *key_format, *value_format;

    WT_DATA_HANDLE **tiers;
    u_int ntiers;

    WT_COLLATOR *collator; /* TODO: handle custom collation */
};
