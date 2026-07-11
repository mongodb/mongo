// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
