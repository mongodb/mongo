/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>
#include <array>
#include <utility>
#include <string_view>

#include "wt_internal.h"

#include "wrappers/connection_wrapper.h"
#include "utils.h"

/*
 * Unit tests for disaggregated storage metadata parsing.
 */

struct disagg_fixture {
    enum Config { CHECKPOINT, TIMESTAMP, KEY_PROVIDER };

    using kv_t = std::pair<std::string, std::string>;
    const std::array<kv_t, 3> config = {std::make_pair("checkpoint",
                                          R"((WiredTigerCheckpoint.1=(
                addr="00c025808282bd21596019",
                order=1,
                time=1766470626,
                size=101,
                newest_start_durable_ts=0,
                oldest_start_ts=0,
                newest_txn=0,
                newest_stop_durable_ts=0,
                newest_stop_ts=-1,
                newest_stop_txn=-11,
                prepare=0,
                write_gen=3,
                run_write_gen=1,
                next_page_id=102
                )
            ))"),
      std::make_pair("timestamp", "c0ffee12"), /* hex number */
      std::make_pair("key_provider",
        R"((page.1=(
                  page_id=1,
                  lsn=123
                  ),
                version=1
            ))")};

    template <typename... Args>
    std::string
    join_cfg(Args... indices)
    {
        std::string result;
        ((result += config[indices].first + "=" + config[indices].second + ","), ...);
        return result;
    }

    connection_wrapper conn;
    WT_SESSION_IMPL *session = nullptr;

    disagg_fixture() : conn(DB_HOME, "create,in_memory")
    {
        session = conn.create_session();
        REQUIRE(session != nullptr);

        REQUIRE(config.size() == KEY_PROVIDER + 1);
    }
};

TEST_CASE_METHOD(disagg_fixture, "Parse metadata", "[disagg]")
{
    SECTION("All fields present")
    {
        const std::string metadata_str = join_cfg(CHECKPOINT, TIMESTAMP, KEY_PROVIDER);

        WT_ITEM metadata_buf{};
        metadata_buf.data = (const void *)metadata_str.data();
        metadata_buf.size = metadata_str.length();
        WT_DISAGG_METADATA metadata{};

        const auto ret = __wti_disagg_parse_meta(session, &metadata_buf, &metadata);
        REQUIRE(ret == 0);

        REQUIRE(config[CHECKPOINT].second ==
          std::string_view(metadata.checkpoint, metadata.checkpoint_len));

        const uint64_t expected_timestamp = std::stoull(config[TIMESTAMP].second, nullptr, 16);
        REQUIRE(expected_timestamp == metadata.checkpoint_timestamp);

        REQUIRE(config[KEY_PROVIDER].second ==
          std::string_view(metadata.key_provider, metadata.key_provider_len));
    }

    SECTION("Key provider missing")
    {
        const std::string metadata_str = join_cfg(CHECKPOINT, TIMESTAMP);

        WT_ITEM metadata_buf{};
        metadata_buf.data = (const void *)metadata_str.data();
        metadata_buf.size = metadata_str.length();
        WT_DISAGG_METADATA metadata{};

        const auto ret = __wti_disagg_parse_meta(session, &metadata_buf, &metadata);
        REQUIRE(ret == 0);

        REQUIRE(config[CHECKPOINT].second ==
          std::string_view(metadata.checkpoint, metadata.checkpoint_len));

        const uint64_t expected_timestamp = std::stoull(config[TIMESTAMP].second, nullptr, 16);
        REQUIRE(expected_timestamp == metadata.checkpoint_timestamp);

        REQUIRE(metadata.key_provider == nullptr);
        REQUIRE(metadata.key_provider_len == 0);
    }

    SECTION("Missing fields")
    {
        const std::string metadata_str = join_cfg(CHECKPOINT, KEY_PROVIDER);

        WT_ITEM metadata_buf{};
        metadata_buf.data = (const void *)metadata_str.data();
        metadata_buf.size = metadata_str.length();
        WT_DISAGG_METADATA metadata{};

        const auto ret = __wti_disagg_parse_meta(session, &metadata_buf, &metadata);
        REQUIRE(ret == EINVAL);
    }

    SECTION("Null metadata")
    {
        WT_ITEM metadata_buf{};
        WT_DISAGG_METADATA metadata{};

        const auto ret = __wti_disagg_parse_meta(session, &metadata_buf, &metadata);
        REQUIRE(ret == EINVAL);
    }

    SECTION("Empty metadata")
    {
        WT_ITEM metadata_buf{};
        metadata_buf.data = (const void *)"";
        metadata_buf.size = 0;
        WT_DISAGG_METADATA metadata{};

        const auto ret = __wti_disagg_parse_meta(session, &metadata_buf, &metadata);
        REQUIRE(ret == EINVAL);
    }

    SECTION("Length limited")
    {
        const std::string metadata_str = "checkpoint=(),timestamp=c0ffee12";

        WT_ITEM metadata_buf{};
        metadata_buf.data = (const void *)metadata_str.data();
        metadata_buf.size = metadata_str.length() - 2; /* truncate the last 2 bytes */
        WT_DISAGG_METADATA metadata{};

        const auto ret = __wti_disagg_parse_meta(session, &metadata_buf, &metadata);
        REQUIRE(ret == 0);
        REQUIRE(std::string_view("()", 2) ==
          std::string_view(metadata.checkpoint, metadata.checkpoint_len));
        const uint64_t expected_timestamp = std::stoull("c0ffee", nullptr, 16);
        REQUIRE(expected_timestamp == metadata.checkpoint_timestamp);
        REQUIRE(metadata.key_provider == nullptr);
        REQUIRE(metadata.key_provider_len == 0);
    }
}

TEST_CASE_METHOD(disagg_fixture, "Parse crypt key metadata", "[disagg]")
{
    SECTION("Well-formed")
    {
        WT_DISAGG_METADATA metadata{};
        metadata.checkpoint = nullptr;
        metadata.checkpoint_len = 0;
        metadata.checkpoint_timestamp = 0;
        metadata.key_provider = config[KEY_PROVIDER].second.data();
        metadata.key_provider_len = config[KEY_PROVIDER].second.length();

        uint64_t page_id = 0, lsn = 0;
        const auto ret = __wti_disagg_parse_crypt_meta(session, &metadata, &page_id, &lsn);
        REQUIRE(ret == 0);

        REQUIRE(page_id == 1);
        REQUIRE(lsn == 123);
    }

    SECTION("Malformed")
    {
        const std::string_view invalid_meta[] = {
          "(page.1=(page_id=aaa,lsn=123),version=1)", /* invalid page_id */
          "(page.1=(page_id=123,lsn=123),version=1)", /* page_id out of range */
          "(page.1=(page_id=1,lsn=aaa),version=1)",   /* invalid lsn */
          "(page.1=(page_id=1),version=1)",           /* missing lsn */
          "(page.1=(lsn=123),version=1)",             /* missing page_id */
          "(page.1=(page_id=1,lsn=123))",             /* missing version */
          "(page.1=(page_id=1,lsn=123),version=2)",   /* unsupported version */
          "invalid_format"                            /* completely invalid */
        };

        for (const auto &meta : invalid_meta) {
            WT_DISAGG_METADATA metadata{};
            metadata.checkpoint = nullptr;
            metadata.checkpoint_len = 0;
            metadata.checkpoint_timestamp = 0;
            metadata.key_provider = meta.data();
            metadata.key_provider_len = meta.length();

            uint64_t page_id = 0, lsn = 0;
            const auto ret = __wti_disagg_parse_crypt_meta(session, &metadata, &page_id, &lsn);
            REQUIRE(ret == EINVAL);
        }
    }
}
