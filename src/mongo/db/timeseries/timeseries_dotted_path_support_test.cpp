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
#include "mongo/db/timeseries/bucket_compression.h"
#include "mongo/db/timeseries/timeseries_dotted_path_support.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

namespace tdps = ::mongo::timeseries::dotted_path_support;

class TimeseriesDottedPathSupportTest : public unittest::Test {
protected:
    void runTest(const BSONObj& obj, const std::function<void(const BSONObj&)>& test) {
        test(obj);

        NamespaceString nss{"test"};
        auto compressionResult =
            timeseries::compressBucket(obj, "time", nss, /*eligibleForReopening=*/false, true);
        ASSERT_TRUE(compressionResult.compressedBucket.has_value());
        ASSERT_FALSE(compressionResult.decompressionFailed);

        test(compressionResult.compressedBucket.get());
    }
};

TEST_F(TimeseriesDottedPathSupportTest, HaveArrayAlongBucketPath) {
    BSONObj input = ::mongo::fromjson(R"(
{
    control: {version: 1},
    bogus: [1],
    data: {
        time: {
            "0": {"$date": "1970-01-01T00:00:00.000Z"},
            "1": {"$date": "1970-01-01T00:00:00.001Z"},
            "3": {"$date": "1970-01-01T00:00:00.003Z"},
            "99": {"$date": "1970-01-01T00:00:00.099Z"}
        },
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
        },
        h: {
            "1": {
                a: {
                    b: true
                }
            }
        },
        i: {
            "1": [
                {a: true},
                {a: false}
            ]
        }
    }
})");

    runTest(input, [this](const BSONObj& obj) {
        // Non-data fields should always be false
        ASSERT_FALSE(tdps::haveArrayAlongBucketDataPath(obj, "control"));
        ASSERT_FALSE(tdps::haveArrayAlongBucketDataPath(obj, "control.version"));
        ASSERT_FALSE(tdps::haveArrayAlongBucketDataPath(obj, "bogus"));

        ASSERT_FALSE(tdps::haveArrayAlongBucketDataPath(obj, "data"));
        ASSERT_FALSE(tdps::haveArrayAlongBucketDataPath(obj, "data.a"));
        ASSERT_FALSE(
            tdps::haveArrayAlongBucketDataPath(obj, "data.b"));  // bucket expansion hides array
        ASSERT_FALSE(tdps::haveArrayAlongBucketDataPath(obj, "data.c"));
        ASSERT_TRUE(tdps::haveArrayAlongBucketDataPath(obj, "data.d"));
        ASSERT_TRUE(tdps::haveArrayAlongBucketDataPath(obj, "data.e"));
        ASSERT_FALSE(tdps::haveArrayAlongBucketDataPath(obj, "data.f"));
        ASSERT_TRUE(tdps::haveArrayAlongBucketDataPath(obj, "data.f.a"));
        ASSERT_FALSE(tdps::haveArrayAlongBucketDataPath(obj, "data.g"));
        ASSERT_TRUE(tdps::haveArrayAlongBucketDataPath(obj, "data.g.a"));
        ASSERT_TRUE(tdps::haveArrayAlongBucketDataPath(obj, "data.g.a.a"));
        ASSERT_FALSE(tdps::haveArrayAlongBucketDataPath(obj, "data.h.a.b"));
        ASSERT_TRUE(tdps::haveArrayAlongBucketDataPath(obj, "data.i"));
        ASSERT_TRUE(tdps::haveArrayAlongBucketDataPath(obj, "data.i.a"));
    });
}

TEST_F(TimeseriesDottedPathSupportTest, fieldContainsArrayData) {
    BSONObj input = ::mongo::fromjson(R"(
{
    control: {
        min: {
            a: 1.0,
            b: true,
            c: 1.0,
            d: 1.0,
            e: [],
            f: [],
            g: 1.0,
            h: {
                a: true,
                b: 1.0,
                c: []
            },
            i: {
                a: 1.0,
                b: [],
                c: false,
                d: { 
                    a: 1.0
                },
                e: {
                    a: 1.0
                },
                f: 1.0,
                g: {
                    a: 1.0,
                    b: {},
                    c: {
                        a: 1.0,
                        b: true
                    },
                    d: [],
                    e: true
                }
            }
        },
        max: {
            a: true,
            b: true,
            c: 1.0,
            d: [],
            e: true,
            f: [],
            g: {
                a: true,
                b: 1.0,
                c: []
            },
            h: true,
            i: {
                a: true,
                b: [],
                c: true,
                d: {
                    a: true
                },
                e: {
                    a: {}
                },
                f: {
                    a: 1.0,
                    b: {},
                    c: {
                        a: 1.0,
                        b: true
                    },
                    d: [],
                    e: true
                },
                g: true
            }
        }
    },
    data: {
        time: {
            "0": {"$date": "1970-01-01T00:00:00.000Z"}
        }
    }
})");

    runTest(input, [this](const BSONObj& obj) {
        // Because this function is meant as an optimization to avoid a more expensive check, we
        // need to ensure that we don't make wrong decisions, but we can always fall through to the
        // more expensive check. So if the *best* decision that the function could make with the
        // information present in the control fields is "Yes", we will accept "Yes" or "Maybe".
        // Similarly if the best decision it could make is "No", we will accept "No" or "Maybe". If
        // there isn't enough information in the control fields to make a firm decision, then it
        // must return "Maybe".

        // A few assumptions about type orders necessary to understand the below tests:
        //    eoo < double < array < bool

        constexpr auto yes = tdps::Decision::Yes;
        constexpr auto no = tdps::Decision::No;
        constexpr auto maybe = tdps::Decision::Maybe;

        // a: {min: double, max: bool},
        ASSERT_EQ(maybe, tdps::fieldContainsArrayData(obj, "a"));

        // b: {min: bool, max: bool}
        ASSERT_NE(yes, tdps::fieldContainsArrayData(obj, "b"));

        // c: {min: double, max: double}
        ASSERT_NE(yes, tdps::fieldContainsArrayData(obj, "c"));

        // d: {min: double, max: array}
        ASSERT_NE(no, tdps::fieldContainsArrayData(obj, "d"));

        // e: {min: array, max: bool}
        ASSERT_NE(no, tdps::fieldContainsArrayData(obj, "e"));

        // f: {min: array, max: array}
        ASSERT_NE(no, tdps::fieldContainsArrayData(obj, "f"));

        // g: {min: double, max: object}
        ASSERT_NE(yes, tdps::fieldContainsArrayData(obj, "g"));
        // g.a: {min: double.eoo, max: object.bool}
        ASSERT_EQ(maybe, tdps::fieldContainsArrayData(obj, "g.a"));
        // g.b: {min: double.eoo, max: object.double}
        ASSERT_NE(yes, tdps::fieldContainsArrayData(obj, "g.b"));
        // g.c: {min: double.eoo, max: object.array}
        ASSERT_NE(no, tdps::fieldContainsArrayData(obj, "g.c"));
        // g.d: {min: double.eoo, max: object.eoo}
        ASSERT_NE(yes, tdps::fieldContainsArrayData(obj, "g.d"));

        // h: {min: object, max: bool}
        ASSERT_EQ(maybe, tdps::fieldContainsArrayData(obj, "h"));
        // h.a: {min: object.bool, max: bool.eoo}
        ASSERT_EQ(maybe, tdps::fieldContainsArrayData(obj, "h.a"));
        // h.b: {min: object.double, max: bool.eoo}
        ASSERT_EQ(maybe, tdps::fieldContainsArrayData(obj, "h.b"));
        // h.c: {min: object.array, max: bool.eoo}
        ASSERT_EQ(maybe, tdps::fieldContainsArrayData(obj, "h.c"));
        // h.d: {min: object.eoo, max: bool.eoo}
        ASSERT_EQ(maybe, tdps::fieldContainsArrayData(obj, "h.d"));

        // i: {min: object, max: object}
        ASSERT_NE(yes, tdps::fieldContainsArrayData(obj, "i"));
        // i.a: {min: object.double, max: object.bool}
        ASSERT_EQ(maybe, tdps::fieldContainsArrayData(obj, "i.a"));
        // i.b: {min: object.array, max: object.array}
        ASSERT_NE(no, tdps::fieldContainsArrayData(obj, "i.b"));
        // i.c: {min: object.bool, max: object.bool}
        ASSERT_NE(yes, tdps::fieldContainsArrayData(obj, "i.c"));
        // i.d: {min: object.object, max: object.object}
        ASSERT_NE(yes, tdps::fieldContainsArrayData(obj, "i.d"));
        // i.d.a: {min: object.object.double, max: object.object.bool}
        ASSERT_EQ(maybe, tdps::fieldContainsArrayData(obj, "i.d.a"));
        // i.e: {min: object.object, max: object.object}
        ASSERT_NE(yes, tdps::fieldContainsArrayData(obj, "i.e"));
        // i.e.a: {min: object.object.double, max: object.object.object}
        ASSERT_NE(yes, tdps::fieldContainsArrayData(obj, "i.e.a"));
        // i.f: {min: object.double, max: object.object}
        ASSERT_NE(yes, tdps::fieldContainsArrayData(obj, "i.f"));
        // i.f.a: {min: object.double.eoo, max: object.object.double}
        ASSERT_NE(yes, tdps::fieldContainsArrayData(obj, "i.f.a"));
        // i.f.b: {min: object.double.eoo, max: object.object.object}
        ASSERT_NE(yes, tdps::fieldContainsArrayData(obj, "i.f.b"));
        // i.f.c: {min: object.double.eoo, max: object.object.object}
        ASSERT_NE(yes, tdps::fieldContainsArrayData(obj, "i.f.c"));
        // i.f.c.a: {min: object.double.eoo.eoo, max: object.object.object.double}
        ASSERT_NE(yes, tdps::fieldContainsArrayData(obj, "i.f.c.a"));
        // i.f.c.b: {min: object.double.eoo.eoo, max: object.object.object.bool}
        ASSERT_EQ(maybe, tdps::fieldContainsArrayData(obj, "i.f.c.b"));
        // i.f.d: {min: object.double.eoo, max: object.object.array}
        ASSERT_NE(no, tdps::fieldContainsArrayData(obj, "i.f.d"));
        // i.f.e: {min: object.double.eoo, max: object.object.bool}
        ASSERT_EQ(maybe, tdps::fieldContainsArrayData(obj, "i.f.e"));
        // i.g: {min: object.object, max: object.bool}
        ASSERT_EQ(maybe, tdps::fieldContainsArrayData(obj, "i.g"));
        // i.g.a: {min: object.object.double, max: object.bool.eoo}
        ASSERT_EQ(maybe, tdps::fieldContainsArrayData(obj, "i.g.a"));
        // i.g.b: {min: object.object.object, max: object.bool.eoo}
        ASSERT_EQ(maybe, tdps::fieldContainsArrayData(obj, "i.g.b"));
        // i.g.c: {min: object.object.object, max: object.bool.eoo}
        ASSERT_EQ(maybe, tdps::fieldContainsArrayData(obj, "i.g.c"));
        // i.g.c.a: {min: object.object.object.double, max: object.bool.eoo.eoo}
        ASSERT_EQ(maybe, tdps::fieldContainsArrayData(obj, "i.g.c.a"));
        // i.g.c.b: {min: object.object.object.bool, max: object.bool.eoo.eoo}
        ASSERT_EQ(maybe, tdps::fieldContainsArrayData(obj, "i.g.c.b"));
        // i.g.d: {min: object.object.array, max: object.bool.eoo}
        ASSERT_NE(no, tdps::fieldContainsArrayData(obj, "i.g.d"));
        // i.g.e: {min: object.object.bool, max: object.bool.eoo}
        ASSERT_EQ(maybe, tdps::fieldContainsArrayData(obj, "i.g.e"));
    });
}

TEST_F(TimeseriesDottedPathSupportTest, ExtractAllElementsAlongBucketPath) {
    BSONObj input = ::mongo::fromjson(R"(
{
    control: {version: 1},
    data: {
        time: {
            "0": {"$date": "1970-01-01T00:00:00.000Z"},
            "1": {"$date": "1970-01-01T00:00:00.001Z"},
            "3": {"$date": "1970-01-01T00:00:00.003Z"},
            "99": {"$date": "1970-01-01T00:00:00.099Z"}
        },
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
        },
        h: {
            "1": {
                a: {
                    b: true
                }
            },
            "3": {
                a: {
                    b: false
                }
            }
        },
        i: {
            "1": [
                {a: true},
                {a: false}
            ]
        }
    }
})");

    runTest(input, [this](const BSONObj& obj) {
        auto assertExtractionMatches = [&](StringData path, const BSONArray expectedStorage) {
            BSONElementSet actual;
            auto actualStorage = tdps::extractAllElementsAlongBucketPath(obj, path, actual);

            BSONElementSet expected;
            for (auto&& el : expectedStorage) {
                expected.emplace(el);
            }

            ASSERT_EQ(actual.size(), expected.size())
                << "Expected path '" << path << "' to yield " << expectedStorage << " from " << obj;

            auto actualIt = actual.begin();
            auto expectedIt = expected.begin();
            while (actualIt != actual.end() && expectedIt != expected.end()) {
                ASSERT_FALSE(actualIt->eoo());
                ASSERT_FALSE(expectedIt->eoo());
                ASSERT_EQ(actualIt->woCompare(*expectedIt, 0), 0);
                actualIt++;
                expectedIt++;
            }
        };

        assertExtractionMatches("data.a"_sd, BSONArray());
        assertExtractionMatches("data.b"_sd, BSONArray());
        assertExtractionMatches("data.c"_sd, BSON_ARRAY(true << false));
        assertExtractionMatches("data.d"_sd, BSON_ARRAY(false));
        assertExtractionMatches("data.e"_sd, BSON_ARRAY("foo" << 1 << 2));
        assertExtractionMatches("data.f"_sd, BSON_ARRAY(BSON("a" << BSON_ARRAY(true << false))));
        assertExtractionMatches("data.f.a"_sd, BSON_ARRAY(true << false));
        assertExtractionMatches("data.g"_sd,
                                BSON_ARRAY(BSON("a" << BSON_ARRAY(BSON("a" << true)))));
        assertExtractionMatches("data.g.a"_sd, BSON_ARRAY(BSON("a" << true)));
        assertExtractionMatches("data.g.a.a"_sd, BSON_ARRAY(true));
        assertExtractionMatches(
            "data.h"_sd,
            BSON_ARRAY(BSON("a" << BSON("b" << true)) << BSON("a" << BSON("b" << false))));
        assertExtractionMatches("data.h.a"_sd, BSON_ARRAY(BSON("b" << true) << BSON("b" << false)));
        assertExtractionMatches("data.h.a.b"_sd, BSON_ARRAY(true << false));
        assertExtractionMatches("data.i.a"_sd, BSON_ARRAY(true << false));
    });
}
}  // namespace
}  // namespace mongo
