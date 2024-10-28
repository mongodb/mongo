/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "utils_extlist.h"

#include <catch2/catch.hpp>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

#include "../utils.h"

bool
operator<(const utils::off_size &left, const utils::off_size &right)
{
    return ((left.off < right.off) || ((left.off == right.off) && (left.size < right.size)));
}

namespace utils {
/*
 * ext_print_list --
 *     Print a skip list of WT_EXT *.
 */
void
ext_print_list(const WT_EXT *const *head)
{
    if (head == nullptr)
        return;

    for (int idx = 0; idx < WT_SKIP_MAXDEPTH; ++idx) {
        std::ostringstream line_stream{};
        line_stream << "L" << idx << ": ";

        const WT_EXT *extp = head[idx];
        while (extp != nullptr) {
            line_stream << reinterpret_cast<const void *>(extp) << ' ' << extp << " -> ";
            extp = extp->next[idx];
        }

        line_stream << "X\n";
        INFO(line_stream.str());
    }
}

/*
 * extlist_print_off --
 *     Print an WT_EXTLIST and it's off skip list.
 */
void
extlist_print_off(const WT_EXTLIST &extlist)
{
    std::ostringstream line_stream{};
    const char *track_size = extlist.track_size ? "true" : "false";
    line_stream << std::showbase << "{name "
                << ((extlist.name != nullptr) ? extlist.name : "(nullptr)") << ", bytes "
                << extlist.bytes << ", entries " << extlist.entries << ", objectid "
                << extlist.objectid << ", offset " << extlist.offset << ", checksum " << std::hex
                << extlist.checksum << std::dec << ", size " << extlist.size << ", track_size "
                << track_size << ", last " << extlist.last << ' ';
    if (extlist.last != nullptr) {
        off_size last_off_size = {extlist.last->off, extlist.last->size};
        line_stream << &last_off_size;
    } else
        line_stream << static_cast<off_size *>(nullptr);
    line_stream << "}\noff:\n";
    INFO(line_stream.str());
    ext_print_list(extlist.off);
}

/*!
 * alloc_new_ext --
 *     Allocate and initialize a WT_EXT structure for tests. Require that the allocation succeeds. A
 *     convenience wrapper for __wti_block_ext_alloc().
 *
 * @param off The offset.
 * @param size The size.
 */
WT_EXT *
alloc_new_ext(WT_SESSION_IMPL *session, wt_off_t off, wt_off_t size)
{
    WT_EXT *ext;
    REQUIRE(__wti_block_ext_alloc(session, &ext) == 0);
    ext->off = off;
    ext->size = size;

    INFO("Allocated WT_EXT " << ext << " {off " << ext->off << ", size " << ext->size << ", end "
                             << (ext->off + ext->size - 1) << ", depth "
                             << static_cast<unsigned>(ext->depth) << ", next[0] " << ext->next[0]
                             << '}');

    return ext;
}

/*!
 * alloc_new_ext --
 *     Allocate and initialize a WT_EXT structure for tests. Require that the allocation succeeds. A
 *     convenience wrapper for __wti_block_ext_alloc().
 *
 * @param off_size The offset and the size.
 */
WT_EXT *
alloc_new_ext(WT_SESSION_IMPL *session, const off_size &one)
{
    return alloc_new_ext(session, one.off, one.size);
}

/*
 * get_off_n --
 *     Get the nth level [0] member of a WT_EXTLIST's offset skiplist. Use case: Scan the offset
 *     skiplist to verify the contents.
 */
WT_EXT *
get_off_n(const WT_EXTLIST &extlist, uint32_t idx)
{
    WT_EXT *ext = extlist.off[0];
    uint32_t ext_idx = 0;
    REQUIRE(idx < extlist.entries);
    while (ext_idx < idx) {
        REQUIRE(ext != NULL);
        ext = ext->next[0];
        ++ext_idx;
    }

    if (idx == (extlist.entries - 1))
        REQUIRE(ext == extlist.last);
    return ext;
}

/*!
 * ext_free_list --
 *    Free a skip list of WT_EXT * for tests.
 *    Return whether WT_EXTLIST.last was found and freed.
 *
 * @param session the session
 * @param head the skip list
 * @param last WT_EXTLIST.last
 */
bool
ext_free_list(WT_SESSION_IMPL *session, WT_EXT **head, WT_EXT *last)
{
    if (head == nullptr)
        return false;

    /* Free just the top level. Lower levels are duplicates. */
    bool last_found = false;
    WT_EXT *extp = head[0];
    head[0] = nullptr;
    while (extp != nullptr) {
        if (extp == last)
            last_found = true;
        WT_EXT *next_extp = extp->next[0];
        extp->next[0] = nullptr;
        __wti_block_ext_free(session, &extp);
        extp = next_extp;
    }
    return last_found;
}

/*!
 * size_free_list --
 *    Free a skip list of WT_SIZE * for tests.
 *    Return whether WT_EXTLIST.last was found and freed.
 *
 * @param session the session
 * @param head the skip list
 */
void
size_free_list(WT_SESSION_IMPL *session, WT_SIZE **head)
{
    if (head == nullptr)
        return;

    /* Free just the top level. Lower levels are duplicates. */
    WT_SIZE *sizep = head[0];
    head[0] = nullptr;
    while (sizep != nullptr) {
        WT_SIZE *next_sizep = sizep->next[0];
        sizep->next[0] = nullptr;
        __wti_block_size_free(session, &sizep);
        sizep = next_sizep;
    }
}

/*!
 * extlist_free --
 *    Free the a skip lists of WT_EXTLIST * for tests.
 *
 * @param session the session
 * @param extlist the extent list
 */
void
extlist_free(WT_SESSION_IMPL *session, WT_EXTLIST &extlist)
{
    if (ext_free_list(session, extlist.off, extlist.last))
        extlist.last = nullptr;
    else if (extlist.last != nullptr)
        __wti_block_ext_free(session, &extlist.last);
    size_free_list(session, extlist.sz);
}

/*!
 * verify_empty_extent_list --
 *     Verify an extent list is empty. This was derived from the tests for __block_off_srch_last.
 *
 * @param head the extlist
 * @param stack the stack for appending returned by __block_off_srch_last
 */
void
verify_empty_extent_list(WT_EXT **head, WT_EXT ***stack)
{
    REQUIRE(__ut_block_off_srch_last(&head[0], &stack[0]) == nullptr);
    for (int i = 0; i < WT_SKIP_MAXDEPTH; i++) {
        REQUIRE(stack[i] == &head[i]);
    }
}

/*!
 * verify_off_extent_list --
 *     Verify the offset skiplist of a WT_EXTLIST is as expected. Also optionally verify the entries
 *     and bytes of a WT_EXTLIST are as expected.
 *
 * @param extlist the extent list to verify
 * @param expected_order the expected offset skip list
 * @param verify_entries_bytes whether to verify entries and bytes
 */
void
verify_off_extent_list(
  const WT_EXTLIST &extlist, const std::vector<off_size> &expected_order, bool verify_bytes)
{
    uint32_t idx = 0;
    uint64_t expected_bytes = 0;
    for (const off_size &expected : expected_order) {
        WT_EXT *ext = get_off_n(extlist, idx);
        INFO("Verify: " << std::showbase << idx << ". Expected: {off " << expected.off << ", size "
                        << expected.size << ", end " << expected.end() << "}; Actual: " << ext
                        << " {off " << ext->off << ", size " << ext->size << ", end "
                        << (ext->off + ext->size - 1) << '}');
        REQUIRE(ext->off == expected.off);
        REQUIRE(ext->size == expected.size);
        ++idx;
        expected_bytes += ext->size;
    }
    REQUIRE(idx == extlist.entries);
    if (!verify_bytes)
        return;
    REQUIRE(extlist.bytes == expected_bytes);
}
} // namespace utils.

/*!
 * operator<<(std::ostream &out, const utils::off_size *os) --
 *     Print function for const utils::off_size *.
 *
 * @param out ostream
 * @param os To be printed
 */
std::ostream &
operator<<(std::ostream &out, const utils::off_size *os)
{
    if (os == nullptr) {
        out << "(nullptr)";
        return out;
    }
    out << "{off " << os->off << ", size " << os->size << ", end " << os->end() << '}';
    return out;
}

/*!
 * operator<<(std::ostream &out, const WT_EXT *ext) --
 *     Print function for const WT_EXT *.
 *
 * @param out ostream
 * @param ext To be printed
 */
std::ostream &
operator<<(std::ostream &out, const WT_EXT *ext)
{
    if (ext == nullptr) {
        out << "(nullptr)";
        return out;
    }

    out << "{off " << ext->off << ", size " << ext->size << ", end " << (ext->off + ext->size - 1)
        << '}';
    return out;
}

/*!
 * operator<<(std::ostream &out, const WT_EXTLIST *extlist) --
 *     Print function for const WT_EXTLIST *.
 *
 * @param out ostream
 * @param extlist To be printed
 */
std::ostream &
operator<<(std::ostream &out, const WT_EXTLIST *extlist)
{
    if (extlist == nullptr) {
        out << "(nullptr)";
        return out;
    }

    out << std::showbase << "{name " << ((extlist->name != nullptr) ? extlist->name : "(nullptr)")
        << ", bytes " << extlist->bytes << ", entries " << extlist->entries << ", objectid "
        << extlist->objectid << ", offset " << extlist->offset << ", checksum " << std::hex
        << extlist->checksum << std::dec << ", size " << extlist->size << ", track_size "
        << (extlist->track_size ? "true" : "false") << ", last " << extlist->last << ' ';
    if (extlist->last != nullptr) {
        utils::off_size last_off_size = {extlist->last->off, extlist->last->size};
        out << &last_off_size;
    } else
        out << static_cast<utils::off_size *>(nullptr);
    return out;
}
