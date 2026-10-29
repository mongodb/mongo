/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>
#include <cstdint>
#include <memory>

#include "wt_internal.h"

#include "wrappers/mock_session.h"
#include "utils.h"

/*
 * checkpoint_meta_version_fixture
 *     Test fixture for checkpoint metadata version tests.
 */
struct checkpoint_meta_version_fixture {
    std::shared_ptr<mock_session> session_wrapper;
    WT_SESSION_IMPL *session = nullptr;

    checkpoint_meta_version_fixture() : session_wrapper(mock_session::build_test_mock_session())
    {
        session = session_wrapper->get_wt_session_impl();
        REQUIRE(session != nullptr);
    }
};

TEST_CASE_METHOD(checkpoint_meta_version_fixture,
  "checkpoint_meta_version: version number validation", "[checkpoint_meta_version]")
{
    uint32_t version, compatible_version;
    int ret;

    SECTION("parse version 1, compatible_version 1")
    {
        const char *meta_str =
          "metadata_lsn=123456789,metadata_checksum=0xDEADBEEF,version=1,compatible_version=1";

        ret = __ut_disagg_validate_checkpoint_meta_version(
          session, meta_str, &version, &compatible_version);

        REQUIRE(ret == 0);
        REQUIRE(version == 1);
        REQUIRE(compatible_version == 1);
    }

    SECTION("backward compatibility - missing version fields defaults to 1,1")
    {
        /* Old-style config string without version fields */
        const char *meta_str = "metadata_lsn=9999,metadata_checksum=0xABCD";

        ret = __ut_disagg_validate_checkpoint_meta_version(
          session, meta_str, &version, &compatible_version);

        REQUIRE(ret == 0);
        REQUIRE(version == WT_DISAGG_CHECKPOINT_META_VERSION_DEFAULT);
        REQUIRE(compatible_version == WT_DISAGG_CHECKPOINT_META_VERSION_DEFAULT);
    }

    SECTION("parse with only version field")
    {
        const char *meta_str = "version=1,metadata_lsn=5555";

        ret = __ut_disagg_validate_checkpoint_meta_version(
          session, meta_str, &version, &compatible_version);

        REQUIRE(ret == 0);
        REQUIRE(version == 1);
        REQUIRE(compatible_version == WT_DISAGG_CHECKPOINT_META_VERSION_DEFAULT);
    }

    SECTION("parse with only compatible_version field")
    {
        const char *meta_str = "compatible_version=1,metadata_lsn=7777";

        ret = __ut_disagg_validate_checkpoint_meta_version(
          session, meta_str, &version, &compatible_version);

        REQUIRE(ret == 0);
        REQUIRE(version == WT_DISAGG_CHECKPOINT_META_VERSION_DEFAULT);
        REQUIRE(compatible_version == 1);
    }

    SECTION("forward compatibility error - incompatible version")
    {
        /* Requires a reader newer than this build (version 2). */
        const char *meta_str =
          "metadata_lsn=111111,metadata_checksum=0xCAFEBABE,version=3,compatible_version=3";

        ret = __ut_disagg_validate_checkpoint_meta_version(
          session, meta_str, &version, &compatible_version);

        /* Should fail with ENOTSUP - reader version is too old */
        REQUIRE(ret == ENOTSUP);
    }

    SECTION("compatible_version newer than version is illegal")
    {
        /* compatible_version should never be greater than version */
        const char *meta_str = "version=0,compatible_version=1,metadata_lsn=11111";

        ret = __ut_disagg_validate_checkpoint_meta_version(
          session, meta_str, &version, &compatible_version);

        /* This is an invalid configuration that should fail */
        REQUIRE(ret == EINVAL);
    }

    SECTION("multiple incompatible versions all fail")
    {
        const char *incompatible_configs[] = {
          "version=3,compatible_version=3",
          "version=6,compatible_version=3",
          "version=5,compatible_version=10",
        };

        for (size_t i = 0; i < sizeof(incompatible_configs) / sizeof(incompatible_configs[0]);
          ++i) {
            ret = __ut_disagg_validate_checkpoint_meta_version(
              session, incompatible_configs[i], &version, &compatible_version);
            REQUIRE(ret == ENOTSUP);
        }
    }

    SECTION("checkpoint at compatible_version 1 is accepted")
    {
        /*
         * An escaped stable-tombstone checkpoint records compatible_version 1 (readable by every
         * reader). The parser only enforces the version gate; the encoding->compatible_version
         * mapping is exercised writer-side by test_layered_tombstone_version_gate.py.
         */
        const char *meta_str = "metadata_lsn=4242,version=1,compatible_version=1";

        ret = __ut_disagg_validate_checkpoint_meta_version(
          session, meta_str, &version, &compatible_version);

        REQUIRE(ret == 0);
        REQUIRE(version == 1);
        REQUIRE(compatible_version == WT_DISAGG_CHECKPOINT_META_COMPATIBLE_VERSION);
    }

    SECTION("checkpoint at this build's maximum reader version is accepted")
    {
        /*
         * An unescaped stable-tombstone checkpoint raises the minimum reader version to the
         * stable-unencoded compatible version, which equals this build's maximum reader version
         * WT_DISAGG_CHECKPOINT_META_VERSION -- the accept side of the version gate. The reject side
         * (compatible_version one greater) is covered by the incompatible-version sections above.
         */
        /* Built from the constants so a future version bump keeps testing this build's gate. */
        const std::string meta_str =
          "metadata_lsn=4243,version=" + std::to_string(WT_DISAGG_CHECKPOINT_META_VERSION) +
          ",compatible_version=" +
          std::to_string(WT_DISAGG_CHECKPOINT_META_VERSION_STABLE_UNENCODED);

        ret = __ut_disagg_validate_checkpoint_meta_version(
          session, meta_str.c_str(), &version, &compatible_version);

        REQUIRE(ret == 0);
        REQUIRE(version == (uint32_t)WT_DISAGG_CHECKPOINT_META_VERSION);
        REQUIRE(compatible_version == WT_DISAGG_CHECKPOINT_META_VERSION_STABLE_UNENCODED);
    }
}
