// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/md5.h"

#include "mongo/util/assert_util.h"

#include <cstring>
#include <sstream>

/**
 * Deprecated MD5 functions. MD-5 is considered cryptographically broken and unsuitable for further
 * use. For non-cryptographic purposes, consider using a modern hash function like XXHash or
 * CityHash.
 *
 * These functions are retained only for compatibility with existing data and applications.
 */
namespace mongo {

namespace {
inline void check_md5_return(int ret) {
    // Intentionally not providing any details and using the same report for all errors.
    uassert(12220700, "MD5 operation failed", ret == CRYPT_OK);
}
}  // namespace

[[deprecated(
    "This API is deprecated. Use SHA-256 instead of MD-5 for all new cryptographic applications. "
    "For non-cryptographic purposes, consider using a modern hash function like XXHash or "
    "CityHash.")]]
void md5_init_state_deprecated(md5_state_t* pms) {
    invariant(pms);
    memset(pms, 0, sizeof(md5_state_t));
    check_md5_return(md5_init(pms));  // won't fail unless pms is null
}

[[deprecated(
    "This API is deprecated. Use SHA-256 instead of MD-5 for all new cryptographic applications. "
    "For non-cryptographic purposes, consider using a modern hash function like XXHash or "
    "CityHash.")]]
void md5_append_deprecated(md5_state_t* pms, const md5_byte_t* data, int nbytes) {
    invariant(pms);
    check_md5_return(md5_process(pms, data, nbytes));
}

[[deprecated(
    "This API is deprecated. Use SHA-256 instead of MD-5 for all new cryptographic applications. "
    "For non-cryptographic purposes, consider using a modern hash function like XXHash or "
    "CityHash.")]]
void md5_finish_deprecated(md5_state_t* pms, md5_byte_t digest[16]) {
    invariant(pms);
    invariant(digest);
    check_md5_return(md5_done(pms, digest));
}

[[deprecated(
    "This API is deprecated. Use SHA-256 instead of MD-5 for all new cryptographic applications. "
    "For non-cryptographic purposes, consider using a modern hash function like XXHash or "
    "CityHash.")]]
void md5_deprecated(const void* buf, int nbytes, md5digest digest) {
    md5_state_t st;
    md5_init_state_deprecated(&st);
    md5_append_deprecated(&st, (const md5_byte_t*)buf, nbytes);
    md5_finish_deprecated(&st, digest);
}

[[deprecated(
    "This API is deprecated. Use SHA-256 instead of MD-5 for all new cryptographic applications. "
    "For non-cryptographic purposes, consider using a modern hash function like XXHash or "
    "CityHash.")]]
void md5_deprecated(const char* str, md5digest digest) {
    md5_deprecated(str, strlen(str), digest);
}

std::string digestToString(md5digest digest) {
    static const char* letters = "0123456789abcdef";
    std::stringstream ss;
    for (int i = 0; i < 16; i++) {
        unsigned char c = digest[i];
        ss << letters[(c >> 4) & 0xf] << letters[c & 0xf];
    }
    return ss.str();
}

std::string md5simpledigest_deprecated(const void* buf, int nbytes) {
    md5digest d;
    md5_deprecated(buf, nbytes, d);
    return digestToString(d);
}

std::string md5simpledigest_deprecated(const std::string& s) {
    return md5simpledigest_deprecated(s.data(), s.size());
}

}  // namespace mongo
