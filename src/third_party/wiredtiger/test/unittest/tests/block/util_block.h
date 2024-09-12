/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */
#pragma once

#include "wt_internal.h"

/* Extent validation functions. */
void validate_and_free_ext_list(WT_BLOCK_MGR_SESSION *bms, int);
void free_ext_list(WT_BLOCK_MGR_SESSION *);
void validate_and_free_ext_block(WT_EXT *);
void validate_ext_list(WT_BLOCK_MGR_SESSION *, int);
void free_ext_block(WT_EXT *);
void validate_ext_block(WT_EXT *);

/* Size validation functions. */
void validate_and_free_size_list(WT_BLOCK_MGR_SESSION *, int);
void free_size_list(WT_BLOCK_MGR_SESSION *);
void validate_and_free_size_block(WT_SIZE *);
void validate_size_list(WT_BLOCK_MGR_SESSION *, int);
void free_size_block(WT_SIZE *);
void validate_size_block(WT_SIZE *);
