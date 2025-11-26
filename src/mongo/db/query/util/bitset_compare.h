/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include <bitset>
#include <climits>

namespace mongo {
/**
 * Returns true if the lhs bitset is smaller than the rhs in lexicographical order.
 */
template <size_t N>
bool bitsetLess(const std::bitset<N>& lhs, const std::bitset<N>& rhs) {
    if constexpr (N <= sizeof(unsigned long long) * CHAR_BIT) {
        return lhs.to_ullong() < rhs.to_ullong();
    } else {
        if (lhs == rhs) {
            return false;
        }
        for (size_t i = N - 1; i >= 0; --i) {
            if (lhs[i] ^ rhs[i]) {
                return rhs[i];
            }
        }
        return false;
    }
}
}  // namespace mongo
