/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#ifndef WT_BLOCK_MODS_H
#define WT_BLOCK_MODS_H

#include "wt_internal.h"

class BlockMods {
public:
    BlockMods();
    ~BlockMods();
    WT_BLOCK_MODS *
    getWTBlockMods()
    {
        return &_block_mods;
    };

private:
    void initBlockMods();
    WT_BLOCK_MODS _block_mods;
};

#endif // WT_BLOCK_MODS_H
