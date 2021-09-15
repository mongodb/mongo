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

#include "mongo/bson/json.h"
#include "mongo/platform/basic.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/timeseries/timeseries_dotted_path_support.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

namespace dps = ::mongo::timeseries::dotted_path_support;

TEST(DottedPathSupport, HaveArrayAlongBucketPath) {
    BSONObj obj = ::mongo::fromjson(R"(
{
    control: {version: 1},
    bogus: [1],
    data: {
        a: {},
        b: [],
        c: {
            "0": true,
            "1": false
        },
        d: {
            "0": false,
            "1": []
        },
        e: {
            "3": "foo",
            "99": [1, 2]
        },
        f: {
            "1": {
                a: [true, false]
            }
        },
        g: {
            "1": {
                a: [
                    {a: true}
                ]
            }
        }
    }
})");

    // Non-data fields should always be false
    ASSERT_FALSE(dps::haveArrayAlongBucketDataPath(obj, "control"));
    ASSERT_FALSE(dps::haveArrayAlongBucketDataPath(obj, "control.version"));
    ASSERT_FALSE(dps::haveArrayAlongBucketDataPath(obj, "bogus"));

    ASSERT_FALSE(dps::haveArrayAlongBucketDataPath(obj, "data"));
    ASSERT_FALSE(dps::haveArrayAlongBucketDataPath(obj, "data.a"));
    ASSERT_FALSE(dps::haveArrayAlongBucketDataPath(obj, "data.b"));  // bucket expansion hides array
    ASSERT_FALSE(dps::haveArrayAlongBucketDataPath(obj, "data.c"));
    ASSERT_TRUE(dps::haveArrayAlongBucketDataPath(obj, "data.d"));
    ASSERT_TRUE(dps::haveArrayAlongBucketDataPath(obj, "data.e"));
    ASSERT_FALSE(dps::haveArrayAlongBucketDataPath(obj, "data.f"));
    ASSERT_TRUE(dps::haveArrayAlongBucketDataPath(obj, "data.f.a"));
    ASSERT_FALSE(dps::haveArrayAlongBucketDataPath(obj, "data.g"));
    ASSERT_TRUE(dps::haveArrayAlongBucketDataPath(obj, "data.g.a"));
    ASSERT_TRUE(dps::haveArrayAlongBucketDataPath(obj, "data.g.a.a"));
}

}  // namespace
}  // namespace mongo
