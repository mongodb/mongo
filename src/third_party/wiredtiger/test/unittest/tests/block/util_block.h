/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */
#pragma once

#include "wt_internal.h"
#include "../wrappers/mock_session.h"

/* Extent validation functions. */
void free_ext_list(WT_BLOCK_MGR_SESSION *);
void validate_and_free_ext_block(WT_EXT *);
void validate_ext_list(WT_BLOCK_MGR_SESSION *, int);
void free_ext_block(WT_EXT *);
void validate_ext_block(WT_EXT *);

/* Size validation functions. */
void free_size_list(WT_BLOCK_MGR_SESSION *);
void validate_and_free_size_block(WT_SIZE *);
void validate_size_list(WT_BLOCK_MGR_SESSION *, int);
void free_size_block(WT_SIZE *);
void validate_size_block(WT_SIZE *);

/* Block Manager file API functions. */
void create_write_buffer(WT_BM *, std::shared_ptr<mock_session>, std::string, WT_ITEM *, size_t);
void setup_bm(std::shared_ptr<mock_session> &, WT_BM *, const std::string &);
void test_and_validate_write_size(WT_BM *, std::shared_ptr<mock_session>, const size_t);
