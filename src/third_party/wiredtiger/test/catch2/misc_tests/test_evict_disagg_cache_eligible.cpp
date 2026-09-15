/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "wt_internal.h"
#include "../wrappers/connection_wrapper.h"

/*
 * These tests exercise the gate that decides whether a page may enter the victim cache and, if so,
 * which on-disk-format image matches the page's current block metadata.
 *
 * The gate returns a named reason rather than a bool, which is what lets these tests assert that a
 * page is rejected for the reason intended rather than for some earlier gate that happened to fire
 * first, and what lets them walk every reason without keeping a list that could fall behind the
 * enum.
 */

namespace {

/* Whether the stand-in page log reports its cache as available. */
bool cache_available = true;

WT_PAGE_HEADER *
make_dsk(uint32_t mem_size)
{
    WT_PAGE_HEADER *dsk;

    dsk = static_cast<WT_PAGE_HEADER *>(calloc(1, sizeof(WT_PAGE_HEADER)));
    REQUIRE(dsk != nullptr);
    dsk->mem_size = mem_size;
    return (dsk);
}

/*
 * Minimal page-log handle: eligibility only requires that the put function pointer is non-NULL and
 * that the handle reports caching as available.
 */
int
dummy_plh_cache_put(
  WT_PAGE_LOG_HANDLE *, WT_SESSION *, uint64_t, uint64_t, WT_PAGE_LOG_PUT_ARGS *, const WT_ITEM *)
{
    return (0);
}

bool
dummy_plh_cache_available(WT_PAGE_LOG_HANDLE *, WT_SESSION *)
{
    return (cache_available);
}

/*
 * eligibility_fixture --
 *     A real connection and session, plus the smallest fabricated btree, ref and page that let us
 *     call __evict_page_victim_cache_eligible(). The connection and session are real so that the
 *     statistics the gate updates are actually allocated and can be read back; a disaggregated
 *     btree backed by a page log is not something a unit test can stand up for real, so that part
 *     of the chain is built by hand. As constructed the page is eligible, and each test mutates
 *     exactly one field.
 */
struct eligibility_fixture {
    connection_wrapper conn_wrapper;
    WT_CONNECTION_IMPL *conn;
    WT_SESSION_IMPL *session;
    WT_DATA_HANDLE *saved_dhandle;

    /* Value-initialized, so every field the gate does not care about reads as zero. */
    WT_DATA_HANDLE dhandle{};
    WT_BTREE btree{};
    WT_BM bm{};
    WT_BLOCK_DISAGG block_disagg{};
    WT_PAGE_LOG_HANDLE plhandle{};
    WT_REF ref{};
    WT_PAGE page{};
    WT_PAGE_DISAGG_INFO disagg_info{};
    WT_PAGE_MODIFY modify{};
    WT_PAGE_HEADER *dsk;

    eligibility_fixture()
        : conn_wrapper("WT_TEST.evict_disagg_cache_eligible", "create,statistics=(all)")
    {
        cache_available = true;
        dsk = make_dsk(4096);

        conn = conn_wrapper.get_wt_connection_impl();
        session = conn_wrapper.create_session();

        /* Wire up the page-log handle. */
        plhandle.plh_cache_put = dummy_plh_cache_put;
        plhandle.plh_cache_available = dummy_plh_cache_available;

        /* Wire up the block manager -> block disagg -> page log chain. */
        block_disagg.plhandle = &plhandle;
        bm.block = reinterpret_cast<WT_BLOCK *>(&block_disagg);

        /* Default to a valid, eligible disaggregated leaf page. */
        F_SET(&btree, WT_BTREE_DISAGGREGATED);
        btree.storage_tier = WT_BTREE_STORAGE_TIER_NONE;
        btree.bm = &bm;
        btree.dhandle = &dhandle;

        dhandle.handle = &btree;
        dhandle.checkpoint = nullptr;

        /*
         * The session belongs to the connection, which closes it. Put the fabricated handle back
         * before that happens.
         */
        saved_dhandle = session->dhandle;
        session->dhandle = &dhandle;

        F_SET(&ref, WT_REF_FLAG_LEAF);
        ref.home = reinterpret_cast<WT_PAGE *>(0x1); /* Any non-NULL value means not root. */
        ref.page = &page;

        disagg_info.block_meta.page_id = 14606;
        page.dsk = dsk;
        page.disagg_info = &disagg_info;
    }

    ~eligibility_fixture()
    {
        session->dhandle = saved_dhandle;
        free(dsk);
    }

    /* Give the page a reconciliation result, as reconciliation itself would have left it. */
    void
    set_rec_result(uint8_t rec_result, void *disk_image = nullptr)
    {
        modify = {};
        modify.rec_result = rec_result;
        if (rec_result == WT_PM_REC_REPLACE)
            modify.mod_disk_image = disk_image;
        page.modify = &modify;
    }

    /* The connection statistic the cold-tier gate updates. */
    int64_t
    cold_not_cached() const
    {
        return (conn->stats[session->stat_conn_bucket]->block_cache_cold_not_cached);
    }

    WTI_EVICT_VICTIM_REASON
    check(const WT_PAGE_HEADER **diskp)
    {
        return (__ut_evict_page_victim_cache_eligible(session, &ref, diskp));
    }
};

/*
 * Mutate an otherwise-eligible fixture so the gate returns exactly this reason.
 *
 * This switch has no default label. The build turns an unhandled enumerator into an error on both
 * toolchains, so adding a reason to WTI_EVICT_VICTIM_REASON fails the build here until somebody
 * writes the state that provokes it. Because the test below walks the reasons by counting up to
 * WTI_EVICT_VICTIM_COUNT rather than from a list kept in this file, the new reason is then
 * exercised automatically. That is the whole drift guard: a gate cannot be added to the eligibility
 * check and left untested.
 */
void
provoke(eligibility_fixture &f, WTI_EVICT_VICTIM_REASON reason)
{
    switch (reason) {
    case WTI_EVICT_VICTIM_OK:
        /* The fixture is eligible as constructed. */
        break;
    case WTI_EVICT_VICTIM_NOT_DISAGG:
        F_CLR(&f.btree, WT_BTREE_DISAGGREGATED);
        break;
    case WTI_EVICT_VICTIM_CHECKPOINT_CURSOR:
        f.dhandle.checkpoint = "test_checkpoint";
        break;
    case WTI_EVICT_VICTIM_NO_BLOCK_MANAGER:
        f.btree.bm = nullptr;
        break;
    case WTI_EVICT_VICTIM_NO_PAGE_LOG:
        f.block_disagg.plhandle = nullptr;
        break;
    case WTI_EVICT_VICTIM_CACHE_UNAVAILABLE:
        /*
         * Unlike every other reason here this one is a property of the page log's current state,
         * not of the page: the same page is rejected now and admitted once capacity frees up.
         */
        cache_available = false;
        break;
    case WTI_EVICT_VICTIM_NOT_LEAF:
        F_CLR(&f.ref, WT_REF_FLAG_LEAF);
        F_SET(&f.ref, WT_REF_FLAG_INTERNAL);
        break;
    case WTI_EVICT_VICTIM_NO_DISAGG_INFO:
        f.page.disagg_info = nullptr;
        break;
    case WTI_EVICT_VICTIM_NO_IMAGE:
        /* A split leaves no single image that could stand for the page. */
        f.set_rec_result(WT_PM_REC_MULTIBLOCK);
        break;
    case WTI_EVICT_VICTIM_INVALID_PAGE_ID:
        f.disagg_info.block_meta.page_id = WT_BLOCK_INVALID_PAGE_ID;
        break;
    case WTI_EVICT_VICTIM_ROOT:
        f.ref.home = nullptr;
        break;
    case WTI_EVICT_VICTIM_COLD_TIER:
        f.btree.storage_tier = WT_BTREE_STORAGE_TIER_COLD;
        break;
    case WTI_EVICT_VICTIM_COUNT:
        /* Not a reason; the loop below never reaches it. */
        REQUIRE(false);
        break;
    }
}

} // namespace

TEST_CASE("Victim cache eligibility: every reason is produced by exactly its own state",
  "[evict][disagg_cache]")
{
    /*
     * Counting to WTI_EVICT_VICTIM_COUNT rather than listing the reasons is deliberate: a reason
     * added to the enum is walked here without anybody remembering to add it.
     */
    for (int i = 0; i < WTI_EVICT_VICTIM_COUNT; i++) {
        auto expected = static_cast<WTI_EVICT_VICTIM_REASON>(i);

        eligibility_fixture f;
        provoke(f, expected);

        /* Pre-seed with a non-NULL image: a rejection must clear it, not leave it alone. */
        const WT_PAGE_HEADER *disk_image = f.dsk;
        WTI_EVICT_VICTIM_REASON reason = f.check(&disk_image);

        INFO("reason " << i << ": " << __ut_evict_page_victim_cache_reason_str(expected));

        /*
         * Equality, not merely "rejected": each single-field mutation must trip its own gate and no
         * earlier one, which pins the order the gates run in as well as the gates themselves.
         */
        REQUIRE(reason == expected);

        if (expected == WTI_EVICT_VICTIM_OK)
            REQUIRE(disk_image == f.dsk);
        else
            REQUIRE(disk_image == nullptr);
    }
}

TEST_CASE("Victim cache eligibility: a dirty page that was never reconciled is eligible",
  "[evict][disagg_cache]")
{
    /*
     * The gate used to reject any page that was still dirty. Nothing about a dirty page makes its
     * own image disagree with its block metadata until a reconciliation has run, so it is
     * cacheable; this is the behavior change with the widest reach and it is pinned here.
     */
    eligibility_fixture f;
    f.set_rec_result(0);

    const WT_PAGE_HEADER *disk_image = nullptr;
    REQUIRE(f.check(&disk_image) == WTI_EVICT_VICTIM_OK);
    REQUIRE(disk_image == f.dsk);
}

TEST_CASE("Victim cache eligibility: a retained replacement image is used, not the page's own",
  "[evict][disagg_cache]")
{
    /*
     * The gate resolves the image rather than handing back page->dsk. Using the page's own image
     * here would publish stale content under the block metadata's newer identity, which is the
     * stale-image bug this gate was reworked to prevent.
     */
    eligibility_fixture f;
    WT_PAGE_HEADER *new_dsk = make_dsk(6144);
    f.set_rec_result(WT_PM_REC_REPLACE, new_dsk);

    const WT_PAGE_HEADER *disk_image = nullptr;
    REQUIRE(f.check(&disk_image) == WTI_EVICT_VICTIM_OK);
    REQUIRE(disk_image == reinterpret_cast<const WT_PAGE_HEADER *>(new_dsk));
    REQUIRE(disk_image != f.dsk);

    free(new_dsk);
}

TEST_CASE("Victim cache eligibility: a replacement that retained no image is rejected",
  "[evict][disagg_cache]")
{
    /*
     * Written to storage, image not retained. The gate used to accept this page and cache
     * page->dsk, which no longer describes it.
     */
    eligibility_fixture f;
    f.set_rec_result(WT_PM_REC_REPLACE, nullptr);

    const WT_PAGE_HEADER *disk_image = f.dsk;
    REQUIRE(f.check(&disk_image) == WTI_EVICT_VICTIM_NO_IMAGE);
    REQUIRE(disk_image == nullptr);
}

TEST_CASE("Victim cache eligibility: rejecting a cold page is counted", "[evict][disagg_cache]")
{
    /* The rejection is only visible to an operator through this statistic. */
    eligibility_fixture f;
    f.btree.storage_tier = WT_BTREE_STORAGE_TIER_COLD;

    int64_t before = f.cold_not_cached();

    const WT_PAGE_HEADER *disk_image = f.dsk;
    REQUIRE(f.check(&disk_image) == WTI_EVICT_VICTIM_COLD_TIER);
    REQUIRE(disk_image == nullptr);
    REQUIRE(f.cold_not_cached() == before + 1);
}

TEST_CASE(
  "Victim cache eligibility: every reason has a distinct description", "[evict][disagg_cache]")
{
    /*
     * The descriptions are what a verbose log shows, so a reason that was added to the enum but
     * given no wording of its own would read as another reason's.
     */
    std::vector<std::string> seen;

    for (int i = 0; i < WTI_EVICT_VICTIM_COUNT; i++) {
        std::string description =
          __ut_evict_page_victim_cache_reason_str(static_cast<WTI_EVICT_VICTIM_REASON>(i));

        INFO("reason " << i << ": " << description);
        REQUIRE(description != "unknown");
        REQUIRE(std::find(seen.begin(), seen.end(), description) == seen.end());

        seen.push_back(description);
    }
}
