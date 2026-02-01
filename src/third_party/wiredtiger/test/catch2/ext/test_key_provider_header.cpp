/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */
#include "wt_internal.h"

#include <catch2/catch.hpp>
#include "wrappers/mock_session.h"

/* Fixture to initialize the kp header. */
struct kp_header_fixture {
    WT_CRYPT_KEYS crypt;
    std::shared_ptr<mock_session> session;
    const std::string test_string;

    WT_SESSION_IMPL *session_impl;

    kp_header_fixture()
        : crypt({}), session(mock_session::build_test_mock_session()), test_string("hello")
    {
        // Initialize the checksum function if not already initialized.
        if (__wt_process.checksum == nullptr)
            __wt_process.checksum = wiredtiger_crc32c_func();

        session_impl = session->get_wt_session_impl();
        kp_crypt_key_buffer(session_impl, test_string);
    }

    ~kp_header_fixture()
    {
        __wt_buf_free(session_impl, &crypt.keys);
    }

    void
    kp_crypt_key_buffer(WT_SESSION_IMPL *session, const std::string &str)
    {
        /* Allocate a buffer and make sure we leave enough space for the header at the start. */
        REQUIRE(__wt_buf_initsize(session, &crypt.keys, sizeof(WT_CRYPT_HEADER) + str.size()) == 0);
        memcpy(((uint8_t *)(crypt.keys.data) + sizeof(WT_CRYPT_HEADER)), str.data(), str.size());
        crypt.keys.size = str.size();
        crypt.keys.data = (uint8_t *)(crypt.keys.data) + sizeof(WT_CRYPT_HEADER);
    }

    void
    kp_copy_crypt_key_buffer(WT_CRYPT_HEADER *write_crypt_header)
    {
        WT_CRYPT_HEADER *crypt_header;
        __ut_disagg_get_crypt_header(&crypt.keys, &crypt_header);

        /* Prepare the header for validation. */
        memcpy(write_crypt_header, crypt_header, sizeof(WT_CRYPT_HEADER));
        __wt_crypt_header_byteswap(write_crypt_header);
        write_crypt_header->checksum = 0;
    }
};

TEST_CASE_METHOD(
  kp_header_fixture, "Validate crypt header offsets and size", "[key_provider_header]")
{
    REQUIRE(sizeof(WT_CRYPT_HEADER) == 16);
    REQUIRE(offsetof(WT_CRYPT_HEADER, signature) == 0);
    REQUIRE(offsetof(WT_CRYPT_HEADER, version) == 4);
    REQUIRE(offsetof(WT_CRYPT_HEADER, compatible_version) == 5);
    REQUIRE(offsetof(WT_CRYPT_HEADER, header_size) == 6);
    REQUIRE(offsetof(WT_CRYPT_HEADER, unused) == 7);
    REQUIRE(offsetof(WT_CRYPT_HEADER, crypt_size) == 8);
    REQUIRE(offsetof(WT_CRYPT_HEADER, checksum) == 12);
}

TEST_CASE_METHOD(kp_header_fixture, "Key provider set header function", "[key_provider_header]")
{
    SECTION("Validate key provider pack")
    {
        __ut_disagg_set_crypt_header(session->get_wt_session_impl(), &crypt);

        /* Validate crypt header information. */
        WT_CRYPT_HEADER *write_crypt_header;
        __ut_disagg_get_crypt_header(&crypt.keys, &write_crypt_header);
        REQUIRE(write_crypt_header->signature == WT_CRYPT_HEADER_SIGNATURE);
        REQUIRE(write_crypt_header->version == WT_CRYPT_HEADER_VERSION);
        REQUIRE(write_crypt_header->compatible_version == WT_CRYPT_HEADER_COMPATIBLE_VERSION);
        REQUIRE(write_crypt_header->header_size == sizeof(WT_CRYPT_HEADER));
        REQUIRE(write_crypt_header->crypt_size == test_string.size());
        REQUIRE(crypt.keys.size == test_string.size() + sizeof(WT_CRYPT_HEADER));

        uint32_t expected_checksum = write_crypt_header->checksum;
        write_crypt_header->checksum = 0;
        REQUIRE(expected_checksum == __wt_checksum(crypt.keys.data, crypt.keys.size));
    }
    __wt_buf_free(session_impl, &crypt.keys);
}

TEST_CASE_METHOD(
  kp_header_fixture, "Key provider: validate crypt function", "[key_provider_header]")
{
    /* Set the header inside the crypt struct. */
    __ut_disagg_set_crypt_header(session_impl, &crypt);

    WT_CRYPT_HEADER *read_crypt_header;
    SECTION("Test key provider basic unpack")
    {
        /* Fetch the baseline header before performing read. */
        WT_CRYPT_HEADER write_crypt_header;
        kp_copy_crypt_key_buffer(&write_crypt_header);
        REQUIRE(__ut_disagg_validate_crypt(session_impl, &crypt.keys, &read_crypt_header) == 0);

        /* Validate that before and after validation should not change the header. */
        REQUIRE(memcmp(&write_crypt_header, read_crypt_header, sizeof(WT_CRYPT_HEADER)) == 0);
    }

    SECTION("Test key provider header version")
    {
        /* Fetch the baseline header before performing read. */
        WT_CRYPT_HEADER write_crypt_header;
        kp_copy_crypt_key_buffer(&write_crypt_header);
        write_crypt_header.version = 2;

        WT_CRYPT_HEADER *header = (WT_CRYPT_HEADER *)crypt.keys.data;
        header->compatible_version = 1;
        header->version = 2;
        header->checksum = 0;
        header->checksum = __wt_checksum(crypt.keys.data, crypt.keys.size);
        REQUIRE(__ut_disagg_validate_crypt(session_impl, &crypt.keys, &read_crypt_header) == 0);

        /* Validate that before and after validation should not change the header. */
        REQUIRE(memcmp(&write_crypt_header, read_crypt_header, sizeof(WT_CRYPT_HEADER)) == 0);
    }

    SECTION("Test key provider header compatibility issue")
    {
        WT_CRYPT_HEADER *header = (WT_CRYPT_HEADER *)crypt.keys.data;
        header->compatible_version = 2;
        header->version = 1;
        header->checksum = 0;
        header->checksum = __wt_checksum(crypt.keys.data, crypt.keys.size);
        REQUIRE(
          __ut_disagg_validate_crypt(session_impl, &crypt.keys, &read_crypt_header) == ENOTSUP);
    }

    SECTION("Test key provider item size smaller than expected")
    {
        crypt.keys.size = 1;
        REQUIRE(__ut_disagg_validate_crypt(session_impl, &crypt.keys, &read_crypt_header) == EIO);
    }

    SECTION("Test key provider header size smaller than expected")
    {
        WT_CRYPT_HEADER *header = (WT_CRYPT_HEADER *)crypt.keys.data;
        header->header_size = 10;
        header->checksum = 0;
        header->checksum = __wt_checksum(crypt.keys.data, crypt.keys.size);
        REQUIRE(__ut_disagg_validate_crypt(session_impl, &crypt.keys, &read_crypt_header) == EIO);
    }

    SECTION("Test key provider header mismatch size")
    {
        WT_CRYPT_HEADER *header = (WT_CRYPT_HEADER *)crypt.keys.data;
        header->header_size = 50;
        header->checksum = 0;
        header->checksum = __wt_checksum(crypt.keys.data, crypt.keys.size);
        REQUIRE(__ut_disagg_validate_crypt(session_impl, &crypt.keys, &read_crypt_header) == EIO);
    }

    SECTION("Test key provider header mismatch checksum")
    {
        WT_CRYPT_HEADER *header = (WT_CRYPT_HEADER *)crypt.keys.data;
        header->checksum = 123;
        REQUIRE(__ut_disagg_validate_crypt(session_impl, &crypt.keys, &read_crypt_header) == EIO);
    }
}
