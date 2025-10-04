/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>

#include "util_block.h"
#include "../wrappers/config_parser.h"
#include "../wrappers/mock_session.h"

void
validate_ext_block(WT_EXT *ext)
{
    REQUIRE(ext != nullptr);
    REQUIRE(ext->depth != 0);
    REQUIRE(ext->size == 0);
    REQUIRE(ext->off == 0);

    for (int i = 0; i < ext->depth; i++) {
        REQUIRE(ext->next[i + ext->depth] == nullptr);
    }
}

void
free_ext_block(WT_EXT *ext)
{
    __wt_free(nullptr, ext);
}

void
validate_ext_list(WT_BLOCK_MGR_SESSION *bms, int expected_items)
{
    REQUIRE(bms != nullptr);
    REQUIRE(bms->ext_cache_cnt == expected_items);
    if (bms->ext_cache_cnt == 0)
        REQUIRE(bms->ext_cache == nullptr);

    WT_EXT *curr = bms->ext_cache;
    for (int i = 0; i < expected_items; i++) {
        validate_ext_block(curr);
        curr = curr->next[0];
    }
    REQUIRE(curr == nullptr);
}
void
validate_and_free_ext_block(WT_EXT *ext)
{
    validate_ext_block(ext);
    free_ext_block(ext);
}

void
validate_and_free_size_block(WT_SIZE *size)
{
    validate_size_block(size);
    free_size_block(size);
}

void
validate_size_list(WT_BLOCK_MGR_SESSION *bms, int expected_items)
{
    REQUIRE(bms != nullptr);
    if (bms->sz_cache_cnt == 0)
        REQUIRE(bms->sz_cache == nullptr);

    REQUIRE(bms->sz_cache_cnt == expected_items);
    WT_SIZE *curr = bms->sz_cache;
    for (int i = 0; i < expected_items; i++) {
        validate_size_block(curr);
        curr = curr->next[0];
    }
    REQUIRE(curr == nullptr);
}

void
validate_size_block(WT_SIZE *size)
{
    REQUIRE(size != nullptr);
    REQUIRE(size->depth == 0);
    REQUIRE(size->off[0] == nullptr);
    REQUIRE(size->size == 0);
}

void
free_size_block(WT_SIZE *size)
{
    __wt_free(nullptr, size);
}

/*
 * Initialize a write buffer to perform bm->write().
 */
void
create_write_buffer(WT_BM *bm, std::shared_ptr<mock_session> session, std::string contents,
  WT_ITEM *buf, size_t buf_memsize, size_t allocation_size)
{
    // Fetch write buffer size from block manager.
    REQUIRE(bm->write_size(bm, session->get_wt_session_impl(), &buf_memsize) == 0);
    test_and_validate_write_size(bm, session, buf_memsize, allocation_size);

    // Initialize the buffer with aligned size.
    REQUIRE(__wt_buf_initsize(session->get_wt_session_impl(), buf, buf_memsize) == 0);

    /*
     * Copy content string into the buffer.
     *
     * Following the architecture guide, it seems that the block manager expects a block header. I
     * have tried to mimic that here.
     */
    REQUIRE(__wt_buf_grow_worker(session->get_wt_session_impl(), buf, buf->size) == 0);
    memcpy(WT_BLOCK_HEADER_BYTE(buf->mem), contents.c_str(), contents.length());
}

void
setup_bm(std::shared_ptr<mock_session> &session, WT_BM *bm, const std::string &file_path,
  const std::string &allocation_size, const std::string &block_allocation,
  const std::string &os_cache_max, const std::string &os_cache_dirty_max,
  const std::string &access_pattern)
{
    REQUIRE(
      (session->get_mock_connection()->setup_block_manager(session->get_wt_session_impl())) == 0);
    session->setup_block_manager_file_operations();

    /*
     * Manually set all the methods in WT_BM. The __wt_blkcache_open() function also initializes the
     * block manager methods, however the function exists within the WiredTiger block cache and is
     * a layer above the block manager module. This violates the testing layer concept as we would
     * be technically testing a whole another module. Therefore we chosen to manually setup the
     * block manager instead
     * .
     */
    WT_CLEAR(*bm);
    __wti_bm_method_set(bm, false);

    // Create the underlying file in the filesystem.
    REQUIRE(__wt_block_manager_create(
              session->get_wt_session_impl(), file_path.c_str(), std::stoi(allocation_size)) == 0);

    // Open the file and return the block handle.
    config_parser cp({{"allocation_size", allocation_size}, {"block_allocation", block_allocation},
      {"os_cache_max", os_cache_max}, {"os_cache_dirty_max", os_cache_dirty_max},
      {"access_pattern_hint", access_pattern}});
    REQUIRE(__wt_block_open(session->get_wt_session_impl(), file_path.c_str(),
              WT_TIERED_OBJECTID_NONE, cp.get_config_array(), false, false, false,
              std::stoi(allocation_size), NULL, &bm->block) == 0);

    // Initialize the extent lists inside the block handle.
    REQUIRE(__wti_block_ckpt_init(session->get_wt_session_impl(), &bm->block->live, nullptr) == 0);
}

/*
 * Test and validate the bm->write_size() function.
 */
void
test_and_validate_write_size(
  WT_BM *bm, const std::shared_ptr<mock_session> &session, size_t size, size_t allocation_size)
{
    size_t ret_size = size;
    // This function internally reads and changes the variable.
    REQUIRE(bm->write_size(bm, session->get_wt_session_impl(), &ret_size) == 0);
    // It is expected that the size is a modulo of the allocation size and is aligned to the nearest
    // greater allocation size.
    CHECK(ret_size % allocation_size == 0);
    CHECK(ret_size == ((size / allocation_size) + 1) * allocation_size);
}
