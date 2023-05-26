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
#include "mongo/db/query/aggregate_request_shapifier.h"
#include "mongo/db/query/find_request_shapifier.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_stats.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/inline_auto_update.h"
#include "mongo/unittest/unittest.h"

namespace mongo::query_stats {
/**
 * A default hmac application strategy that generates easy to check results for testing purposes.
 */
std::string applyHmacForTest(StringData s) {
    return str::stream() << "HASH<" << s << ">";
}

std::size_t hash(const BSONObj& obj) {
    return absl::hash_internal::CityHash64(obj.objdata(), obj.objsize());
}

class QueryStatsStoreTest : public ServiceContextTest {
public:
    BSONObj makeQueryStatsKeyFindRequest(
        const FindCommandRequest& fcr,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        bool applyHmac = false,
        LiteralSerializationPolicy literalPolicy = LiteralSerializationPolicy::kUnchanged) {
        auto fcrCopy = std::make_unique<FindCommandRequest>(fcr);
        auto parsedFind = uassertStatusOK(parsed_find_command::parse(expCtx, std::move(fcrCopy)));
        FindRequestShapifier findShapifier(expCtx, *parsedFind);

        SerializationOptions opts;
        if (literalPolicy != LiteralSerializationPolicy::kUnchanged) {
            // TODO SERVER-75400 Use only 'literalPolicy.'
            opts.replacementForLiteralArgs = "?";
            opts.literalPolicy = literalPolicy;
        }
        if (applyHmac) {
            opts.applyHmacToIdentifiers = true;
            opts.identifierHmacPolicy = applyHmacForTest;
        }
        return findShapifier.makeQueryStatsKey(opts, expCtx);
    }

    BSONObj makeTelemetryKeyAggregateRequest(
        AggregateCommandRequest acr,
        const Pipeline& pipeline,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        bool applyHmac = false,
        LiteralSerializationPolicy literalPolicy = LiteralSerializationPolicy::kUnchanged) {
        AggregateRequestShapifier aggShapifier(acr, pipeline, expCtx);

        SerializationOptions opts;
        if (literalPolicy != LiteralSerializationPolicy::kUnchanged) {
            // TODO SERVER-75419 Use only 'literalPolicy.'
            opts.replacementForLiteralArgs = "?";
            opts.literalPolicy = literalPolicy;
        }
        if (applyHmac) {
            opts.applyHmacToIdentifiers = true;
            opts.identifierHmacPolicy = applyHmacForTest;
        }
        return aggShapifier.makeQueryStatsKey(opts, expCtx);
    }
};

TEST_F(QueryStatsStoreTest, BasicUsage) {
    QueryStatsStore telStore{5000000, 1000};

    auto getMetrics = [&](const BSONObj& key) {
        auto lookupResult = telStore.lookup(hash(key));
        return *lookupResult.getValue();
    };

    auto collectMetrics = [&](BSONObj& key) {
        std::shared_ptr<QueryStatsEntry> metrics;
        auto lookupResult = telStore.lookup(hash(key));
        if (!lookupResult.isOK()) {
            telStore.put(hash(key), std::make_shared<QueryStatsEntry>(nullptr, NamespaceString{}));
            lookupResult = telStore.lookup(hash(key));
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
        auto [lookupResult, lock] = telStore.getWithPartitionLock(hash(key));
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
        [&](std::size_t key, const std::shared_ptr<QueryStatsEntry>& entry) { numKeys++; });

    ASSERT_EQ(numKeys, 2);
}


TEST_F(QueryStatsStoreTest, EvictEntries) {
    // This creates a queryStats store with 2 partitions, each with a size of 1200 bytes.
    const auto cacheSize = 2400;
    const auto numPartitions = 2;
    QueryStatsStore telStore{cacheSize, numPartitions};

    for (int i = 0; i < 30; i++) {
        auto query = BSON("query" + std::to_string(i) << 1 << "xEquals" << 42);
        telStore.put(hash(query), std::make_shared<QueryStatsEntry>(nullptr, NamespaceString{}));
    }
    int numKeys = 0;
    telStore.forEach(
        [&](std::size_t key, const std::shared_ptr<QueryStatsEntry>& entry) { numKeys++; });

    int entriesPerPartition =
        (cacheSize / numPartitions) / (sizeof(std::size_t) + sizeof(QueryStatsEntry));

    ASSERT_EQ(numKeys, entriesPerPartition * numPartitions);
}

TEST_F(QueryStatsStoreTest, CorrectlyRedactsFindCommandRequestAllFields) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    FindCommandRequest fcr(NamespaceStringOrUUID(NamespaceString("testDB.testColl")));

    fcr.setFilter(BSON("a" << 1));

    auto key = makeQueryStatsKeyFindRequest(
        fcr, expCtx, true, LiteralSerializationPolicy::kToDebugTypeString);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "HASH<testDB>",
                    "coll": "HASH<testColl>"
                },
                "command": "find",
                "filter": {
                    "HASH<a>": {
                        "$eq": "?number"
                    }
                }
            }
        })",
        key);

    // Add sort.
    fcr.setSort(BSON("sortVal" << 1 << "otherSort" << -1));
    key = makeQueryStatsKeyFindRequest(
        fcr, expCtx, true, LiteralSerializationPolicy::kToDebugTypeString);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "HASH<testDB>",
                    "coll": "HASH<testColl>"
                },
                "command": "find",
                "filter": {
                    "HASH<a>": {
                        "$eq": "?number"
                    }
                },
                "sort": {
                    "HASH<sortVal>": 1,
                    "HASH<otherSort>": -1
                }
            }
        })",
        key);

    // Add inclusion projection.
    fcr.setProjection(BSON("e" << true << "f" << true));
    key = makeQueryStatsKeyFindRequest(
        fcr, expCtx, true, LiteralSerializationPolicy::kToDebugTypeString);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "HASH<testDB>",
                    "coll": "HASH<testColl>"
                },
                "command": "find",
                "filter": {
                    "HASH<a>": {
                        "$eq": "?number"
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
            }
        })",
        key);

    // Add let.
    fcr.setLet(BSON("var1" << 1 << "var2"
                           << "const1"));
    key = makeQueryStatsKeyFindRequest(
        fcr, expCtx, true, LiteralSerializationPolicy::kToDebugTypeString);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "HASH<testDB>",
                    "coll": "HASH<testColl>"
                },
                "command": "find",
                "filter": {
                    "HASH<a>": {
                        "$eq": "?number"
                    }
                },
                "let": {
                    "HASH<var1>": "?number",
                    "HASH<var2>": "?string"
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
            }
        })",
        key);

    // Add hinting fields.
    fcr.setHint(BSON("z" << 1 << "c" << 1));
    fcr.setMax(BSON("z" << 25));
    fcr.setMin(BSON("z" << 80));
    key = makeQueryStatsKeyFindRequest(
        fcr, expCtx, true, LiteralSerializationPolicy::kToDebugTypeString);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "HASH<testDB>",
                    "coll": "HASH<testColl>"
                },
                "command": "find",
                "filter": {
                    "HASH<a>": {
                        "$eq": "?number"
                    }
                },
                "let": {
                    "HASH<var1>": "?number",
                    "HASH<var2>": "?string"
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
            }
        })",
        key);

    // Add the literal redaction fields.
    fcr.setLimit(5);
    fcr.setSkip(2);
    fcr.setBatchSize(25);
    fcr.setMaxTimeMS(1000);
    fcr.setNoCursorTimeout(false);

    key = makeQueryStatsKeyFindRequest(
        fcr, expCtx, true, LiteralSerializationPolicy::kToDebugTypeString);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "HASH<testDB>",
                    "coll": "HASH<testColl>"
                },
                "command": "find",
                "filter": {
                    "HASH<a>": {
                        "$eq": "?number"
                    }
                },
                "let": {
                    "HASH<var1>": "?number",
                    "HASH<var2>": "?string"
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
                "limit": "?number",
                "skip": "?number"
            },
            "maxTimeMS": "?number",
            "batchSize": "?number"
            }
        )",
        key);

    // Add the fields that shouldn't be hmacApplied.
    fcr.setSingleBatch(true);
    fcr.setAllowDiskUse(false);
    fcr.setAllowPartialResults(true);
    fcr.setAllowDiskUse(false);
    fcr.setShowRecordId(true);
    fcr.setAwaitData(false);
    fcr.setMirrored(true);
    key = makeQueryStatsKeyFindRequest(
        fcr, expCtx, true, LiteralSerializationPolicy::kToDebugTypeString);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "HASH<testDB>",
                    "coll": "HASH<testColl>"
                },
                "command": "find",
                "filter": {
                    "HASH<a>": {
                        "$eq": "?number"
                    }
                },
                "let": {
                    "HASH<var1>": "?number",
                    "HASH<var2>": "?string"
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
                "limit": "?number",
                "skip": "?number",
                "singleBatch": "?bool",
                "allowDiskUse": "?bool",
                "showRecordId": "?bool",
                "awaitData": "?bool",
                "mirrored": "?bool"
            },
            "allowPartialResults": true,
            "maxTimeMS": "?number",
            "batchSize": "?number"
        })",
        key);

    fcr.setAllowPartialResults(false);
    key = makeQueryStatsKeyFindRequest(
        fcr, expCtx, true, LiteralSerializationPolicy::kToDebugTypeString);
    // Make sure that a false allowPartialResults is also accurately captured.
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "HASH<testDB>",
                    "coll": "HASH<testColl>"
                },
                "command": "find",
                "filter": {
                    "HASH<a>": {
                        "$eq": "?number"
                    }
                },
                "let": {
                    "HASH<var1>": "?number",
                    "HASH<var2>": "?string"
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
                "limit": "?number",
                "skip": "?number",
                "singleBatch": "?bool",
                "allowDiskUse": "?bool",
                "showRecordId": "?bool",
                "awaitData": "?bool",
                "mirrored": "?bool"
            },
            "allowPartialResults": false,
            "maxTimeMS": "?number",
            "batchSize": "?number"
        })",
        key);
}

TEST_F(QueryStatsStoreTest, CorrectlyRedactsFindCommandRequestEmptyFields) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    FindCommandRequest fcr(NamespaceStringOrUUID(NamespaceString("testDB.testColl")));
    fcr.setFilter(BSONObj());
    fcr.setSort(BSONObj());
    fcr.setProjection(BSONObj());

    auto hmacApplied = makeQueryStatsKeyFindRequest(
        fcr, expCtx, true, LiteralSerializationPolicy::kToDebugTypeString);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "HASH<testDB>",
                    "coll": "HASH<testColl>"
                },
                "command": "find",
                "filter": {}
            }
        })",
        hmacApplied);  // NOLINT (test auto-update)
}

TEST_F(QueryStatsStoreTest, CorrectlyRedactsHintsWithOptions) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    FindCommandRequest fcr(NamespaceStringOrUUID(NamespaceString("testDB.testColl")));

    fcr.setFilter(BSON("b" << 1));
    fcr.setHint(BSON("z" << 1 << "c" << 1));
    fcr.setMax(BSON("z" << 25));
    fcr.setMin(BSON("z" << 80));

    auto key = makeQueryStatsKeyFindRequest(
        fcr, expCtx, false, LiteralSerializationPolicy::kToDebugTypeString);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "testDB",
                    "coll": "testColl"
                },
                "command": "find",
                "filter": {
                    "b": {
                        "$eq": "?number"
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
            }
        })",
        key);
    // Test with a string hint. Note that this is the internal representation of the string hint
    // generated at parse time.
    fcr.setHint(BSON("$hint"
                     << "z"));

    key = makeQueryStatsKeyFindRequest(
        fcr, expCtx, false, LiteralSerializationPolicy::kToDebugTypeString);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "testDB",
                    "coll": "testColl"
                },
                "command": "find",
                "filter": {
                    "b": {
                        "$eq": "?number"
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
            }
        })",
        key);

    fcr.setHint(BSON("z" << 1 << "c" << 1));
    key = makeQueryStatsKeyFindRequest(fcr, expCtx, true, LiteralSerializationPolicy::kUnchanged);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "HASH<testDB>",
                    "coll": "HASH<testColl>"
                },
                "command": "find",
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
            }
        })",
        key);

    key = makeQueryStatsKeyFindRequest(
        fcr, expCtx, true, LiteralSerializationPolicy::kToDebugTypeString);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "HASH<testDB>",
                    "coll": "HASH<testColl>"
                },
                "command": "find",
                "filter": {
                    "HASH<b>": {
                        "$eq": "?number"
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
            }
        })",
        key);

    // Test that $natural comes through unmodified.
    fcr.setHint(BSON("$natural" << -1));
    key = makeQueryStatsKeyFindRequest(
        fcr, expCtx, true, LiteralSerializationPolicy::kToDebugTypeString);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "HASH<testDB>",
                    "coll": "HASH<testColl>"
                },
                "command": "find",
                "filter": {
                    "HASH<b>": {
                        "$eq": "?number"
                    }
                },
                "hint": {
                    "$natural": -1
                },
                "max": {
                    "HASH<z>": "?"
                },
                "min": {
                    "HASH<z>": "?"
                }
            }
        })",
        key);
}

TEST_F(QueryStatsStoreTest, DefinesLetVariables) {
    // Test that the expression context we use to apply hmac will understand the 'let' part of the
    // find command while parsing the other pieces of the command.

    // Note that this ExpressionContext will not have the let variables defined - we expect the
    // 'makeQueryStatsKey' call to do that.
    auto opCtx = makeOperationContext();
    auto fcr = std::make_unique<FindCommandRequest>(
        NamespaceStringOrUUID(NamespaceString("testDB.testColl")));
    fcr->setLet(BSON("var" << 2));
    fcr->setFilter(fromjson("{$expr: [{$eq: ['$a', '$$var']}]}"));
    fcr->setProjection(fromjson("{varIs: '$$var'}"));

    const auto cmdObj = fcr->toBSON(BSON("$db"
                                         << "testDB"));
    auto&& [expCtx, parsedFind] =
        uassertStatusOK(parsed_find_command::parse(opCtx.get(), std::move(fcr)));
    QueryStatsEntry testMetrics{
        std::make_unique<query_stats::FindRequestShapifier>(expCtx, *parsedFind),
        parsedFind->findCommandRequest->getNamespaceOrUUID()};

    bool applyHmacToIdentifiers = false;
    auto hmacApplied =
        testMetrics.computeQueryStatsKey(opCtx.get(), applyHmacToIdentifiers, std::string{});
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "testDB",
                    "coll": "testColl"
                },
                "command": "find",
                "filter": {
                    "$expr": [
                        {
                            "$eq": [
                                "$a",
                                "$$var"
                            ]
                        }
                    ]
                },
                "let": {
                    "var": "?number"
                },
                "projection": {
                    "varIs": "$$var",
                    "_id": true
                }
            }
        })",
        hmacApplied);

    // Now be sure hmac is applied to variable names. We don't currently expose a different way to
    // do the hashing, so we'll just stick with the big long strings here for now.
    applyHmacToIdentifiers = true;
    hmacApplied =
        testMetrics.computeQueryStatsKey(opCtx.get(), applyHmacToIdentifiers, std::string{});
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "IyuPUD33jXD1td/VA/JyhbOPYY0MdGkXgdExniXmCyg=",
                    "coll": "QFhYnXorzWDLwH/wBgpXxp8fkfsZKo4n2cIN/O0uf/c="
                },
                "command": "find",
                "filter": {
                    "$expr": [
                        {
                            "$eq": [
                                "$lhWpXUozYRjENbnNVMXoZEq5VrVzqikmJ0oSgLZnRxM=",
                                "$$adaJc6H3zDirh5/52MLv5yvnb6nXNP15Z4HzGfumvx8="
                            ]
                        }
                    ]
                },
                "let": {
                    "adaJc6H3zDirh5/52MLv5yvnb6nXNP15Z4HzGfumvx8=": "?number"
                },
                "projection": {
                    "BL649QER7lTs0+8ozTMVNAa6JNjbhf57YT8YQ4EkT1E=": "$$adaJc6H3zDirh5/52MLv5yvnb6nXNP15Z4HzGfumvx8=",
                    "ljovqLSfuj6o2syO1SynOzHQK1YVij6+Wlx1fL8frUo=": true
                }
            }
        })",
        hmacApplied);
}

TEST_F(QueryStatsStoreTest, CorrectlyRedactsAggregateCommandRequestAllFieldsSimplePipeline) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    AggregateCommandRequest acr(NamespaceString("testDB.testColl"));
    auto matchStage = fromjson(R"({
            $match: {
                foo: { $in: ["a", "b"] },
                bar: { $gte: { $date: "2022-01-01T00:00:00Z" } }
            }
        })");
    auto unwindStage = fromjson("{$unwind: '$x'}");
    auto groupStage = fromjson(R"({
            $group: {
                _id: "$_id",
                c: { $first: "$d.e" },
                f: { $sum: 1 }
            }
        })");
    auto limitStage = fromjson("{$limit: 10}");
    auto outStage = fromjson(R"({$out: 'outColl'})");
    auto rawPipeline = {matchStage, unwindStage, groupStage, limitStage, outStage};
    acr.setPipeline(rawPipeline);
    auto pipeline = Pipeline::parse(rawPipeline, expCtx);

    auto shapified = makeTelemetryKeyAggregateRequest(acr, *pipeline, expCtx);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "testDB",
                    "coll": "testColl"
                },
                "command": "aggregate",
                "pipeline": [
                    {
                        "$match": {
                            "foo": {
                                "$in": [
                                    "a",
                                    "b"
                                ]
                            },
                            "bar": {
                                "$gte": {"$date":"2022-01-01T00:00:00.000Z"}
                            }
                        }
                    },
                    {
                        "$unwind": {
                            "path": "$x"
                        }
                    },
                    {
                        "$group": {
                            "_id": "$_id",
                            "c": {
                                "$first": "$d.e"
                            },
                            "f": {
                                "$sum": {
                                    "$const": 1
                                }
                            }
                        }
                    },
                    {
                        "$limit": 10
                    },
                    {
                        "$out": {
                            "coll": "outColl",
                            "db": "test"
                        }
                    }
                ]
            }
        })",
        shapified);

    shapified = makeTelemetryKeyAggregateRequest(
        acr, *pipeline, expCtx, true, LiteralSerializationPolicy::kToDebugTypeString);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "HASH<testDB>",
                    "coll": "HASH<testColl>"
                },
                "command": "aggregate",
                "pipeline": [
                    {
                        "$match": {
                            "$and": [
                                {
                                    "HASH<foo>": {
                                        "$in": "?array<?string>"
                                    }
                                },
                                {
                                    "HASH<bar>": {
                                        "$gte": "?date"
                                    }
                                }
                            ]
                        }
                    },
                    {
                        "$unwind": {
                            "path": "$HASH<x>"
                        }
                    },
                    {
                        "$group": {
                            "_id": "$HASH<_id>",
                            "HASH<c>": {
                                "$first": "$HASH<d>.HASH<e>"
                            },
                            "HASH<f>": {
                                "$sum": "?number"
                            }
                        }
                    },
                    {
                        "$limit": "?"
                    },
                    {
                        "$out": {
                            "coll": "HASH<outColl>",
                            "db": "HASH<test>"
                        }
                    }
                ]
            }
        })",
        shapified);

    // Add the fields that shouldn't be abstracted.
    acr.setExplain(ExplainOptions::Verbosity::kExecStats);
    acr.setAllowDiskUse(false);
    acr.setHint(BSON("z" << 1 << "c" << 1));
    acr.setCollation(BSON("locale"
                          << "simple"));
    shapified = makeTelemetryKeyAggregateRequest(
        acr, *pipeline, expCtx, true, LiteralSerializationPolicy::kToDebugTypeString);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "HASH<testDB>",
                    "coll": "HASH<testColl>"
                },
                "command": "aggregate",
                "pipeline": [
                    {
                        "$match": {
                            "$and": [
                                {
                                    "HASH<foo>": {
                                        "$in": "?array<?string>"
                                    }
                                },
                                {
                                    "HASH<bar>": {
                                        "$gte": "?date"
                                    }
                                }
                            ]
                        }
                    },
                    {
                        "$unwind": {
                            "path": "$HASH<x>"
                        }
                    },
                    {
                        "$group": {
                            "_id": "$HASH<_id>",
                            "HASH<c>": {
                                "$first": "$HASH<d>.HASH<e>"
                            },
                            "HASH<f>": {
                                "$sum": "?number"
                            }
                        }
                    },
                    {
                        "$limit": "?"
                    },
                    {
                        "$out": {
                            "coll": "HASH<outColl>",
                            "db": "HASH<test>"
                        }
                    }
                ],
                "explain": true,
                "allowDiskUse": false,
                "collation": {
                    "locale": "simple"
                },
                "hint": {
                    "HASH<z>": 1,
                    "HASH<c>": 1
                }
            }
        })",
        shapified);

    // Add let.
    acr.setLet(BSON("var1"
                    << "$foo"
                    << "var2"
                    << "bar"));
    shapified = makeTelemetryKeyAggregateRequest(
        acr, *pipeline, expCtx, true, LiteralSerializationPolicy::kToDebugTypeString);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "HASH<testDB>",
                    "coll": "HASH<testColl>"
                },
                "command": "aggregate",
                "pipeline": [
                    {
                        "$match": {
                            "$and": [
                                {
                                    "HASH<foo>": {
                                        "$in": "?array<?string>"
                                    }
                                },
                                {
                                    "HASH<bar>": {
                                        "$gte": "?date"
                                    }
                                }
                            ]
                        }
                    },
                    {
                        "$unwind": {
                            "path": "$HASH<x>"
                        }
                    },
                    {
                        "$group": {
                            "_id": "$HASH<_id>",
                            "HASH<c>": {
                                "$first": "$HASH<d>.HASH<e>"
                            },
                            "HASH<f>": {
                                "$sum": "?number"
                            }
                        }
                    },
                    {
                        "$limit": "?"
                    },
                    {
                        "$out": {
                            "coll": "HASH<outColl>",
                            "db": "HASH<test>"
                        }
                    }
                ],
                "explain": true,
                "allowDiskUse": false,
                "collation": {
                    "locale": "simple"
                },
                "hint": {
                    "HASH<z>": 1,
                    "HASH<c>": 1
                },
                "let": {
                    "HASH<var1>": "$HASH<foo>",
                    "HASH<var2>": "?string"
                }
            }
        })",
        shapified);

    // Add the fields that should be abstracted.
    auto cursorOptions = SimpleCursorOptions();
    cursorOptions.setBatchSize(10);
    acr.setCursor(cursorOptions);
    acr.setMaxTimeMS(500);
    acr.setBypassDocumentValidation(true);
    expCtx->opCtx->setComment(BSON("comment"
                                   << "note to self"));
    shapified = makeTelemetryKeyAggregateRequest(
        acr, *pipeline, expCtx, true, LiteralSerializationPolicy::kToDebugTypeString);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "HASH<testDB>",
                    "coll": "HASH<testColl>"
                },
                "command": "aggregate",
                "pipeline": [
                    {
                        "$match": {
                            "$and": [
                                {
                                    "HASH<foo>": {
                                        "$in": "?array<?string>"
                                    }
                                },
                                {
                                    "HASH<bar>": {
                                        "$gte": "?date"
                                    }
                                }
                            ]
                        }
                    },
                    {
                        "$unwind": {
                            "path": "$HASH<x>"
                        }
                    },
                    {
                        "$group": {
                            "_id": "$HASH<_id>",
                            "HASH<c>": {
                                "$first": "$HASH<d>.HASH<e>"
                            },
                            "HASH<f>": {
                                "$sum": "?number"
                            }
                        }
                    },
                    {
                        "$limit": "?"
                    },
                    {
                        "$out": {
                            "coll": "HASH<outColl>",
                            "db": "HASH<test>"
                        }
                    }
                ],
                "explain": true,
                "allowDiskUse": false,
                "collation": {
                    "locale": "simple"
                },
                "hint": {
                    "HASH<z>": 1,
                    "HASH<c>": 1
                },
                "let": {
                    "HASH<var1>": "$HASH<foo>",
                    "HASH<var2>": "?string"
                }
            },
            "cursor": {
                "batchSize": "?number"
            },
            "maxTimeMS": "?number",
            "bypassDocumentValidation": "?bool",
            "comment": "?string"
        })",
        shapified);
}
TEST_F(QueryStatsStoreTest, CorrectlyRedactsAggregateCommandRequestEmptyFields) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    AggregateCommandRequest acr(NamespaceString("testDB.testColl"));
    acr.setPipeline({});
    auto pipeline = Pipeline::parse({}, expCtx);

    auto shapified = makeTelemetryKeyAggregateRequest(
        acr, *pipeline, expCtx, true, LiteralSerializationPolicy::kToDebugTypeString);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "HASH<testDB>",
                    "coll": "HASH<testColl>"
                },
                "command": "aggregate",
                "pipeline": []
            }
        })",
        shapified);  // NOLINT (test auto-update)
}
}  // namespace mongo::query_stats
