// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/unittest/unittest.h"

#include <memory>

namespace mongo::unittest {
namespace {
TEST(AutoUpdateAssertion, BSONTest) {
    // BSONObj that fits on a single line.
    auto actual = fromjson("{hello: 'world'}");
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"hello":"world"})",
        actual);

    // BSONObj that spills over to multiple lines.
    actual = fromjson(
        "{hello: 'world', nested: {key1: 1, nested: {key2: 3, key4: 'some string here', nested: "
        "{anotherLevel: true}}}}");
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "hello": "world",
            "nested": {
                "key1": 1,
                "nested": {
                    "key2": 3,
                    "key4": "some string here",
                    "nested": {
                        "anotherLevel": true
                    }
                }
            }
        })",
        actual);
}
}  // namespace
}  // namespace mongo::unittest
