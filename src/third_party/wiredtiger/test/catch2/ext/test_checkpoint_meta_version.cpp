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
        /* Version requires reader version 2 but we only have version 1 */
        const char *meta_str =
          "metadata_lsn=111111,metadata_checksum=0xCAFEBABE,version=2,compatible_version=2";

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
}
