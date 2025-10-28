/**
 *    Copyright (C) 2024-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/util/md5.h"

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

[[deprecated(
    "This API is deprecated. Use SHA-256 instead of MD-5 for all new cryptographic applications. "
    "For non-cryptographic purposes, consider using a modern hash function like XXHash or "
    "CityHash.")]]
void md5_init_state_deprecated(md5_state_t* pms) {
    md5_init(pms);
}

[[deprecated(
    "This API is deprecated. Use SHA-256 instead of MD-5 for all new cryptographic applications. "
    "For non-cryptographic purposes, consider using a modern hash function like XXHash or "
    "CityHash.")]]
void md5_append_deprecated(md5_state_t* pms, const md5_byte_t* data, int nbytes) {
    md5_process(pms, data, nbytes);
}

[[deprecated(
    "This API is deprecated. Use SHA-256 instead of MD-5 for all new cryptographic applications. "
    "For non-cryptographic purposes, consider using a modern hash function like XXHash or "
    "CityHash.")]]
void md5_finish_deprecated(md5_state_t* pms, md5_byte_t digest[16]) {
    md5_done(pms, digest);
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
