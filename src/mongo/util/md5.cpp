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

namespace mongo {

void md5_init_state(md5_state_t* pms) {
    md5_init(pms);
}

void md5_append(md5_state_t* pms, const md5_byte_t* data, int nbytes) {
    md5_process(pms, data, nbytes);
}

void md5_finish(md5_state_t* pms, md5_byte_t digest[16]) {
    md5_done(pms, digest);
}

void md5(const void* buf, int nbytes, md5digest digest) {
    md5_state_t st;
    md5_init_state(&st);
    md5_append(&st, (const md5_byte_t*)buf, nbytes);
    md5_finish(&st, digest);
}

void md5(const char* str, md5digest digest) {
    md5(str, strlen(str), digest);
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

std::string md5simpledigest(const void* buf, int nbytes) {
    md5digest d;
    md5(buf, nbytes, d);
    return digestToString(d);
}

std::string md5simpledigest(const std::string& s) {
    return md5simpledigest(s.data(), s.size());
}

}  // namespace mongo
