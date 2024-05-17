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

#pragma once

#include <string>

#include <tomcrypt.h>

namespace mongo {

using md5digest = unsigned char[16];
using md5_byte_t = unsigned char; /* 8-bit byte */
using md5_word_t = unsigned int;  /* 32-bit word */
using md5_state_t = hash_state;

void md5_init_state(md5_state_t* pms);

/* Append a std::string to the message. */
void md5_append(md5_state_t* pms, const md5_byte_t* data, int nbytes);

/* Finish the message and store the result in the digest. */
void md5_finish(md5_state_t* pms, md5_byte_t digest[16]);

// Calculate the MD5 digest for the given buffer and store the result in the specified digest array.
void md5(const void* buf, int nbytes, md5digest digest);

// Calculate the MD5 digest for the input string and store the result in the specified digest array.
void md5(const char* str, md5digest digest);

// Convert the MD5 digest array into a hexadecimal string representation.
std::string digestToString(md5digest digest);

// Calculate the MD5 digest for the given buffer and return the result as a hexadecimal string.
std::string md5simpledigest(const void* buf, int nbytes);

// Calculate the MD5 digest for the input string and return the result as a hexadecimal string.
std::string md5simpledigest(const std::string& s);

}  // namespace mongo
