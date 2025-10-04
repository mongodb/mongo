/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace mongo {

/**
 * A utility class for performing itoa style integer formatting. This class is highly optimized
 * and only really should be used in hot code paths.
 */
class ItoA {
public:
    // digits10 is 1 less than the maximum number of digits.
    static constexpr size_t kBufSize = std::numeric_limits<std::uint64_t>::digits10 + 1;

    explicit ItoA(std::uint64_t i);
    ItoA(const ItoA&) = delete;
    ItoA& operator=(const ItoA&) = delete;

    std::string toString() const {
        return std::string{_str};
    }

    StringData toStringData() const {
        return _str;
    }

    operator StringData() const {
        return _str;
    }

private:
    StringData _str;
    char _buf[kBufSize];
};

}  // namespace mongo
