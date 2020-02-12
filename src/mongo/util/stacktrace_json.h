/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <array>
#include <ostream>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/util/stacktrace.h"

namespace mongo::stack_trace_detail {

/**
 * A utility for uint64_t <=> uppercase hex string conversions. It
 * can be used to produce a StringData.
 *
 *     sink << Hex(x);  // as a temporary
 *
 *     Hex hx(x);
 *     StringData sd = hx;  // sd storage is in `hx`.
 */
class Hex {
public:
    using Buf = std::array<char, 18>;  // 64/4 hex digits plus potential "0x"

    static StringData toHex(uint64_t x, Buf& buf, bool showBase = false);

    static uint64_t fromHex(StringData s);

    explicit Hex(uint64_t x, bool showBase = false) : _str{toHex(x, _buf, showBase)} {}
    explicit Hex(const void* x, bool showBase = false)
        : Hex{reinterpret_cast<uintptr_t>(x), showBase} {}

    operator StringData() const {
        return _str;
    }

private:
    Buf _buf;
    StringData _str;
};

class Dec {
public:
    using Buf = std::array<char, 20>;  // ceil(64*log10(2))

    static StringData toDec(uint64_t x, Buf& buf);

    static uint64_t fromDec(StringData s);

    explicit Dec(uint64_t x) : _str(toDec(x, _buf)) {}

    operator StringData() const {
        return _str;
    }

private:
    Buf _buf;
    StringData _str;
};

}  // namespace mongo::stack_trace_detail
