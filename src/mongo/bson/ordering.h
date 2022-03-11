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

#include "mongo/bson/bsonobj.h"

namespace mongo {

/** A precomputation of a BSON index or sort key pattern.  That is something like:
 *     { a : 1, b : -1 }
 *   The constructor is private to make conversion more explicit so we notice where we call make().
 *   Over time we should push this up higher and higher.
 */
class Ordering {
    uint32_t bits;
    Ordering(uint32_t b) : bits(b) {}

public:
    static constexpr size_t kMaxCompoundIndexKeys = size_t{32};
    static_assert(kMaxCompoundIndexKeys == 8 * sizeof(bits));

    static Ordering allAscending() {
        return {0};
    }

    Ordering(const Ordering& r) : bits(r.bits) {}
    void operator=(const Ordering& r) {
        bits = r.bits;
    }

    uint32_t getBits() const {
        return bits;
    }

    /** so, for key pattern { a : 1, b : -1 }
     *   get(0) == 1
     *   get(1) == -1
     */
    int get(int i) const {
        uassert(ErrorCodes::Overflow,
                str::stream() << "Ordering offset is out of bounds: " << i,
                i >= 0);
        // Ordering only allows the first 32 fields to be inverted; any fields after the 32nd must
        // be ascending. Return 1 to avoid a left shift a 32-bit integer by more than 31 bits, which
        // is undefined behavior in C++.
        if (static_cast<size_t>(i) >= kMaxCompoundIndexKeys) {
            return 1;
        }
        return ((1u << i) & bits) ? -1 : 1;
    }

    uint32_t descending(uint32_t mask) const {
        return bits & mask;
    }

    static Ordering make(const BSONObj& obj) {
        uint32_t b = 0;
        BSONObjIterator k(obj);
        uint32_t n = 0;
        while (1) {
            BSONElement e = k.next();
            if (e.eoo())
                break;
            uassert(13103, "too many compound keys", n < kMaxCompoundIndexKeys);
            if (e.number() < 0)
                b |= (1u << n);
            n++;
        }
        return Ordering(b);
    }
};
}  // namespace mongo
