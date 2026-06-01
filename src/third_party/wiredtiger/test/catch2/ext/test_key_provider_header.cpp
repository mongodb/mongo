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

/* Hand-craft a header at the front of key_item. */
static void
build_crypt_page(WT_ITEM *key_item, uint8_t version, uint8_t compatible_version,
  uint8_t header_size, uint64_t timestamp, size_t payload_size)
{
    WT_CRYPT_HEADER local = {};
    local.signature = WT_CRYPT_HEADER_SIGNATURE;
    local.version = version;
    local.compatible_version = compatible_version;
    local.header_size = header_size;
    local.crypt_size = (uint32_t)payload_size;
    local.timestamp = timestamp;

    __wt_crypt_header_byteswap(&local);
    REQUIRE(WT_MIN((size_t)header_size, sizeof(WT_CRYPT_HEADER)) <= key_item->size);
    memcpy((void *)key_item->data, &local, WT_MIN((size_t)header_size, sizeof(WT_CRYPT_HEADER)));

    uint32_t cksum = __wt_checksum(key_item->data, key_item->size);
#ifdef WORDS_BIGENDIAN
    cksum = __wt_bswap32(cksum);
#endif
    ((WT_CRYPT_HEADER *)key_item->data)->checksum = cksum;
}

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
        /* Prepare the header for validation. */
        memcpy(write_crypt_header, crypt.keys.data, sizeof(WT_CRYPT_HEADER));
        __wt_crypt_header_byteswap(write_crypt_header);
        write_crypt_header->checksum = 0;
    }
};

TEST_CASE_METHOD(
  kp_header_fixture, "Validate crypt header offsets and size", "[key_provider_header]")
{
    REQUIRE(sizeof(WT_CRYPT_HEADER) == 24);
    REQUIRE(offsetof(WT_CRYPT_HEADER, signature) == 0);
    REQUIRE(offsetof(WT_CRYPT_HEADER, version) == 4);
    REQUIRE(offsetof(WT_CRYPT_HEADER, compatible_version) == 5);
    REQUIRE(offsetof(WT_CRYPT_HEADER, header_size) == 6);
    REQUIRE(offsetof(WT_CRYPT_HEADER, unused) == 7);
    REQUIRE(offsetof(WT_CRYPT_HEADER, crypt_size) == 8);
    REQUIRE(offsetof(WT_CRYPT_HEADER, checksum) == 12);
    REQUIRE(WT_CRYPT_HEADER_MIN_SIZE == 16);
}

TEST_CASE_METHOD(kp_header_fixture, "Key provider set header function", "[key_provider_header]")
{
    SECTION("Validate key provider pack")
    {
        __ut_disagg_set_crypt_header(session->get_wt_session_impl(), &crypt);

        /* Validate crypt header information. */
        WT_CRYPT_HEADER *write_crypt_header = (WT_CRYPT_HEADER *)crypt.keys.data;
        REQUIRE(write_crypt_header->signature == WT_CRYPT_HEADER_SIGNATURE);
        REQUIRE(write_crypt_header->version == WT_CRYPT_HEADER_VERSION);
        REQUIRE(write_crypt_header->compatible_version == WT_CRYPT_HEADER_COMPATIBLE_VERSION);
        REQUIRE(write_crypt_header->header_size == sizeof(WT_CRYPT_HEADER));
        REQUIRE(write_crypt_header->crypt_size == test_string.size());
        REQUIRE(write_crypt_header->timestamp == 0);
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

    WT_CRYPT_HEADER *read_crypt_header = nullptr;
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
        write_crypt_header.version = WT_CRYPT_HEADER_VERSION + 1;

        WT_CRYPT_HEADER *header = (WT_CRYPT_HEADER *)crypt.keys.data;
        header->compatible_version = WT_CRYPT_HEADER_COMPATIBLE_VERSION;
        header->version = WT_CRYPT_HEADER_VERSION + 1;
        header->checksum = 0;
        header->checksum = __wt_checksum(crypt.keys.data, crypt.keys.size);
        REQUIRE(__ut_disagg_validate_crypt(session_impl, &crypt.keys, &read_crypt_header) == 0);

        /* Validate that before and after validation should not change the header. */
        REQUIRE(memcmp(&write_crypt_header, read_crypt_header, sizeof(WT_CRYPT_HEADER)) == 0);
    }

    SECTION("Test key provider header compatibility issue")
    {
        /* Page demands a reader newer than this one (compat_version > our VERSION). */
        build_crypt_page(&crypt.keys, WT_CRYPT_HEADER_VERSION, WT_CRYPT_HEADER_VERSION + 1,
          sizeof(WT_CRYPT_HEADER), 0, test_string.size());
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
        build_crypt_page(&crypt.keys, WT_CRYPT_HEADER_VERSION, WT_CRYPT_HEADER_COMPATIBLE_VERSION,
          10, 0, test_string.size());
        REQUIRE(__ut_disagg_validate_crypt(session_impl, &crypt.keys, &read_crypt_header) == EIO);
    }

    SECTION("Test key provider header mismatch size")
    {
        /* header_size larger than the underlying buffer. */
        build_crypt_page(&crypt.keys, WT_CRYPT_HEADER_VERSION, WT_CRYPT_HEADER_COMPATIBLE_VERSION,
          (uint8_t)(sizeof(WT_CRYPT_HEADER) + 10), 0, test_string.size());
        REQUIRE(__ut_disagg_validate_crypt(session_impl, &crypt.keys, &read_crypt_header) == EIO);
    }

    SECTION("Test key provider header mismatch checksum")
    {
        WT_CRYPT_HEADER *header = (WT_CRYPT_HEADER *)crypt.keys.data;
        header->checksum = 123;
        REQUIRE(__ut_disagg_validate_crypt(session_impl, &crypt.keys, &read_crypt_header) == EIO);
    }
    __wt_free(session_impl, read_crypt_header);
}

/*
 * Backward compatibility: the reader must accept a v1 page (16-byte header, no timestamp field) as
 * written by an older version of WT on disk.
 */
TEST_CASE_METHOD(
  kp_header_fixture, "Key provider header: reader accepts v1 page", "[key_provider_header]")
{
    /* The original on-disk header size, before the timestamp field was appended. */
    static constexpr uint8_t k_v1_header_size = 16;

    REQUIRE(
      __wt_buf_initsize(session_impl, &crypt.keys, k_v1_header_size + test_string.size()) == 0);
    memcpy((uint8_t *)crypt.keys.mem + k_v1_header_size, test_string.data(), test_string.size());
    crypt.keys.data = crypt.keys.mem;
    crypt.keys.size = k_v1_header_size + test_string.size();

    build_crypt_page(&crypt.keys, 1, 1, k_v1_header_size, 0, test_string.size());

    WT_CRYPT_HEADER *read_crypt_header = nullptr;
    REQUIRE(__ut_disagg_validate_crypt(session_impl, &crypt.keys, &read_crypt_header) == 0);
    REQUIRE(read_crypt_header->signature == WT_CRYPT_HEADER_SIGNATURE);
    REQUIRE(read_crypt_header->version == 1);
    REQUIRE(read_crypt_header->compatible_version == 1);
    REQUIRE(read_crypt_header->header_size == k_v1_header_size);
    REQUIRE(read_crypt_header->crypt_size == test_string.size());
    /* No timestamp on a v1 page; the heap-allocated header keeps it as 0. */
    REQUIRE(read_crypt_header->timestamp == 0);
    __wt_free(session_impl, read_crypt_header);
}

/*
 * Forward compatibility: the reader must accept a page whose version is higher than this writer
 * emits, as long as compatible_version stays within what this reader knows.
 */
TEST_CASE_METHOD(
  kp_header_fixture, "Key provider header: reader accepts future version", "[key_provider_header]")
{
    crypt.keys.data = crypt.keys.mem;
    crypt.keys.size = sizeof(WT_CRYPT_HEADER) + test_string.size();

    const uint64_t expected_timestamp = 10;
    build_crypt_page(&crypt.keys, WT_CRYPT_HEADER_VERSION + 1, WT_CRYPT_HEADER_COMPATIBLE_VERSION,
      sizeof(WT_CRYPT_HEADER), expected_timestamp, test_string.size());

    WT_CRYPT_HEADER *read_crypt_header = nullptr;
    REQUIRE(__ut_disagg_validate_crypt(session_impl, &crypt.keys, &read_crypt_header) == 0);
    REQUIRE(read_crypt_header->signature == WT_CRYPT_HEADER_SIGNATURE);
    REQUIRE(read_crypt_header->version == WT_CRYPT_HEADER_VERSION + 1);
    REQUIRE(read_crypt_header->compatible_version == WT_CRYPT_HEADER_COMPATIBLE_VERSION);
    REQUIRE(read_crypt_header->header_size == sizeof(WT_CRYPT_HEADER));
    REQUIRE(read_crypt_header->crypt_size == test_string.size());
    REQUIRE(read_crypt_header->timestamp == expected_timestamp);
    __wt_free(session_impl, read_crypt_header);
}

/*
 * Forward compatibility: the reader must accept a page whose header is larger than the current
 * struct -- simulating a future writer that appended additional fields.
 */
TEST_CASE_METHOD(
  kp_header_fixture, "Key provider header: reader accepts longer header", "[key_provider_header]")
{
    /* A future writer might extend the header beyond the current struct size. */
    static constexpr uint8_t k_future_header_size = (uint8_t)(sizeof(WT_CRYPT_HEADER) + 8);

    REQUIRE(
      __wt_buf_initsize(session_impl, &crypt.keys, k_future_header_size + test_string.size()) == 0);
    memcpy(
      (uint8_t *)crypt.keys.mem + k_future_header_size, test_string.data(), test_string.size());
    crypt.keys.data = crypt.keys.mem;
    crypt.keys.size = k_future_header_size + test_string.size();

    build_crypt_page(&crypt.keys, WT_CRYPT_HEADER_VERSION + 1, WT_CRYPT_HEADER_COMPATIBLE_VERSION,
      k_future_header_size, 0, test_string.size());

    WT_CRYPT_HEADER *read_crypt_header = nullptr;
    REQUIRE(__ut_disagg_validate_crypt(session_impl, &crypt.keys, &read_crypt_header) == 0);
    REQUIRE(read_crypt_header->signature == WT_CRYPT_HEADER_SIGNATURE);
    REQUIRE(read_crypt_header->version == WT_CRYPT_HEADER_VERSION + 1);
    REQUIRE(read_crypt_header->compatible_version == WT_CRYPT_HEADER_COMPATIBLE_VERSION);
    REQUIRE(read_crypt_header->header_size == k_future_header_size);
    REQUIRE(read_crypt_header->crypt_size == test_string.size());
    __wt_free(session_impl, read_crypt_header);
}

/*
 * Boundary case for the compatibility check: a page whose compatible_version equals this reader's
 * version. The reader satisfies the page's demand exactly and must accept.
 */
TEST_CASE_METHOD(kp_header_fixture,
  "Key provider header: reader accepts compat_version equal to reader version",
  "[key_provider_header]")
{
    crypt.keys.data = crypt.keys.mem;
    crypt.keys.size = sizeof(WT_CRYPT_HEADER) + test_string.size();

    build_crypt_page(&crypt.keys, WT_CRYPT_HEADER_VERSION, WT_CRYPT_HEADER_VERSION,
      sizeof(WT_CRYPT_HEADER), 0, test_string.size());

    WT_CRYPT_HEADER *read_crypt_header = nullptr;
    REQUIRE(__ut_disagg_validate_crypt(session_impl, &crypt.keys, &read_crypt_header) == 0);
    REQUIRE(read_crypt_header->signature == WT_CRYPT_HEADER_SIGNATURE);
    REQUIRE(read_crypt_header->version == WT_CRYPT_HEADER_VERSION);
    REQUIRE(read_crypt_header->compatible_version == WT_CRYPT_HEADER_VERSION);
    REQUIRE(read_crypt_header->header_size == sizeof(WT_CRYPT_HEADER));
    REQUIRE(read_crypt_header->crypt_size == test_string.size());
    __wt_free(session_impl, read_crypt_header);
}
