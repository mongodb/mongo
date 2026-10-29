/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>
#include <cstring>

#include "wt_internal.h"

/*
 * These tests exercise the resolution of the on-disk-format image a page's block metadata should be
 * cached with. A page's own image and its block metadata can independently describe different
 * versions of a page once reconciliation has run: a page force-cleaned on an outdated disaggregated
 * btree can reach the victim cache with its own image still the pre-reconciliation one while the
 * metadata already describes the reconciled one.
 *
 * A page's reconciliation result uses a union keyed by which kind of result it is (no replacement,
 * multiple blocks, or a single block); these helpers build only the union arm each result implies,
 * matching what reconciliation itself would have populated.
 */

namespace {

WT_PAGE_HEADER *
make_dsk(uint32_t mem_size)
{
    WT_PAGE_HEADER *dsk;
    /*
     * A real page header, sized like the real image buffers this stands in for; content beyond
     * mem_size is never inspected by the code under test.
     */
    dsk = static_cast<WT_PAGE_HEADER *>(calloc(1, sizeof(WT_PAGE_HEADER)));
    REQUIRE(dsk != nullptr);
    dsk->mem_size = mem_size;
    return dsk;
}

/* A page with disagg info and its own image, no reconciliation result at all. */
WT_PAGE
make_unmodified_page(WT_PAGE_HEADER *dsk, WT_PAGE_DISAGG_INFO *disagg_info)
{
    WT_PAGE page;
    memset(&page, 0, sizeof(page));
    page.dsk = dsk;
    page.modify = nullptr;
    page.disagg_info = disagg_info;
    return page;
}

/*
 * A page whose modify structure exists but has not been reconciled: the state a page is in before
 * its first reconciliation, still described entirely by its own image.
 */
WT_PAGE
make_dirty_unreconciled_page(
  WT_PAGE_HEADER *dsk, WT_PAGE_DISAGG_INFO *disagg_info, WT_PAGE_MODIFY *mod)
{
    memset(mod, 0, sizeof(*mod));
    mod->rec_result = 0;

    WT_PAGE page;
    memset(&page, 0, sizeof(page));
    page.dsk = dsk;
    page.modify = mod;
    page.disagg_info = disagg_info;
    return page;
}

/*
 * A page reconciled into a single replacement block. disk_image may be null to model the case where
 * the block was written but no in-memory copy was retained.
 */
WT_PAGE
make_replaced_page(
  WT_PAGE_HEADER *dsk, WT_PAGE_DISAGG_INFO *disagg_info, WT_PAGE_MODIFY *mod, void *disk_image)
{
    memset(mod, 0, sizeof(*mod));
    mod->rec_result = WT_PM_REC_REPLACE;
    mod->mod_disk_image = disk_image;

    WT_PAGE page;
    memset(&page, 0, sizeof(page));
    page.dsk = dsk;
    page.modify = mod;
    page.disagg_info = disagg_info;
    return page;
}

/*
 * A page reconciled into a split or a deletion. Neither populates the union arm a retained
 * replacement image would occupy.
 */
WT_PAGE
make_non_replace_page(
  WT_PAGE_HEADER *dsk, WT_PAGE_DISAGG_INFO *disagg_info, WT_PAGE_MODIFY *mod, uint8_t rec_result)
{
    memset(mod, 0, sizeof(*mod));
    mod->rec_result = rec_result;
    mod->mod_multi = nullptr;
    mod->mod_multi_entries = 0;

    WT_PAGE page;
    memset(&page, 0, sizeof(page));
    page.dsk = dsk;
    page.modify = mod;
    page.disagg_info = disagg_info;
    return page;
}

WT_PAGE_DISAGG_INFO
make_disagg_info(
  uint64_t page_id, uint64_t disagg_lsn, uint64_t base_lsn, uint32_t checksum, uint8_t delta_count)
{
    WT_PAGE_DISAGG_INFO info;
    memset(&info, 0, sizeof(info));
    info.block_meta.page_id = page_id;
    info.block_meta.disagg_lsn = disagg_lsn;
    info.block_meta.base_lsn = base_lsn;
    info.block_meta.checksum = checksum;
    info.block_meta.delta_count = delta_count;
    return info;
}

} // namespace

TEST_CASE("Victim cache source: an unmodified page's own image is used", "[evict][disagg_cache]")
{
    WT_PAGE_HEADER *dsk = make_dsk(4096);
    WT_PAGE_DISAGG_INFO info = make_disagg_info(14606, 100, 50, 0xabcd, 0);
    WT_PAGE page = make_unmodified_page(dsk, &info);

    REQUIRE(__ut_evict_page_disagg_image(&page) == dsk);

    free(dsk);
}

TEST_CASE(
  "Victim cache source: a dirty page never reconciled uses its own image", "[evict][disagg_cache]")
{
    WT_PAGE_HEADER *dsk = make_dsk(4096);
    WT_PAGE_DISAGG_INFO info = make_disagg_info(14606, 100, 50, 0xabcd, 0);
    WT_PAGE_MODIFY mod;
    WT_PAGE page = make_dirty_unreconciled_page(dsk, &info, &mod);

    REQUIRE(__ut_evict_page_disagg_image(&page) == dsk);

    free(dsk);
}

TEST_CASE(
  "Victim cache source: a single-block replacement with a retained image uses that image, "
  "not the page's original one",
  "[evict][disagg_cache]")
{
    /* The pre-reconciliation image: what the page's own image field still points at. */
    WT_PAGE_HEADER *old_dsk = make_dsk(4096);
    /*
     * The post-reconciliation image, retained for exactly this purpose. The page's block metadata
     * (checked separately, at the call site, not here) has already been advanced to describe this
     * one.
     */
    WT_PAGE_HEADER *new_dsk = make_dsk(6144);

    WT_PAGE_DISAGG_INFO info = make_disagg_info(14606, 200, 100, 0x1234, 1);
    WT_PAGE_MODIFY mod;
    WT_PAGE page = make_replaced_page(old_dsk, &info, &mod, new_dsk);

    /*
     * Using the page's own image here would publish stale content under the block metadata's newer
     * identity.
     */
    REQUIRE(
      __ut_evict_page_disagg_image(&page) == reinterpret_cast<const WT_PAGE_HEADER *>(new_dsk));
    REQUIRE(__ut_evict_page_disagg_image(&page) != old_dsk);

    free(old_dsk);
    free(new_dsk);
}

TEST_CASE(
  "Victim cache source: a single-block replacement with a retained image is used even when the "
  "page has no image of its own",
  "[evict][disagg_cache]")
{
    /*
     * A page with no on-disk image of its own: created purely in memory, never instantiated from a
     * read. The retained replacement image is the only image this page has ever had.
     */
    WT_PAGE_HEADER *new_dsk = make_dsk(6144);

    WT_PAGE_DISAGG_INFO info = make_disagg_info(14606, 200, 100, 0x1234, 1);
    WT_PAGE_MODIFY mod;
    WT_PAGE page = make_replaced_page(nullptr, &info, &mod, new_dsk);

    REQUIRE(
      __ut_evict_page_disagg_image(&page) == reinterpret_cast<const WT_PAGE_HEADER *>(new_dsk));

    free(new_dsk);
}

TEST_CASE(
  "Victim cache source: a single-block replacement without a retained image is not "
  "cacheable",
  "[evict][disagg_cache]")
{
    WT_PAGE_HEADER *dsk = make_dsk(4096);
    WT_PAGE_DISAGG_INFO info = make_disagg_info(14606, 200, 100, 0x1234, 1);
    WT_PAGE_MODIFY mod;
    /* Written to storage, image not retained: the retained-image field is null. */
    WT_PAGE page = make_replaced_page(dsk, &info, &mod, nullptr);

    REQUIRE(__ut_evict_page_disagg_image(&page) == nullptr);

    free(dsk);
}

TEST_CASE(
  "Victim cache source: a page reconciled into a split is not cacheable", "[evict][disagg_cache]")
{
    WT_PAGE_HEADER *dsk = make_dsk(4096);
    WT_PAGE_DISAGG_INFO info = make_disagg_info(14606, 200, 100, 0x1234, 0);
    WT_PAGE_MODIFY mod;
    WT_PAGE page = make_non_replace_page(dsk, &info, &mod, WT_PM_REC_MULTIBLOCK);

    REQUIRE(__ut_evict_page_disagg_image(&page) == nullptr);

    free(dsk);
}

TEST_CASE(
  "Victim cache source: a page reconciled as empty is not cacheable", "[evict][disagg_cache]")
{
    WT_PAGE_HEADER *dsk = make_dsk(4096);
    WT_PAGE_DISAGG_INFO info = make_disagg_info(14606, 200, 100, 0x1234, 0);
    WT_PAGE_MODIFY mod;
    WT_PAGE page = make_non_replace_page(dsk, &info, &mod, WT_PM_REC_EMPTY);

    REQUIRE(__ut_evict_page_disagg_image(&page) == nullptr);

    free(dsk);
}
