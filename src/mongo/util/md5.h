// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once
#include "mongo/util/modules.h"

#include <string>

#include <tomcrypt.h>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

using md5digest = unsigned char[16];
using md5_byte_t = unsigned char; /* 8-bit byte */
using md5_word_t = unsigned int;  /* 32-bit word */
using md5_state_t = hash_state;

[[deprecated(
    "This API is deprecated. Use SHA-256 instead of MD-5 for all new cryptographic applications. "
    "For non-cryptographic purposes, consider using a modern hash function like XXHash or "
    "CityHash.")]]
void md5_init_state_deprecated(md5_state_t* pms);

/* Append a std::string to the message. */
[[deprecated(
    "This API is deprecated. Use SHA-256 instead of MD-5 for all new cryptographic applications. "
    "For non-cryptographic purposes, consider using a modern hash function like XXHash or "
    "CityHash.")]]
void md5_append_deprecated(md5_state_t* pms, const md5_byte_t* data, int nbytes);

/* Finish the message and store the result in the digest. */
[[deprecated(
    "This API is deprecated. Use SHA-256 instead of MD-5 for all new cryptographic applications. "
    "For non-cryptographic purposes, consider using a modern hash function like XXHash or "
    "CityHash.")]]
void md5_finish_deprecated(md5_state_t* pms, md5_byte_t digest[16]);

// Calculate the MD5 digest for the given buffer and store the result in the specified digest array.
[[deprecated(
    "This API is deprecated. Use SHA-256 instead of MD-5 for all new cryptographic applications. "
    "For non-cryptographic purposes, consider using a modern hash function like XXHash or "
    "CityHash.")]]
void md5_deprecated(const void* buf, int nbytes, md5digest digest);

// Calculate the MD5 digest for the input string and store the result in the specified digest array.
[[deprecated(
    "This API is deprecated. Use SHA-256 instead of MD-5 for all new cryptographic applications. "
    "For non-cryptographic purposes, consider using a modern hash function like XXHash or "
    "CityHash.")]]
void md5_deprecated(const char* str, md5digest digest);

// Convert the MD5 digest array into a hexadecimal string representation.
std::string digestToString(md5digest digest);

// Calculate the MD5 digest for the given buffer and return the result as a hexadecimal string.
std::string md5simpledigest_deprecated(const void* buf, int nbytes);

// Calculate the MD5 digest for the input string and return the result as a hexadecimal string.
std::string md5simpledigest_deprecated(const std::string& s);

}  // namespace mongo
