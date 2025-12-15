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

#include "mongo/db/query/compiler/ce/sampling/ce_multikey_dotted_path_support.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/unittest/unittest.h"

TEST(MultikeyDottedPathSupport, HandlesScalars) {
    const auto obj = mongo::fromjson("{a : 1}");

    auto it = mongo::ce::MultiKeyDottedPathIterator("a");

    ASSERT_BSONELT_EQ(it.resetObj(&obj), obj["a"]);
    ASSERT_EQ(it.hasNext(), false);
}

TEST(MultikeyDottedPathSupport, ResetObj) {
    const auto obj1 = mongo::fromjson("{a : [1, 2]}");
    const auto obj2 = mongo::fromjson("{a : 3}");

    auto it = mongo::ce::MultiKeyDottedPathIterator("a");

    ASSERT_BSONELT_EQ(it.resetObj(&obj1), obj1["a"]["0"]);
    ASSERT_EQ(it.hasNext(), true);

    ASSERT_BSONELT_EQ(it.resetObj(&obj2), obj2["a"]);
    ASSERT_EQ(it.hasNext(), false);

    ASSERT_BSONELT_EQ(it.resetObj(&obj1), obj1["a"]["0"]);
    ASSERT_EQ(it.hasNext(), true);
    ASSERT_BSONELT_EQ(it.getNext(), obj1["a"]["1"]);
    ASSERT_EQ(it.hasNext(), false);

    ASSERT_BSONELT_EQ(it.resetObj(&obj2), obj2["a"]);
    ASSERT_EQ(it.hasNext(), false);
}

TEST(MultikeyDottedPathSupport, HandlesLeafArrays) {
    const auto obj = mongo::fromjson("{a : [1, 2]}");

    auto it = mongo::ce::MultiKeyDottedPathIterator("a");

    ASSERT_BSONELT_EQ(it.resetObj(&obj), obj["a"]["0"]);
    ASSERT_EQ(it.hasNext(), true);
    ASSERT_BSONELT_EQ(it.getNext(), obj["a"]["1"]);
    ASSERT_EQ(it.hasNext(), false);
}

TEST(MultikeyDottedPathSupport, HandlesNestedArrays) {
    const auto obj = mongo::fromjson("{a: [{b: [1, 2]}, {b: [3, 4]}]}");

    auto it = mongo::ce::MultiKeyDottedPathIterator("a.b");

    ASSERT_BSONELT_EQ(it.resetObj(&obj), obj["a"]["0"]["b"]["0"]);
    ASSERT_EQ(it.hasNext(), true);
    ASSERT_BSONELT_EQ(it.getNext(), obj["a"]["0"]["b"]["1"]);
    ASSERT_EQ(it.hasNext(), true);
    ASSERT_BSONELT_EQ(it.getNext(), obj["a"]["1"]["b"]["0"]);
    ASSERT_EQ(it.hasNext(), true);
    ASSERT_BSONELT_EQ(it.getNext(), obj["a"]["1"]["b"]["1"]);
    ASSERT_EQ(it.hasNext(), false);
}

TEST(MultikeyDottedPathSupport, NoCascadingUnwind) {
    const auto obj = mongo::fromjson("{a: [[0], [0]]}");


    auto it = mongo::ce::MultiKeyDottedPathIterator("a");

    ASSERT_BSONELT_EQ(it.resetObj(&obj), obj["a"]["0"]);
    ASSERT_EQ(it.hasNext(), true);
    ASSERT_BSONELT_EQ(it.getNext(), obj["a"]["1"]);
    ASSERT_EQ(it.hasNext(), false);
}

namespace {
const mongo::BSONObj nullObj = BSON("" << mongo::BSONNULL);
const mongo::BSONElement nullElt = nullObj.firstElement();
const mongo::BSONObj undefinedObj = BSON("" << mongo::BSONUndefined);
const mongo::BSONElement undefinedElt = undefinedObj.firstElement();
}  // namespace

TEST(MultikeyDottedPathSupport, HandlesEmptyArrays) {
    {
        // For leaf nested arrays
        const auto obj = mongo::fromjson("{a: []}");
        auto it = mongo::ce::MultiKeyDottedPathIterator("a");
        ASSERT_BSONELT_EQ(it.resetObj(&obj), undefinedElt);
        ASSERT_EQ(it.hasNext(), false);
    }

    {
        // For leaf nested arrays - 2
        const auto obj = mongo::fromjson("{a: {b: []}}");
        auto it = mongo::ce::MultiKeyDottedPathIterator("a.b");
        ASSERT_BSONELT_EQ(it.resetObj(&obj), undefinedElt);
        ASSERT_EQ(it.hasNext(), false);
    }

    {
        // For leaf nested arrays - bigger
        const auto obj = mongo::fromjson("{a: [{b: [1]}, {b: []}, {b: [2]}, {b: []}]}");

        auto it = mongo::ce::MultiKeyDottedPathIterator("a.b");

        ASSERT_BSONELT_EQ(it.resetObj(&obj), obj["a"]["0"]["b"]["0"]);
        ASSERT_EQ(it.hasNext(), true);
        ASSERT_BSONELT_EQ(it.getNext(), undefinedElt);
        ASSERT_EQ(it.hasNext(), true);
        ASSERT_BSONELT_EQ(it.getNext(), obj["a"]["2"]["b"]["0"]);
        ASSERT_EQ(it.hasNext(), true);
        ASSERT_BSONELT_EQ(it.getNext(), undefinedElt);
        ASSERT_EQ(it.hasNext(), false);
    }

    {
        // For non-leaf nested array
        const auto obj = mongo::fromjson("{a: []}");

        auto it = mongo::ce::MultiKeyDottedPathIterator("a.b");

        ASSERT_BSONELT_EQ(it.resetObj(&obj), nullElt);
        ASSERT_EQ(it.hasNext(), false);
    }
}

TEST(MultikeyDottedPathSupport, HandlesElementAbsence) {
    {
        // Is an object, doesn't have the key
        const auto obj = mongo::BSONObj();

        auto it = mongo::ce::MultiKeyDottedPathIterator("a");

        ASSERT_BSONELT_EQ(it.resetObj(&obj), nullElt);
        ASSERT_EQ(it.hasNext(), false);
    }

    {
        // Is not an object
        const auto obj = mongo::fromjson("{a: 0}");

        auto it = mongo::ce::MultiKeyDottedPathIterator("a.b");

        ASSERT_BSONELT_EQ(it.resetObj(&obj), nullElt);
        ASSERT_EQ(it.hasNext(), false);
    }
}

TEST(MultikeyDottedPathSupport, NumberComponents) {
    {
        const auto obj = mongo::fromjson("{a: [{b: 0}]}");

        auto it = mongo::ce::MultiKeyDottedPathIterator("a.0.b");

        ASSERT_BSONELT_EQ(it.resetObj(&obj), obj["a"]["0"]["b"]);
        ASSERT_EQ(it.hasNext(), false);
    }

    {
        const auto obj = mongo::fromjson("{a: [{b: 0}]}");

        auto it = mongo::ce::MultiKeyDottedPathIterator("a.1.b");

        ASSERT_BSONELT_EQ(it.resetObj(&obj), nullElt);
        ASSERT_EQ(it.hasNext(), false);
    }

    {
        const auto obj = mongo::fromjson("{a: {\"1\": {b: 0}}}");

        auto it = mongo::ce::MultiKeyDottedPathIterator("a.1.b");

        ASSERT_BSONELT_EQ(it.resetObj(&obj), obj["a"]["1"]["b"]);
        ASSERT_EQ(it.hasNext(), false);
    }

    {
        const auto obj = mongo::fromjson("{a: [[0, {b: 1}]]}");

        auto it = mongo::ce::MultiKeyDottedPathIterator("a.1.b");

        ASSERT_BSONELT_EQ(it.resetObj(&obj), nullElt);
        ASSERT_EQ(it.hasNext(), false);
    }

    {
        const auto obj = mongo::fromjson("{a: [0, {b: 1}]}");

        auto it = mongo::ce::MultiKeyDottedPathIterator("a.1.b");

        ASSERT_BSONELT_EQ(it.resetObj(&obj), obj["a"]["1"]["b"]);
        ASSERT_EQ(it.hasNext(), false);
    }

    {
        const auto obj = mongo::fromjson("{a: {\"1\": [{b: 0}]}}");

        auto it = mongo::ce::MultiKeyDottedPathIterator("a.1.b");

        ASSERT_BSONELT_EQ(it.resetObj(&obj), obj["a"]["1"]["0"]["b"]);
        ASSERT_EQ(it.hasNext(), false);
    }

    {
        const auto obj = mongo::fromjson("{a: []}");

        auto it = mongo::ce::MultiKeyDottedPathIterator("a.1.b");

        ASSERT_BSONELT_EQ(it.resetObj(&obj), nullElt);
        ASSERT_EQ(it.hasNext(), false);
    }

    {
        const auto obj = mongo::fromjson("{a: []}");

        auto it = mongo::ce::MultiKeyDottedPathIterator("a.1");

        ASSERT_BSONELT_EQ(it.resetObj(&obj), nullElt);
        ASSERT_EQ(it.hasNext(), false);
    }

    {
        const auto obj = mongo::fromjson("{a: {\"0\": 0}}");

        auto it = mongo::ce::MultiKeyDottedPathIterator("a.0");

        ASSERT_BSONELT_EQ(it.resetObj(&obj), obj["a"]["0"]);
        ASSERT_EQ(it.hasNext(), false);
    }

    {
        const auto obj = mongo::fromjson("{a: {\"0\": [0]}}");

        auto it = mongo::ce::MultiKeyDottedPathIterator("a.0");

        ASSERT_BSONELT_EQ(it.resetObj(&obj), obj["a"]["0"]["0"]);
        ASSERT_EQ(it.hasNext(), false);
    }

    {
        const auto obj = mongo::fromjson("{a: [[{b: 0}]]}");

        auto it = mongo::ce::MultiKeyDottedPathIterator("a.0.b");

        ASSERT_BSONELT_EQ(it.resetObj(&obj), obj["a"]["0"]["0"]["b"]);
        ASSERT_EQ(it.hasNext(), false);
    }

    {
        const auto obj = mongo::fromjson("{a: [0]}");

        auto it = mongo::ce::MultiKeyDottedPathIterator("a.0");

        ASSERT_BSONELT_EQ(it.resetObj(&obj), obj["a"]["0"]);
        ASSERT_EQ(it.hasNext(), false);
    }

    {
        const auto obj = mongo::fromjson("{a: [0]}");

        auto it = mongo::ce::MultiKeyDottedPathIterator("a.00");

        ASSERT_BSONELT_EQ(it.resetObj(&obj), nullElt);
        ASSERT_EQ(it.hasNext(), false);
    }

    {
        const auto obj = mongo::fromjson("{a: [[0]]}");

        auto it = mongo::ce::MultiKeyDottedPathIterator("a.0");

        ASSERT_BSONELT_EQ(it.resetObj(&obj), obj["a"]["0"]);
        ASSERT_EQ(it.hasNext(), false);
    }

    {
        const auto obj = mongo::fromjson("{a: []}");

        auto it = mongo::ce::MultiKeyDottedPathIterator("a.0");

        ASSERT_BSONELT_EQ(it.resetObj(&obj), nullElt);
        ASSERT_EQ(it.hasNext(), false);
    }

    {
        const auto obj = mongo::fromjson("{a: 0}");

        auto it = mongo::ce::MultiKeyDottedPathIterator("a.0");

        ASSERT_BSONELT_EQ(it.resetObj(&obj), nullElt);
        ASSERT_EQ(it.hasNext(), false);
    }

    {
        const auto obj = mongo::fromjson("{a: [[]]}");

        auto it = mongo::ce::MultiKeyDottedPathIterator("a.0");

        ASSERT_BSONELT_EQ(it.resetObj(&obj), undefinedElt);
        ASSERT_EQ(it.hasNext(), false);
    }
}

TEST(MultikeyDottedPathSupport, Mixed) {
    {
        // Missing field + empty leaf array
        const auto obj = mongo::fromjson("{a: [{}, {b: []}]}");

        auto it = mongo::ce::MultiKeyDottedPathIterator("a.b");

        ASSERT_BSONELT_EQ(it.resetObj(&obj), nullElt);
        ASSERT_EQ(it.hasNext(), true);
        ASSERT_BSONELT_EQ(it.getNext(), undefinedElt);
        ASSERT_EQ(it.hasNext(), false);
    }

    {
        // Missing field + no cascading unwind
        const auto obj = mongo::fromjson("{a: [[{b: 0}]]}");

        auto it = mongo::ce::MultiKeyDottedPathIterator("a.b");

        ASSERT_BSONELT_EQ(it.resetObj(&obj), nullElt);
        ASSERT_EQ(it.hasNext(), false);
    }

    {
        const auto obj = mongo::fromjson("{a: [{b:1}, {b: []}]}");

        auto it = mongo::ce::MultiKeyDottedPathIterator("a.b");

        ASSERT_BSONELT_EQ(it.resetObj(&obj), obj["a"]["0"]["b"]);
        ASSERT_EQ(it.hasNext(), true);
        ASSERT_BSONELT_EQ(it.getNext(), undefinedElt);
        ASSERT_EQ(it.hasNext(), false);
    }


    {
        const auto obj = mongo::fromjson("{a: [{b: [{c:1}]}, {b: []}]}");

        auto it = mongo::ce::MultiKeyDottedPathIterator("a.b.c");

        ASSERT_BSONELT_EQ(it.resetObj(&obj), obj["a"]["0"]["b"]["0"]["c"]);
        ASSERT_EQ(it.hasNext(), true);
        ASSERT_BSONELT_EQ(it.getNext(), nullElt);
        ASSERT_EQ(it.hasNext(), false);
    }


    {
        const auto obj = mongo::fromjson("{a: [{b:1}, {}]}");

        auto it = mongo::ce::MultiKeyDottedPathIterator("a.b");

        ASSERT_BSONELT_EQ(it.resetObj(&obj), obj["a"]["0"]["b"]);
        ASSERT_EQ(it.hasNext(), true);
        ASSERT_BSONELT_EQ(it.getNext(), nullElt);
        ASSERT_EQ(it.hasNext(), false);
    }

    {
        const auto obj = mongo::fromjson("{a: [{\"00\": 1}]}");

        auto it = mongo::ce::MultiKeyDottedPathIterator("a.00");

        ASSERT_BSONELT_EQ(it.resetObj(&obj), obj["a"]["0"]["00"]);
        ASSERT_EQ(it.hasNext(), false);
    }
}
