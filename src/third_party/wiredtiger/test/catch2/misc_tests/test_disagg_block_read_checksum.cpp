/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>
#include <cstdint>
#include <cstring>
#include <vector>

#include "wt_internal.h"
#include "../wrappers/connection_wrapper.h"

/*
 * A disaggregated read is handed a base page and the deltas written on top of it, and must decide
 * that it received all of them, in order, and intact. It does that with a chain of checksums: the
 * address cookie in the internal page names the newest block's checksum, and each block names its
 * predecessor's in previous_checksum. A page rewritten "offline" has no predecessor, so it reuses
 * previous_checksum to carry the checksum the internal page still references.
 *
 * These tests forge the block images and serve them through a stub page log, because the chain is a
 * property of the bytes rather than of anything a workload can choose, and because the page log
 * that stores and returns offline-modified images is not available here.
 */

namespace {

/*
 * One page-log result image, keeping the flags a test needs to rewrite a header field and
 * re-checksum the image.
 */
struct block_image {
    std::vector<uint8_t> bytes;
    uint8_t flags;
    uint32_t checksum; /* The checksum written into the header. */

    WT_BLOCK_DISAGG_HEADER *
    header()
    {
        return (static_cast<WT_BLOCK_DISAGG_HEADER *>(WT_BLOCK_HEADER_REF(bytes.data())));
    }

    uint32_t
    size() const
    {
        return (static_cast<uint32_t>(bytes.size()));
    }

    /* Checksum the image the way the write path does, over the prefix or the whole image. */
    void
    recompute_checksum()
    {
        WT_BLOCK_DISAGG_HEADER *blk = header();

        blk->checksum = 0;
        checksum = __wt_checksum(bytes.data(),
          (flags & WT_BLOCK_DISAGG_DATA_CKSUM) ? size() : WT_MIN(size(), WT_BLOCK_COMPRESS_SKIP));
        blk->checksum = checksum;
    }

    /* Point this image at a different predecessor, keeping the image itself self-consistent. */
    void
    relink(uint32_t previous_checksum)
    {
        header()->previous_checksum = previous_checksum;
        recompute_checksum();
    }

    /* Damage the image, leaving the stored checksum stale. */
    void
    corrupt_body(size_t offset)
    {
        REQUIRE(offset >= WT_BLOCK_DISAGG_HEADER_BYTE_SIZE);
        REQUIRE(offset < bytes.size());
        bytes[offset] ^= 0xff;
    }
};

/*
 * Build one result image the way __wti_block_disagg_write_internal would, so that the reader's
 * checks see exactly the byte layout production produces.
 */
block_image
make_block_image(uint32_t size, uint8_t magic, uint32_t previous_checksum, uint8_t flags,
  uint8_t compatible_version = WT_BLOCK_DISAGG_COMPATIBLE_VERSION)
{
    block_image img;

    REQUIRE(size > WT_BLOCK_DISAGG_HEADER_BYTE_SIZE);
    img.bytes.assign(size, 0);
    img.flags = flags;

    /* Give the body a recognizable pattern so that flipping a byte in it actually changes it. */
    for (uint32_t i = WT_BLOCK_DISAGG_HEADER_BYTE_SIZE; i < size; i++)
        img.bytes[i] = static_cast<uint8_t>(i * 31 + 7);

    WT_PAGE_HEADER *dsk = reinterpret_cast<WT_PAGE_HEADER *>(img.bytes.data());
    dsk->mem_size = size;

    WT_BLOCK_DISAGG_HEADER host;
    memset(&host, 0, sizeof(host));
    host.magic = magic;
    host.version = WT_BLOCK_DISAGG_VERSION;
    host.compatible_version = compatible_version;
    host.header_size = WT_BLOCK_DISAGG_HEADER_BYTE_SIZE;
    host.previous_checksum = previous_checksum;
    host.flags = flags;
    host.checksum = 0;
    __wt_block_disagg_header_byteswap_copy(&host, img.header());

    img.recompute_checksum();
    return (img);
}

/*
 * Build a base page with delta_count deltas on top, each naming its predecessor. Returns the
 * checksum the address cookie must carry, which is the newest delta's.
 */
uint32_t
build_chain(std::vector<block_image> &out, size_t delta_count, uint8_t flags, uint32_t size = 512)
{
    out.clear();
    out.push_back(make_block_image(size, WT_BLOCK_DISAGG_MAGIC_BASE, 0, flags));
    for (size_t i = 0; i < delta_count; i++)
        out.push_back(
          make_block_image(size, WT_BLOCK_DISAGG_MAGIC_DELTA, out.back().checksum, flags));
    return (out.back().checksum);
}

/*
 * A page rewritten offline: no predecessor, so previous_checksum carries the checksum the internal
 * page still references.
 */
block_image
make_offline_modified_page(uint32_t original_checksum)
{
    return (make_block_image(512, WT_BLOCK_DISAGG_MAGIC_BASE, original_checksum,
      WT_BLOCK_DISAGG_DATA_CKSUM | WT_BLOCK_DISAGG_MODIFIED));
}

/*
 * What the stub page log hands back: the result images, and the metadata it reports alongside them.
 * The suite is single threaded, so a single pointer to the running test's state is enough.
 */
struct stub_page_log {
    std::vector<block_image> *blocks;
    uint64_t lsn;
    uint64_t backlink_lsn;
    uint64_t base_lsn;
    uint64_t base_checkpoint_id;
    uint64_t delta_count;
};

stub_page_log *g_page_log = nullptr;

int
stub_plh_get(WT_PAGE_LOG_HANDLE *, WT_SESSION *, uint64_t, uint64_t, WT_PAGE_LOG_GET_ARGS *args,
  WT_ITEM *results_array, uint32_t *results_count)
{
    uint32_t count = static_cast<uint32_t>(g_page_log->blocks->size());

    *results_count = count;
    for (uint32_t i = 0; i < count; i++) {
        block_image &img = (*g_page_log->blocks)[i];
        results_array[i].data = img.bytes.data();
        results_array[i].size = img.bytes.size();
        results_array[i].mem = nullptr;
        results_array[i].memsize = 0;
    }

    /* Fill only the output fields; the caller's requested LSN and flags stay as they are. */
    args->lsn = g_page_log->lsn;
    args->backlink_lsn = g_page_log->backlink_lsn;
    args->base_lsn = g_page_log->base_lsn;
    args->base_checkpoint_id = g_page_log->base_checkpoint_id;
    args->delta_count = g_page_log->delta_count;

    return (0);
}

/*
 * read_checksum_fixture --
 *     A real connection and session, because the read updates statistics and needs the process
 *     checksum function installed, over the smallest fabricated btree and block manager that reach
 *     the disaggregated read.
 */
struct read_checksum_fixture {
    connection_wrapper conn_wrapper;
    WT_CONNECTION_IMPL *conn;
    WT_SESSION_IMPL *session;
    WT_DATA_HANDLE *saved_dhandle;

    /* Value-initialized, so every field the read does not care about reads as zero. */
    WT_DATA_HANDLE dhandle{};
    WT_BTREE btree{};
    WT_BM bm{};
    WT_BLOCK_DISAGG block_disagg{};
    WT_PAGE_LOG_HANDLE plhandle{};

    WT_PAGE_BLOCK_META block_meta{};
    WT_ITEM results[WT_DELTA_LIMIT + 1];
    u_int results_count = 0;

    std::vector<block_image> blocks;
    stub_page_log page_log{};

    static constexpr uint64_t PAGE_ID = 7;
    static constexpr uint64_t LSN = 100;
    static constexpr uint64_t BASE_LSN = 50;

    read_checksum_fixture() : conn_wrapper("WT_TEST.block_disagg_read_checksum", "create")
    {
        memset(results, 0, sizeof(results));

        conn = conn_wrapper.get_wt_connection_impl();
        session = conn_wrapper.create_session();

        plhandle.plh_get = stub_plh_get;

        block_disagg.name = "test_disagg";
        block_disagg.tableid = 42;
        block_disagg.plhandle = &plhandle;
        bm.block = reinterpret_cast<WT_BLOCK *>(&block_disagg);

        btree.bm = &bm;
        btree.dhandle = &dhandle;
        dhandle.handle = &btree;

        /*
         * The session belongs to the connection, which closes it. Put the fabricated handle back
         * before that happens.
         */
        saved_dhandle = session->dhandle;
        session->dhandle = &dhandle;

        /* A failed checksum panics unless the session tolerates corruption, taking the binary. */
        F_SET(session, WT_SESSION_QUIET_CORRUPT_FILE);

        page_log.blocks = &blocks;
        page_log.lsn = LSN;
        g_page_log = &page_log;
    }

    ~read_checksum_fixture()
    {
        g_page_log = nullptr;

        /* The corruption flag lives on the connection and nothing in the read path clears it. */
        F_CLR_ATOMIC_32(conn, WT_CONN_DATA_CORRUPTION);

        session->dhandle = saved_dhandle;
    }

    /* The cumulative size the cookie must carry for a read that is not served from the cache. */
    uint32_t
    cumulative_size() const
    {
        uint32_t sum = 0;
        for (const block_image &img : blocks)
            sum += img.size();
        return (sum);
    }

    /*
     * The read asserts that the cookie's delta flag, base LSN, and size all agree with the chain it
     * is handed, so derive them from the chain; a caller passing a size is forging an image the
     * page service never stored.
     */
    int
    read_multiple(uint32_t cookie_checksum, uint32_t cookie_size = 0)
    {
        WT_BLOCK_DISAGG_ADDRESS_COOKIE cookie;
        uint8_t addr[WT_ADDR_MAX_COOKIE];
        uint8_t *endp = addr;
        bool has_deltas = blocks.size() > 1;

        memset(&cookie, 0, sizeof(cookie));
        cookie.page_id = PAGE_ID;
        cookie.flags = has_deltas ? WT_BLOCK_DISAGG_ADDR_FLAG_DELTA : 0;
        cookie.lsn = LSN;
        cookie.base_lsn = page_log.base_lsn = has_deltas ? BASE_LSN : 0;
        cookie.size = cookie_size != 0 ? cookie_size : cumulative_size();
        cookie.checksum = cookie_checksum;
        REQUIRE(__wti_block_disagg_addr_pack(session, &endp, &cookie) == 0);

        results_count = WT_DELTA_LIMIT + 1;
        return (__wti_block_disagg_read_multiple(&bm, session, &block_meta, addr,
          static_cast<size_t>(endp - addr), results, &results_count));
    }

    bool
    corruption_flagged() const
    {
        return (F_ISSET_ATOMIC_32(conn, WT_CONN_DATA_CORRUPTION));
    }
};

} // namespace

TEST_CASE_METHOD(read_checksum_fixture, "disagg block read: checksum validation", "[block_disagg]")
{
    SECTION("a base page whose checksum matches the cookie is accepted")
    {
        uint32_t cookie_checksum = build_chain(blocks, 0, WT_BLOCK_DISAGG_DATA_CKSUM);

        REQUIRE(read_multiple(cookie_checksum) == 0);
        CHECK_FALSE(corruption_flagged());
        CHECK(results_count == 1);
        CHECK(block_meta.page_id == PAGE_ID);
        CHECK(block_meta.disagg_lsn == LSN);
        CHECK(block_meta.delta_count == 0);
        CHECK(block_meta.base_lsn == 0);
        CHECK(block_meta.checksum == cookie_checksum);
    }

    SECTION("a delta chain is accepted when every block names its predecessor")
    {
        uint32_t cookie_checksum = build_chain(blocks, 3, WT_BLOCK_DISAGG_DATA_CKSUM);

        REQUIRE(read_multiple(cookie_checksum) == 0);
        CHECK_FALSE(corruption_flagged());
        CHECK(results_count == 4);
        CHECK(block_meta.delta_count == 3);
        CHECK(block_meta.base_lsn == BASE_LSN);
        CHECK(block_meta.checksum == cookie_checksum);
    }

    SECTION("a base page whose checksum does not match the cookie is rejected")
    {
        uint32_t cookie_checksum = build_chain(blocks, 0, WT_BLOCK_DISAGG_DATA_CKSUM);

        REQUIRE(read_multiple(cookie_checksum ^ 1) == WT_ERROR);
        CHECK(corruption_flagged());
    }

    SECTION("a newest delta whose checksum does not match the cookie is rejected")
    {
        uint32_t cookie_checksum = build_chain(blocks, 2, WT_BLOCK_DISAGG_DATA_CKSUM);

        REQUIRE(read_multiple(cookie_checksum ^ 1) == WT_ERROR);
        CHECK(corruption_flagged());
    }

    /*
     * Every block is individually well formed and the newest one matches the cookie, so only
     * walking the whole chain catches the broken link in the middle of it.
     */
    SECTION("a delta chain with a broken link below the newest block is rejected")
    {
        build_chain(blocks, 3, WT_BLOCK_DISAGG_DATA_CKSUM);
        blocks[2].relink(blocks[1].checksum ^ 1);
        blocks[3].relink(blocks[2].checksum);

        REQUIRE(read_multiple(blocks[3].checksum) == WT_ERROR);
        CHECK(corruption_flagged());
    }

    SECTION("a corrupt body is detected when the checksum covers the whole image")
    {
        uint32_t cookie_checksum = build_chain(blocks, 0, WT_BLOCK_DISAGG_DATA_CKSUM);
        blocks[0].corrupt_body(300);

        REQUIRE(read_multiple(cookie_checksum) == WT_ERROR);
        CHECK(corruption_flagged());
    }

    /*
     * Without a data checksum the stored checksum covers only the leading bytes, so damage past
     * that point is outside what the block manager promises to detect.
     */
    SECTION("a corrupt body beyond the checksum prefix is not detected")
    {
        uint32_t cookie_checksum = build_chain(blocks, 0, 0);
        blocks[0].corrupt_body(300);

        REQUIRE(read_multiple(cookie_checksum) == 0);
        CHECK_FALSE(corruption_flagged());
    }

    SECTION("a corrupt body within the checksum prefix is detected")
    {
        uint32_t cookie_checksum = build_chain(blocks, 0, 0);
        blocks[0].corrupt_body(WT_BLOCK_COMPRESS_SKIP - 8);

        REQUIRE(read_multiple(cookie_checksum) == WT_ERROR);
        CHECK(corruption_flagged());
    }

    /*
     * The rewritten image is not what was written to the page service, so its size is unrelated to
     * the cumulative size the cookie tracks.
     */
    SECTION("an offline-modified page whose previous checksum matches the cookie is accepted")
    {
        const uint32_t original_checksum = 0xabcd1234;
        blocks = {make_offline_modified_page(original_checksum)};
        page_log.delta_count = 2;

        REQUIRE(read_multiple(original_checksum, 4096) == 0);
        CHECK_FALSE(corruption_flagged());
        CHECK(block_meta.cumulative_size == 4096);

        /* The delta count comes from the page log rather than from the number of results. */
        CHECK(block_meta.delta_count == 2);

        /* The checksum the internal page references survives, so the page can be cached again. */
        CHECK(block_meta.checksum == original_checksum);
    }

    /*
     * The image is internally valid and describes some page, just not the one the internal page
     * asked for, which nothing catches until previous_checksum carries the original checksum.
     */
    SECTION(
      "an offline-modified page whose previous checksum does not match the cookie is "
      "rejected")
    {
        const uint32_t original_checksum = 0xabcd1234;
        blocks = {make_offline_modified_page(original_checksum)};

        REQUIRE(read_multiple(original_checksum ^ 1, 4096) == WT_ERROR);
        CHECK(corruption_flagged());
    }

    SECTION("an offline-modified page with a corrupt body is rejected")
    {
        const uint32_t original_checksum = 0xabcd1234;
        blocks = {make_offline_modified_page(original_checksum)};
        blocks[0].corrupt_body(300);

        REQUIRE(read_multiple(original_checksum, 4096) == WT_ERROR);
        CHECK(corruption_flagged());
    }
}

/*
 * The remaining header checks sit behind the checksum, so they are only reachable once a block has
 * verified.
 */
TEST_CASE_METHOD(
  read_checksum_fixture, "disagg block read: block header validation", "[block_disagg]")
{
    SECTION("a base page carrying the delta magic is rejected")
    {
        blocks = {
          make_block_image(512, WT_BLOCK_DISAGG_MAGIC_DELTA, 0, WT_BLOCK_DISAGG_DATA_CKSUM)};

        REQUIRE(read_multiple(blocks[0].checksum) == WT_ERROR);
        CHECK(corruption_flagged());
    }

    SECTION("a delta carrying the base magic is rejected")
    {
        build_chain(blocks, 1, WT_BLOCK_DISAGG_DATA_CKSUM);
        blocks[1].header()->magic = WT_BLOCK_DISAGG_MAGIC_BASE;
        blocks[1].recompute_checksum();

        REQUIRE(read_multiple(blocks[1].checksum) == WT_ERROR);
        CHECK(corruption_flagged());
    }

    SECTION("a block requiring a newer reader than this build is rejected")
    {
        blocks = {make_block_image(512, WT_BLOCK_DISAGG_MAGIC_BASE, 0, WT_BLOCK_DISAGG_DATA_CKSUM,
          WT_BLOCK_DISAGG_VERSION + 1)};

        REQUIRE(read_multiple(blocks[0].checksum) == WT_ERROR);
        CHECK(corruption_flagged());
    }
}
