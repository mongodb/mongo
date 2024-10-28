/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */
#pragma once

#include <string>
#include <memory>

#include "wt_internal.h"
#include "../wrappers/mock_session.h"

/* Extent validation functions. */
void validate_and_free_ext_block(WT_EXT *ext);
void validate_ext_list(WT_BLOCK_MGR_SESSION *bms, int expected_items);
void free_ext_block(WT_EXT *ext);
void validate_ext_block(WT_EXT *ext);

/* Size validation functions. */
void validate_and_free_size_block(WT_SIZE *size);
void validate_size_list(WT_BLOCK_MGR_SESSION *bms, int expected_items);
void free_size_block(WT_SIZE *size);
void validate_size_block(WT_SIZE *size);

/* Block Manager file API functions. */
void create_write_buffer(WT_BM *bm, std::shared_ptr<mock_session> session, std::string contents,
  WT_ITEM *buf, size_t buf_memsize, size_t allocation_size);
void setup_bm(std::shared_ptr<mock_session> &session, WT_BM *bm, const std::string &file_path,
  const std::string &allocation_size, const std::string &block_allocation,
  const std::string &os_cache_max, const std::string &os_cache_dirty_max,
  const std::string &access_pattern);
void test_and_validate_write_size(
  WT_BM *bm, const std::shared_ptr<mock_session> &session, size_t size, size_t allocation_size);
