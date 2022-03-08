/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "block_mods.h"

BlockMods::BlockMods()
{
    initBlockMods();
}

BlockMods::~BlockMods()
{
    __wt_buf_free(nullptr, &_block_mods.bitstring);
    __wt_free(nullptr, _block_mods.id_str);
}

void
BlockMods::initBlockMods()
{
    _block_mods.id_str = nullptr;
    _block_mods.bitstring.data = nullptr;
    _block_mods.bitstring.size = 0;
    _block_mods.bitstring.mem = nullptr;
    _block_mods.bitstring.memsize = 0;
    _block_mods.bitstring.flags = 0;
    _block_mods.nbits = 0;
    _block_mods.offset = 0;
    _block_mods.granularity = 0;
    _block_mods.flags = 0;
}
