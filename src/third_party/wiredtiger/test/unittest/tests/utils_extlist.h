/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

#include <string>
#include <vector>

#include "wt_internal.h"

namespace utils {
/*!
 * An offset (_off) and a size (_size) of a WT_EXT for two use cases: specifying a test setup or an
 * expected result.
 */
struct off_size {
    wt_off_t _off;
    wt_off_t _size;

    off_size(wt_off_t off = 0, wt_off_t size = 0) : _off(off), _size(size) {}
    /*!
     * end --
     *     Return the end of the closed interval represented by _off and _size.
     */
    wt_off_t
    end(void) const
    {
        return (_off + _size - 1);
    }
};

/*!
 * A test (_off_size) and the expected value (_expected_list) for operations that need an off_size
 * to modify a WT_EXTLIST.
 */
struct off_size_expected {
    off_size _off_size;
    std::vector<off_size> _expected_list;
};

void ext_print_list(const WT_EXT *const *head);
void extlist_print_off(const WT_EXTLIST &extlist);
WT_EXT *alloc_new_ext(WT_SESSION_IMPL *session, wt_off_t off = 0, wt_off_t size = 0);
WT_EXT *alloc_new_ext(WT_SESSION_IMPL *session, const off_size &one);
WT_EXT *get_off_n(const WT_EXTLIST &extlist, uint32_t idx);
bool ext_free_list(WT_SESSION_IMPL *session, WT_EXT **head, WT_EXT *last);
void size_free_list(WT_SESSION_IMPL *session, WT_SIZE **head);
void extlist_free(WT_SESSION_IMPL *session, WT_EXTLIST &extlist);
void verify_empty_extent_list(WT_EXT **head, WT_EXT ***stack);
void verify_off_extent_list(
  const WT_EXTLIST &extlist, const std::vector<off_size> &expected_order, bool verify_bytes = true);
} // namespace utils.

bool operator<(const utils::off_size &left, const utils::off_size &right);
