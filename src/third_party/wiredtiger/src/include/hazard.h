/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/*
 * WT_HAZARD_COOKIE --
 *   State passed through to callbacks during the session walk logic when
 *   looking for active hazard pointers.
 */
struct __wt_hazard_cookie {
    WT_REF *search_ref;
    WT_SESSION_IMPL **ret_session;
    WT_HAZARD *ret_hp;
    uint32_t walk_cnt;
    uint32_t max;
};
