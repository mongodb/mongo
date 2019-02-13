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

// todo: ideally move to db/ instead of bson/, but elim any dependencies first

/** A precomputation of a BSON index or sort key pattern.  That is something like:
       { a : 1, b : -1 }
    The constructor is private to make conversion more explicit so we notice where we call make().
    Over time we should push this up higher and higher.
*/
class Ordering {
    unsigned bits;
    Ordering(unsigned b) : bits(b) {}

public:
    static constexpr size_t kMaxCompoundIndexKeys = size_t{32};

    Ordering(const Ordering& r) : bits(r.bits) {}
    void operator=(const Ordering& r) {
        bits = r.bits;
    }

    /** so, for key pattern { a : 1, b : -1 }
        get(0) == 1
        get(1) == -1
    */
    int get(int i) const {
        return ((1 << i) & bits) ? -1 : 1;
    }

    // for woCompare...
    unsigned descending(unsigned mask) const {
        return bits & mask;
    }

    /*operator std::string() const {
        StringBuilder buf;
        for ( unsigned i=0; i<nkeys; i++)
            buf.append( get(i) > 0 ? "+" : "-" );
        return buf.str();
    }*/

    static Ordering make(const BSONObj& obj) {
        unsigned b = 0;
        BSONObjIterator k(obj);
        unsigned n = 0;
        while (1) {
            BSONElement e = k.next();
            if (e.eoo())
                break;
            uassert(13103, "too many compound keys", n < kMaxCompoundIndexKeys);
            if (e.number() < 0)
                b |= (1 << n);
            n++;
        }
        return Ordering(b);
    }
};
}
