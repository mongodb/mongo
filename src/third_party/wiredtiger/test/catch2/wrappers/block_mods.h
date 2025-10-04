/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

#include "wt_internal.h"

class block_mods {
public:
    block_mods();
    ~block_mods();
    WT_CKPT_BLOCK_MODS *
    get_wt_block_mods()
    {
        return &_block_mods;
    };

private:
    void init_block_mods();
    WT_CKPT_BLOCK_MODS _block_mods;
};
