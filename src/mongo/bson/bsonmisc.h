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

#include <memory>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/util/builder.h"

namespace mongo {

class BSONElementCmpWithoutField {
public:
    /**
     * If 'stringComparator' is null, the default binary comparator will be used for comparing
     * string elements.  A custom string comparator may be provided, but it must outlive the
     * constructed BSONElementCmpWithoutField.
     */
    BSONElementCmpWithoutField(const StringDataComparator* stringComparator = nullptr)
        : _stringComparator(stringComparator) {}

    bool operator()(const BSONElement& l, const BSONElement& r) const {
        return l.woCompare(r, false, _stringComparator) < 0;
    }

private:
    const StringDataComparator* _stringComparator;
};

/**
   used in conjuction with BSONObjBuilder, allows for proper buffer size to prevent crazy memory
   usage
 */
class BSONSizeTracker {
public:
    BSONSizeTracker() {
        _pos = 0;
        for (int i = 0; i < SIZE; i++)
            _sizes[i] = 512;  // this is the default, so just be consistent
    }

    ~BSONSizeTracker() {}

    void got(int size) {
        _sizes[_pos] = size;
        _pos = (_pos + 1) % SIZE;  // thread safe at least on certain compilers
    }

    /**
     * right now choosing largest size
     */
    int getSize() const {
        int x = 16;  // sane min
        for (int i = 0; i < SIZE; i++) {
            if (_sizes[i] > x)
                x = _sizes[i];
        }
        return x;
    }

private:
    enum { SIZE = 10 };
    int _pos;
    int _sizes[SIZE];
};

// considers order
bool fieldsMatch(const BSONObj& lhs, const BSONObj& rhs);
}  // namespace mongo
