/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/telemetry.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/inline_auto_update.h"
#include "mongo/unittest/unittest.h"

namespace mongo::telemetry {

class TelemetryStoreTest : public ServiceContextTest {};

TEST_F(TelemetryStoreTest, BasicUsage) {
    TelemetryStore telStore{5000000, 1000};

    auto getMetrics = [&](BSONObj& key) {
        auto lookupResult = telStore.lookup(key);
        return *lookupResult.getValue();
    };

    auto collectMetrics = [&](BSONObj& key) {
        std::shared_ptr<TelemetryMetrics> metrics;
        auto lookupResult = telStore.lookup(key);
        if (!lookupResult.isOK()) {
            telStore.put(
                key, std::make_shared<TelemetryMetrics>(BSONObj(), boost::none, NamespaceString{}));
            lookupResult = telStore.lookup(key);
        }
        metrics = *lookupResult.getValue();
        metrics->execCount += 1;
        metrics->lastExecutionMicros += 123456;
    };

    auto query1 = BSON("query" << 1 << "xEquals" << 42);
    // same value, different instance (tests hashing & equality)
    auto query1x = BSON("query" << 1 << "xEquals" << 42);
    auto query2 = BSON("query" << 2 << "yEquals" << 43);

    collectMetrics(query1);
    collectMetrics(query1);
    collectMetrics(query1x);
    collectMetrics(query2);

    ASSERT_EQ(getMetrics(query1)->execCount, 3);
    ASSERT_EQ(getMetrics(query1x)->execCount, 3);
    ASSERT_EQ(getMetrics(query2)->execCount, 1);

    auto collectMetricsWithLock = [&](BSONObj& key) {
        auto [lookupResult, lock] = telStore.getWithPartitionLock(key);
        auto metrics = *lookupResult.getValue();
        metrics->execCount += 1;
        metrics->lastExecutionMicros += 123456;
    };

    collectMetricsWithLock(query1x);
    collectMetricsWithLock(query2);

    ASSERT_EQ(getMetrics(query1)->execCount, 4);
    ASSERT_EQ(getMetrics(query1x)->execCount, 4);
    ASSERT_EQ(getMetrics(query2)->execCount, 2);

    int numKeys = 0;

    telStore.forEach(
        [&](const BSONObj& key, const std::shared_ptr<TelemetryMetrics>& entry) { numKeys++; });

    ASSERT_EQ(numKeys, 2);
}


TEST_F(TelemetryStoreTest, EvictEntries) {
    // This creates a telemetry store with 2 partitions, each with a size of 1200 bytes.
    const auto cacheSize = 2400;
    const auto numPartitions = 2;
    TelemetryStore telStore{cacheSize, numPartitions};

    for (int i = 0; i < 20; i++) {
        auto query = BSON("query" + std::to_string(i) << 1 << "xEquals" << 42);
        telStore.put(query,
                     std::make_shared<TelemetryMetrics>(BSONObj(), boost::none, NamespaceString{}));
    }
    int numKeys = 0;
    telStore.forEach(
        [&](const BSONObj& key, const std::shared_ptr<TelemetryMetrics>& entry) { numKeys++; });

    int entriesPerPartition = (cacheSize / numPartitions) / (46 + sizeof(TelemetryMetrics));
    ASSERT_EQ(numKeys, entriesPerPartition * numPartitions);
}

/**
 * A default redaction strategy that generates easy to check results for testing purposes.
 */
std::string redactFieldNameForTest(StringData s) {
    return str::stream() << "HASH<" << s << ">";
}
TEST_F(TelemetryStoreTest, CorrectlyRedactsFindCommandRequestAllFields) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    FindCommandRequest fcr(NamespaceStringOrUUID(NamespaceString("testDB.testColl")));
    fcr.setFilter(BSON("a" << 1));
    SerializationOptions opts;
    opts.replacementForLiteralArgs = "?";
    opts.redactIdentifiers = true;
    opts.identifierRedactionPolicy = redactFieldNameForTest;

    auto redacted = uassertStatusOK(telemetry::makeTelemetryKey(fcr, opts, expCtx));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "HASH<testDB>",
                "coll": "HASH<testColl>"
            },
            "find": "HASH<testColl>",
            "filter": {
                "HASH<a>": {
                    "$eq": "?"
                }
            }
        })",
        redacted);

    // Add sort.
    fcr.setSort(BSON("sortVal" << 1 << "otherSort" << -1));
    redacted = uassertStatusOK(telemetry::makeTelemetryKey(fcr, opts, expCtx));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "HASH<testDB>",
                "coll": "HASH<testColl>"
            },
            "find": "HASH<testColl>",
            "filter": {
                "HASH<a>": {
                    "$eq": "?"
                }
            },
            "sort": {
                "HASH<sortVal>": 1,
                "HASH<otherSort>": -1
            }
        })",
        redacted);

    // Add inclusion projection.
    fcr.setProjection(BSON("e" << true << "f" << true));
    redacted = uassertStatusOK(telemetry::makeTelemetryKey(fcr, opts, expCtx));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "HASH<testDB>",
                "coll": "HASH<testColl>"
            },
            "find": "HASH<testColl>",
            "filter": {
                "HASH<a>": {
                    "$eq": "?"
                }
            },
            "projection": {
                "HASH<e>": true,
                "HASH<f>": true,
                "HASH<_id>": true
            },
            "sort": {
                "HASH<sortVal>": 1,
                "HASH<otherSort>": -1
            }
        })",
        redacted);

    // Add let.
    fcr.setLet(BSON("var1"
                    << "$a"
                    << "var2"
                    << "const1"));
    redacted = uassertStatusOK(telemetry::makeTelemetryKey(fcr, opts, expCtx));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "HASH<testDB>",
                "coll": "HASH<testColl>"
            },
            "find": "HASH<testColl>",
            "filter": {
                "HASH<a>": {
                    "$eq": "?"
                }
            },
            "let": {
                "HASH<var1>": "$HASH<a>",
                "HASH<var2>": {
                    "$const": "?"
                }
            },
            "projection": {
                "HASH<e>": true,
                "HASH<f>": true,
                "HASH<_id>": true
            },
            "sort": {
                "HASH<sortVal>": 1,
                "HASH<otherSort>": -1
            }
        })",
        redacted);

    // Add hinting fields.
    fcr.setHint(BSON("z" << 1 << "c" << 1));
    fcr.setMax(BSON("z" << 25));
    fcr.setMin(BSON("z" << 80));
    redacted = uassertStatusOK(telemetry::makeTelemetryKey(fcr, opts, expCtx));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "HASH<testDB>",
                "coll": "HASH<testColl>"
            },
            "find": "HASH<testColl>",
            "filter": {
                "HASH<a>": {
                    "$eq": "?"
                }
            },
            "let": {
                "HASH<var1>": "$HASH<a>",
                "HASH<var2>": {
                    "$const": "?"
                }
            },
            "projection": {
                "HASH<e>": true,
                "HASH<f>": true,
                "HASH<_id>": true
            },
            "hint": {
                "HASH<z>": 1,
                "HASH<c>": 1
            },
            "max": {
                "HASH<z>": "?"
            },
            "min": {
                "HASH<z>": "?"
            },
            "sort": {
                "HASH<sortVal>": 1,
                "HASH<otherSort>": -1
            }
        })",
        redacted);

    // Add the literal redaction fields.
    fcr.setLimit(5);
    fcr.setSkip(2);
    fcr.setBatchSize(25);
    fcr.setMaxTimeMS(1000);
    fcr.setNoCursorTimeout(false);

    redacted = uassertStatusOK(telemetry::makeTelemetryKey(fcr, opts, expCtx));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "HASH<testDB>",
                "coll": "HASH<testColl>"
            },
            "find": "HASH<testColl>",
            "filter": {
                "HASH<a>": {
                    "$eq": "?"
                }
            },
            "let": {
                "HASH<var1>": "$HASH<a>",
                "HASH<var2>": {
                    "$const": "?"
                }
            },
            "projection": {
                "HASH<e>": true,
                "HASH<f>": true,
                "HASH<_id>": true
            },
            "hint": {
                "HASH<z>": 1,
                "HASH<c>": 1
            },
            "max": {
                "HASH<z>": "?"
            },
            "min": {
                "HASH<z>": "?"
            },
            "sort": {
                "HASH<sortVal>": 1,
                "HASH<otherSort>": -1
            },
            "limit": "?",
            "skip": "?",
            "batchSize": "?",
            "maxTimeMS": "?"
        })",
        redacted);

    // Add the fields that shouldn't be redacted.
    fcr.setSingleBatch(true);
    fcr.setAllowDiskUse(false);
    fcr.setAllowPartialResults(true);
    fcr.setAllowDiskUse(false);
    fcr.setShowRecordId(true);
    fcr.setAwaitData(false);
    fcr.setMirrored(true);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "HASH<testDB>",
                "coll": "HASH<testColl>"
            },
            "find": "HASH<testColl>",
            "filter": {
                "HASH<a>": {
                    "$eq": "?"
                }
            },
            "let": {
                "HASH<var1>": "$HASH<a>",
                "HASH<var2>": {
                    "$const": "?"
                }
            },
            "projection": {
                "HASH<e>": true,
                "HASH<f>": true,
                "HASH<_id>": true
            },
            "hint": {
                "HASH<z>": 1,
                "HASH<c>": 1
            },
            "max": {
                "HASH<z>": "?"
            },
            "min": {
                "HASH<z>": "?"
            },
            "sort": {
                "HASH<sortVal>": 1,
                "HASH<otherSort>": -1
            },
            "limit": "?",
            "skip": "?",
            "batchSize": "?",
            "maxTimeMS": "?"
        })",
        redacted);
}
TEST_F(TelemetryStoreTest, CorrectlyRedactsFindCommandRequestEmptyFields) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    FindCommandRequest fcr(NamespaceStringOrUUID(NamespaceString("testDB.testColl")));
    fcr.setFilter(BSONObj());
    fcr.setSort(BSONObj());
    fcr.setProjection(BSONObj());
    SerializationOptions opts;
    opts.replacementForLiteralArgs = "?";
    opts.redactIdentifiers = true;
    opts.identifierRedactionPolicy = redactFieldNameForTest;

    auto redacted = uassertStatusOK(telemetry::makeTelemetryKey(fcr, opts, expCtx));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "HASH<testDB>",
                "coll": "HASH<testColl>"
            },
            "find": "HASH<testColl>",
            "filter": {}
        })",
        redacted);  // NOLINT (test auto-update)
}

TEST_F(TelemetryStoreTest, CorrectlyRedactsHintsWithOptions) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    FindCommandRequest fcr(NamespaceStringOrUUID(NamespaceString("testDB.testColl")));
    fcr.setFilter(BSON("b" << 1));
    SerializationOptions opts;
    opts.replacementForLiteralArgs = "?";
    fcr.setHint(BSON("z" << 1 << "c" << 1));
    fcr.setMax(BSON("z" << 25));
    fcr.setMin(BSON("z" << 80));

    auto redacted = uassertStatusOK(telemetry::makeTelemetryKey(fcr, opts, expCtx));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "find": "testColl",
            "filter": {
                "b": {
                    "$eq": "?"
                }
            },
            "hint": {
                "z": 1,
                "c": 1
            },
            "max": {
                "z": "?"
            },
            "min": {
                "z": "?"
            }
        })",
        redacted);
    // Test with a string hint. Note that this is the internal representation of the string hint
    // generated at parse time.
    fcr.setHint(BSON("$hint"
                     << "z"));

    redacted = uassertStatusOK(telemetry::makeTelemetryKey(fcr, opts, expCtx));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "find": "testColl",
            "filter": {
                "b": {
                    "$eq": "?"
                }
            },
            "hint": {
                "$hint": "z"
            },
            "max": {
                "z": "?"
            },
            "min": {
                "z": "?"
            }
        })",
        redacted);

    fcr.setHint(BSON("z" << 1 << "c" << 1));
    opts.identifierRedactionPolicy = redactFieldNameForTest;
    opts.redactIdentifiers = true;
    opts.replacementForLiteralArgs = boost::none;
    redacted = uassertStatusOK(telemetry::makeTelemetryKey(fcr, opts, expCtx));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "HASH<testDB>",
                "coll": "HASH<testColl>"
            },
            "find": "HASH<testColl>",
            "filter": {
                "HASH<b>": {
                    "$eq": 1
                }
            },
            "hint": {
                "HASH<z>": 1,
                "HASH<c>": 1
            },
            "max": {
                "HASH<z>": 25
            },
            "min": {
                "HASH<z>": 80
            }
        })",
        redacted);

    redacted = uassertStatusOK(telemetry::makeTelemetryKey(fcr, opts, expCtx));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "HASH<testDB>",
                "coll": "HASH<testColl>"
            },
            "find": "HASH<testColl>",
            "filter": {
                "HASH<b>": {
                    "$eq": 1
                }
            },
            "hint": {
                "HASH<z>": 1,
                "HASH<c>": 1
            },
            "max": {
                "HASH<z>": 25
            },
            "min": {
                "HASH<z>": 80
            }
        })",
        redacted);

    opts.replacementForLiteralArgs = "?";
    redacted = uassertStatusOK(telemetry::makeTelemetryKey(fcr, opts, expCtx));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "HASH<testDB>",
                "coll": "HASH<testColl>"
            },
            "find": "HASH<testColl>",
            "filter": {
                "HASH<b>": {
                    "$eq": "?"
                }
            },
            "hint": {
                "HASH<z>": 1,
                "HASH<c>": 1
            },
            "max": {
                "HASH<z>": "?"
            },
            "min": {
                "HASH<z>": "?"
            }
        })",
        redacted);

    redacted = uassertStatusOK(telemetry::makeTelemetryKey(fcr, opts, expCtx));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "HASH<testDB>",
                "coll": "HASH<testColl>"
            },
            "find": "HASH<testColl>",
            "filter": {
                "HASH<b>": {
                    "$eq": "?"
                }
            },
            "hint": {
                "HASH<z>": 1,
                "HASH<c>": 1
            },
            "max": {
                "HASH<z>": "?"
            },
            "min": {
                "HASH<z>": "?"
            }
        })",
        redacted);
}

}  // namespace mongo::telemetry
