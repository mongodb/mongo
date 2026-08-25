/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <cstring>
#include <string>
#include <vector>

#include <catch2/catch.hpp>

#include "utils.h"
#include "wrappers/connection_wrapper.h"

#include "wiredtiger.h"
#include "wt_internal.h"

namespace {

using extension_init_t = int (*)(WT_CONNECTION *, WT_CONFIG_ARG *);

std::vector<std::string> captured_errors;

int
capture_error(WT_EVENT_HANDLER *handler, WT_SESSION *session, int error, const char *message)
{
    (void)handler;
    (void)session;
    (void)error;
    captured_errors.emplace_back(message);
    return (0);
}

WT_EVENT_HANDLER capture_handler = {capture_error, nullptr, nullptr, nullptr, nullptr};

/*
 * A failed stored-length check and a failed decompression both come back as WT_ERROR, so the return
 * code alone cannot tell them apart. Watch the error stream instead, and match the two messages the
 * extensions emit when they reject a stored length. Anything else means the length was accepted and
 * the compression library was handed it.
 */
bool
guard_rejected(WT_COMPRESSOR *c, WT_SESSION *session, std::vector<uint8_t> &src, size_t src_len,
  std::vector<uint8_t> &dst)
{
    size_t result_len = 0;

    captured_errors.clear();
    if (c->decompress(c, session, src.data(), src_len, dst.data(), dst.size(), &result_len) !=
      WT_ERROR)
        return (false);

    for (const std::string &m : captured_errors)
        if (m.find("stored size exceeds source size") != std::string::npos ||
          m.find("source size is smaller than the size prefix") != std::string::npos)
            return (true);
    return (false);
}

/*
 * The compressed-length prefix is read back out of the block payload, so it is only as trustworthy
 * as the bytes on disk. These tests drive decompress() directly rather than through a table read,
 * which would otherwise mean producing a block whose checksum verifies.
 */
struct compressor_fixture {
    connection_wrapper conn{DB_HOME, "create,in_memory", &capture_handler};
    WT_SESSION *session = nullptr;
    WT_COMPRESSOR *compressor = nullptr;

    compressor_fixture(const char *name, extension_init_t init)
    {
        WT_NAMED_COMPRESSOR *ncomp;

        REQUIRE(conn.get_wt_connection()->open_session(
                  conn.get_wt_connection(), nullptr, nullptr, &session) == 0);
        REQUIRE(init(conn.get_wt_connection(), nullptr) == 0);

        TAILQ_FOREACH (ncomp, &conn.get_wt_connection_impl()->ext.compqh, q)
            if (std::strcmp(ncomp->name, name) == 0)
                compressor = ncomp->compressor;
        REQUIRE(compressor != nullptr);
    }
};

/*
 * store_prefix64 --
 *     Write a stored length the way the 64-bit-prefix compressors read it back.
 */
void
store_prefix64(uint8_t *dst, uint64_t v)
{
#ifdef WORDS_BIGENDIAN
    v = __wt_bswap64(v);
#endif
    std::memcpy(dst, &v, sizeof(v));
}

/*
 * store_prefix32 --
 *     Write a stored length the way the 32-bit-prefix compressors read it back.
 */
void
store_prefix32(uint8_t *dst, uint32_t v)
{
#ifdef WORDS_BIGENDIAN
    v = __wt_bswap32(v);
#endif
    std::memcpy(dst, &v, sizeof(v));
}

std::vector<uint8_t>
sample_data()
{
    std::vector<uint8_t> raw(4096);

    /* Compressible, but not so uniform that a compressor collapses it to nothing. */
    for (size_t i = 0; i < raw.size(); ++i)
        raw[i] = static_cast<uint8_t>(i % 64);
    return raw;
}

std::vector<uint8_t>
compress_buffer(WT_COMPRESSOR *c, WT_SESSION *session, std::vector<uint8_t> &raw)
{
    size_t dst_len, result_len;
    int failed = 0;

    REQUIRE(c->pre_size(c, session, raw.data(), raw.size(), &dst_len) == 0);

    std::vector<uint8_t> dst(dst_len);
    REQUIRE(c->compress(c, session, raw.data(), raw.size(), dst.data(), dst.size(), &result_len,
              &failed) == 0);
    REQUIRE(failed == 0);
    dst.resize(result_len);
    return dst;
}

/*
 * check_decompresses_to_original --
 *     An untouched block decompresses back to the bytes it was compressed from.
 */
void
check_decompresses_to_original(compressor_fixture &f, std::vector<uint8_t> &raw,
  std::vector<uint8_t> &comp, std::vector<uint8_t> &out)
{
    WT_COMPRESSOR *c = f.compressor;
    size_t result_len = 0;

    REQUIRE(c->decompress(
              c, f.session, comp.data(), comp.size(), out.data(), out.size(), &result_len) == 0);
    REQUIRE(result_len == raw.size());
    REQUIRE(std::memcmp(out.data(), raw.data(), raw.size()) == 0);
}

/*
 * check_short_block_rejected --
 *     A block too short to hold the stored-length prefix is rejected rather than read.
 */
void
check_short_block_rejected(
  compressor_fixture &f, std::vector<uint8_t> &comp, std::vector<uint8_t> &out, size_t prefix_size)
{
    REQUIRE(guard_rejected(f.compressor, f.session, comp, prefix_size - 1, out));
}

/*
 * check_guard_64 --
 *     Exercise the compressors that store the compressed length as a 64-bit prefix.
 */
void
check_guard_64(const char *name, extension_init_t init)
{
    compressor_fixture f(name, init);
    std::vector<uint8_t> raw = sample_data();
    std::vector<uint8_t> comp = compress_buffer(f.compressor, f.session, raw);
    std::vector<uint8_t> out(raw.size() * 2);
    std::vector<uint8_t> bad;

    check_decompresses_to_original(f, raw, comp, out);
    check_short_block_rejected(f, comp, out, sizeof(uint64_t));

    /* A stored length larger than the block is rejected. */
    bad = comp;
    store_prefix64(bad.data(), bad.size() + 1000);
    REQUIRE(guard_rejected(f.compressor, f.session, bad, bad.size(), out));

    /*
     * Stored lengths within the prefix size of UINT64_MAX are the values for which adding the
     * prefix wraps. Every one of them must be rejected, along with the largest value below the
     * window.
     */
    for (size_t i = 0; i <= sizeof(uint64_t); ++i) {
        uint64_t stored = UINT64_MAX - i;
        CAPTURE(name, i, stored);
        bad = comp;
        store_prefix64(bad.data(), stored);
        REQUIRE(guard_rejected(f.compressor, f.session, bad, bad.size(), out));
    }
}

/*
 * check_guard_32 --
 *     Exercise the compressors that store the compressed length as a 32-bit field. The addition
 *     only wraps where size_t is 32 bits, but the guard must reject these values regardless.
 */
void
check_guard_32(const char *name, extension_init_t init, size_t prefix_size)
{
    compressor_fixture f(name, init);
    std::vector<uint8_t> raw = sample_data();
    std::vector<uint8_t> comp = compress_buffer(f.compressor, f.session, raw);
    std::vector<uint8_t> out(raw.size() * 2);
    std::vector<uint8_t> bad;

    check_decompresses_to_original(f, raw, comp, out);
    check_short_block_rejected(f, comp, out, prefix_size);

    for (size_t i = 0; i <= prefix_size; ++i) {
        uint32_t stored = UINT32_MAX - static_cast<uint32_t>(i);
        CAPTURE(name, i, stored);
        bad = comp;
        store_prefix32(bad.data(), stored);
        REQUIRE(guard_rejected(f.compressor, f.session, bad, bad.size(), out));
    }
}

} // namespace

#if defined(HAVE_BUILTIN_EXTENSION_SNAPPY)
extern "C" int snappy_extension_init(WT_CONNECTION *, WT_CONFIG_ARG *);
#endif

#if defined(HAVE_BUILTIN_EXTENSION_SNAPPY) || defined(SNAPPY_EXTENSION)
TEST_CASE("snappy: a crafted stored length is rejected", "[compressor]")
{
#if defined(HAVE_BUILTIN_EXTENSION_SNAPPY)
    check_guard_64("snappy", snappy_extension_init);
#else
    static utils::shared_library lib{SNAPPY_EXTENSION};
    check_guard_64("snappy", lib.get<extension_init_t>("wiredtiger_extension_init"));
#endif
}
#endif

#if defined(HAVE_BUILTIN_EXTENSION_ZSTD)
extern "C" int zstd_extension_init(WT_CONNECTION *, WT_CONFIG_ARG *);
#endif

#if defined(HAVE_BUILTIN_EXTENSION_ZSTD) || defined(ZSTD_EXTENSION)
TEST_CASE("zstd: a crafted stored length is rejected", "[compressor]")
{
#if defined(HAVE_BUILTIN_EXTENSION_ZSTD)
    check_guard_64("zstd", zstd_extension_init);
#else
    static utils::shared_library lib{ZSTD_EXTENSION};
    check_guard_64("zstd", lib.get<extension_init_t>("wiredtiger_extension_init"));
#endif
}
#endif

#if defined(HAVE_BUILTIN_EXTENSION_LZ4)
extern "C" int lz4_extension_init(WT_CONNECTION *, WT_CONFIG_ARG *);
#endif

#if defined(HAVE_BUILTIN_EXTENSION_LZ4) || defined(LZ4_EXTENSION)
TEST_CASE("lz4: a crafted stored length is rejected", "[compressor]")
{
    /* Four 4B fields: compressed, uncompressed and useful lengths, and a reserved word. */
    static const size_t lz4_prefix_size = 4 * sizeof(uint32_t);

#if defined(HAVE_BUILTIN_EXTENSION_LZ4)
    check_guard_32("lz4", lz4_extension_init, lz4_prefix_size);
#else
    static utils::shared_library lib{LZ4_EXTENSION};
    check_guard_32("lz4", lib.get<extension_init_t>("wiredtiger_extension_init"), lz4_prefix_size);
#endif
}
#endif
