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

#include <absl/hash/hash.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/collection_type.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/parsed_find_command.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/query_stats/agg_key_generator.h"
#include "mongo/db/query/query_stats/find_key_generator.h"
#include "mongo/db/query/query_stats/key_generator.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/query/query_stats/transform_algorithm_gen.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

namespace mongo::query_stats {

int countAllEntries(const QueryStatsStore& store) {
    int numKeys = 0;
    store.forEach([&](auto&& key, auto&& entry) { numKeys++; });
    return numKeys;
}

static const NamespaceStringOrUUID kDefaultTestNss =
    NamespaceStringOrUUID{NamespaceString::createNamespaceString_forTest("testDB.testColl")};
class QueryStatsStoreTest : public ServiceContextTest {
public:
    static std::unique_ptr<const KeyGenerator> makeFindKeyGeneratorFromQuery(BSONObj filter) {
        auto expCtx = make_intrusive<ExpressionContextForTest>();
        auto fcr = std::make_unique<FindCommandRequest>(kDefaultTestNss);
        fcr->setFilter(filter.getOwned());
        auto parsedFind = uassertStatusOK(parsed_find_command::parse(expCtx, std::move(fcr)));
        return std::make_unique<FindKeyGenerator>(expCtx, *parsedFind, collectionType);
    }

    static constexpr auto collectionType = query_shape::CollectionType::kCollection;
    BSONObj makeQueryStatsKeyFindRequest(const FindCommandRequest& fcr,
                                         const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         bool applyHmac) {
        auto fcrCopy = std::make_unique<FindCommandRequest>(fcr);
        auto parsedFind = uassertStatusOK(parsed_find_command::parse(expCtx, std::move(fcrCopy)));
        FindKeyGenerator findKeyGenerator(expCtx, *parsedFind, collectionType);
        SerializationOptions opts = SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST;
        if (!applyHmac) {
            opts.transformIdentifiers = false;
            opts.transformIdentifiersCallback = defaultHmacStrategy;
        }
        return findKeyGenerator.generate(expCtx->opCtx, opts, SerializationContext::stateDefault());
    }

    BSONObj makeTelemetryKeyAggregateRequest(
        AggregateCommandRequest acr,
        const Pipeline& pipeline,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        bool applyHmac = false,
        LiteralSerializationPolicy literalPolicy = LiteralSerializationPolicy::kUnchanged) {
        auto aggKeyGenerator = std::make_unique<AggKeyGenerator>(acr,
                                                                 pipeline,
                                                                 expCtx,
                                                                 pipeline.getInvolvedCollections(),
                                                                 acr.getNamespace(),
                                                                 collectionType);

        // SerializationOptions opts{.literalPolicy = literalPolicy};
        SerializationOptions opts = SerializationOptions::kMarkIdentifiers_FOR_TEST;
        opts.literalPolicy = literalPolicy;
        if (!applyHmac) {
            opts.transformIdentifiers = false;
            opts.transformIdentifiersCallback = defaultHmacStrategy;
        }
        return aggKeyGenerator->generate(expCtx->opCtx, opts, SerializationContext::stateDefault());
    }
};

TEST_F(QueryStatsStoreTest, BasicUsage) {
    QueryStatsStore queryStatsStore{5000000, 1000};

    auto getMetrics = [&](BSONObj query) {
        auto key = makeFindKeyGeneratorFromQuery(query);
        auto lookupResult = queryStatsStore.lookup(absl::HashOf(key));
        ASSERT_OK(lookupResult);
        return *lookupResult.getValue();
    };

    auto collectMetrics = [&](BSONObj query) {
        auto key = makeFindKeyGeneratorFromQuery(query);
        auto lookupHash = absl::HashOf(key);
        auto lookupResult = queryStatsStore.lookup(lookupHash);
        if (!lookupResult.isOK()) {
            queryStatsStore.put(lookupHash, QueryStatsEntry{std::move(key)});
            lookupResult = queryStatsStore.lookup(lookupHash);
        }
        auto metrics = lookupResult.getValue();
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

    ASSERT_EQ(getMetrics(query1).execCount, 3);
    ASSERT_EQ(getMetrics(query1x).execCount, 3);
    ASSERT_EQ(getMetrics(query2).execCount, 1);

    auto collectMetricsWithLock = [&](BSONObj& filter) {
        auto key = makeFindKeyGeneratorFromQuery(filter);
        auto [lookupResult, lock] = queryStatsStore.getWithPartitionLock(absl::HashOf(key));
        ASSERT_OK(lookupResult);
        auto& metrics = *lookupResult.getValue();
        metrics.execCount += 1;
        metrics.lastExecutionMicros += 123456;
    };

    collectMetricsWithLock(query1x);
    collectMetricsWithLock(query2);

    ASSERT_EQ(getMetrics(query1).execCount, 4);
    ASSERT_EQ(getMetrics(query1x).execCount, 4);
    ASSERT_EQ(getMetrics(query2).execCount, 2);

    ASSERT_EQ(2, countAllEntries(queryStatsStore));
}

TEST_F(QueryStatsStoreTest, EvictionTest) {
    // This creates a queryStats store with a single partition to specifically test the eviction
    // behavior with very large queries.
    // Add an entry that is smaller than the max partition size.
    auto query = BSON("query" << 1 << "xEquals" << 42);
    auto keyGenerator = makeFindKeyGeneratorFromQuery(query);

    const size_t cacheSize = keyGenerator->size() + sizeof(QueryStatsEntry) + 100;
    const auto numPartitions = 1;
    QueryStatsStore queryStatsStore{cacheSize, numPartitions};

    auto hash = absl::HashOf(keyGenerator);
    queryStatsStore.put(hash, QueryStatsEntry{std::move(keyGenerator)});
    ASSERT_EQ(countAllEntries(queryStatsStore), 1);

    // We'll do this again later so save this as a helper function.
    auto addLargeEntry = [&](auto& queryStatsStore) {
        // Add an entry that is larger than the max partition size to the non-empty partition. This
        // should evict both entries, the first small entry written to the partition and the current
        // too large entry we wish to write to the partition. The reason is because entries are
        // evicted from the partition in order of least recently used. Thus, the small entry will be
        // evicted first but the partition will still be over budget so the final, too large entry
        // will also be evicted.
        auto opCtx = makeOperationContext();
        auto fcr = std::make_unique<FindCommandRequest>(NamespaceStringOrUUID(
            NamespaceString::createNamespaceString_forTest("testDB.testColl")));
        fcr->setLet(BSON("var" << 2));
        fcr->setFilter(fromjson("{$expr: [{$eq: ['$a', '$$var']}]}"));
        fcr->setProjection(fromjson("{varIs: '$$var'}"));
        fcr->setLimit(5);
        fcr->setSkip(2);
        fcr->setBatchSize(25);
        fcr->setMaxTimeMS(1000);
        fcr->setNoCursorTimeout(false);
        opCtx->setComment(BSON("comment"
                               << " foo"));
        fcr->setSingleBatch(false);
        fcr->setAllowDiskUse(false);
        fcr->setAllowPartialResults(true);
        fcr->setAllowDiskUse(false);
        fcr->setShowRecordId(true);
        fcr->setMirrored(true);
        fcr->setHint(BSON("z" << 1 << "c" << 1));
        fcr->setMax(BSON("z" << 25));
        fcr->setMin(BSON("z" << 80));
        fcr->setSort(BSON("sortVal" << 1 << "otherSort" << -1));
        auto&& [expCtx, parsedFind] =
            uassertStatusOK(parsed_find_command::parse(opCtx.get(), std::move(fcr)));

        keyGenerator =
            std::make_unique<query_stats::FindKeyGenerator>(expCtx, *parsedFind, collectionType);
        auto lookupHash = absl::HashOf(keyGenerator);
        QueryStatsEntry testMetrics{std::move(keyGenerator)};
        queryStatsStore.put(lookupHash, testMetrics);
    };

    addLargeEntry(queryStatsStore);
    ASSERT_EQ(countAllEntries(queryStatsStore), 0);

    // This creates a queryStats store where each partition has a max size of 500 bytes.
    QueryStatsStore queryStatsStoreTwo{/*cacheSize*/ cacheSize * 3, /*numPartitions*/ 3};
    // Adding a queryStats store entry that is smaller than the overal cache size but larger
    // than a single partition max size, will cause an eviction. testMetrics is larger than 500
    // bytes and thus over budget for the partitions of this cache.
    addLargeEntry(queryStatsStoreTwo);
    ASSERT_EQ(countAllEntries(queryStatsStoreTwo), 0);
}

TEST_F(QueryStatsStoreTest, GenerateMaxBsonSizeQueryShape) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("testDB.testColl");
    FindCommandRequest fcr((NamespaceStringOrUUID(nss)));
    // This creates a query that is just below the 16 MB memory limit.
    int limit = 225500;
    BSONObjBuilder bob;
    BSONArrayBuilder andBob(bob.subarrayStart("$and"));
    for (int i = 1; i <= limit; i++) {
        BSONObjBuilder childrenBob;
        childrenBob.append("x", BSON("$lt" << i << "$gte" << i));
        andBob.append(childrenBob.obj());
    }
    andBob.doneFast();
    fcr.setFilter(bob.obj());
    auto fcrCopy = std::make_unique<FindCommandRequest>(fcr);
    auto opCtx = makeOperationContext();
    auto parsedFindPair =
        uassertStatusOK(parsed_find_command::parse(opCtx.get(), std::move(fcrCopy)));
    RAIIServerParameterControllerForTest controller("featureFlagQueryStats", true);
    RAIIServerParameterControllerForTest queryKnobController{"internalQueryStatsRateLimit", -1};

    auto&& globalQueryStatsStoreManager = queryStatsStoreDecoration(opCtx->getServiceContext());
    globalQueryStatsStoreManager = std::make_unique<QueryStatsStoreManager>(500000, 1000);

    // The shapification process will bloat the input query over the 16 MB memory limit. Assert
    // that calling registerRequest() doesn't throw and that the opDebug isn't registered with a
    // key hash (thus metrics won't be tracked for this query).
    ASSERT_DOES_NOT_THROW(query_stats::registerRequest(
        opCtx.get(),
        nss,
        [&]() {
            return std::make_unique<query_stats::FindKeyGenerator>(
                parsedFindPair.first,
                *parsedFindPair.second,
                query_shape::CollectionType::kCollection);
        },
        /*requiresFullQueryStatsFeatureFlag*/ false));
    auto& opDebug = CurOp::get(*opCtx)->debug();
    ASSERT_EQ(opDebug.queryStatsStoreKeyHash, boost::none);
}

TEST_F(QueryStatsStoreTest, CorrectlyRedactsFindCommandRequestAllFields) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    FindCommandRequest fcr(kDefaultTestNss);

    fcr.setFilter(BSON("a" << 1));

    auto key = makeQueryStatsKeyFindRequest(fcr, expCtx, true);

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
            },
            "collectionType": "collection"
        })",
        key);

    // Add sort.
    fcr.setSort(BSON("sortVal" << 1 << "otherSort" << -1));
    key = makeQueryStatsKeyFindRequest(fcr, expCtx, true);
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
            },
            "collectionType": "collection"
        })",
        key);

    // Add inclusion projection.
    fcr.setProjection(BSON("e" << true << "f" << true));
    key = makeQueryStatsKeyFindRequest(fcr, expCtx, true);
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
            },
            "collectionType": "collection"
        })",
        key);

    // Add let.
    fcr.setLet(BSON("var1" << 1 << "var2"
                           << "const1"));
    key = makeQueryStatsKeyFindRequest(fcr, expCtx, true);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "HASH<testDB>",
                    "coll": "HASH<testColl>"
                },
                "let": {
                    "HASH<var1>": "?number",
                    "HASH<var2>": "?string"
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
            },
            "collectionType": "collection"
        })",
        key);

    // Add hinting fields.
    fcr.setHint(BSON("z" << 1 << "c" << 1));
    fcr.setMax(BSON("z" << 25));
    fcr.setMin(BSON("z" << 80));
    key = makeQueryStatsKeyFindRequest(fcr, expCtx, true);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "HASH<testDB>",
                    "coll": "HASH<testColl>"
                },
                "let": {
                    "HASH<var1>": "?number",
                    "HASH<var2>": "?string"
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
                "max": {
                    "HASH<z>": "?number"
                },
                "min": {
                    "HASH<z>": "?number"
                },
                "sort": {
                    "HASH<sortVal>": 1,
                    "HASH<otherSort>": -1
                }
            },
            "collectionType": "collection",
            "hint": {
                "HASH<z>": 1,
                "HASH<c>": 1
            }
        })",
        key);

    // Add the literal redaction fields.
    fcr.setLimit(5);
    fcr.setSkip(2);
    fcr.setBatchSize(25);
    fcr.setMaxTimeMS(1000);
    fcr.setNoCursorTimeout(false);

    key = makeQueryStatsKeyFindRequest(fcr, expCtx, true);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "HASH<testDB>",
                    "coll": "HASH<testColl>"
                },
                "let": {
                    "HASH<var1>": "?number",
                    "HASH<var2>": "?string"
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
                "max": {
                    "HASH<z>": "?number"
                },
                "min": {
                    "HASH<z>": "?number"
                },
                "sort": {
                    "HASH<sortVal>": 1,
                    "HASH<otherSort>": -1
                },
                "limit": "?number",
                "skip": "?number"
            },
            "collectionType": "collection",
            "hint": {
                "HASH<z>": 1,
                "HASH<c>": 1
            },
            "noCursorTimeout": "?bool",
            "maxTimeMS": "?number",
            "batchSize": "?number"
        })",
        key);

    // Add the fields that shouldn't be hmacApplied.
    fcr.setSingleBatch(true);
    fcr.setAllowDiskUse(false);
    fcr.setAllowPartialResults(true);
    fcr.setAllowDiskUse(false);
    fcr.setShowRecordId(true);
    fcr.setMirrored(true);
    key = makeQueryStatsKeyFindRequest(fcr, expCtx, true);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "HASH<testDB>",
                    "coll": "HASH<testColl>"
                },
                "let": {
                    "HASH<var1>": "?number",
                    "HASH<var2>": "?string"
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
                "max": {
                    "HASH<z>": "?number"
                },
                "min": {
                    "HASH<z>": "?number"
                },
                "sort": {
                    "HASH<sortVal>": 1,
                    "HASH<otherSort>": -1
                },
                "limit": "?number",
                "skip": "?number",
                "singleBatch": true,
                "allowDiskUse": false,
                "showRecordId": true,
                "mirrored": true
            },
            "collectionType": "collection",
            "hint": {
                "HASH<z>": 1,
                "HASH<c>": 1
            },
            "allowPartialResults": true,
            "noCursorTimeout": "?bool",
            "maxTimeMS": "?number",
            "batchSize": "?number"
        })",
        key);

    fcr.setAllowPartialResults(false);
    key = makeQueryStatsKeyFindRequest(fcr, expCtx, true);
    // Make sure that a false allowPartialResults is also accurately captured.
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "HASH<testDB>",
                    "coll": "HASH<testColl>"
                },
                "let": {
                    "HASH<var1>": "?number",
                    "HASH<var2>": "?string"
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
                "max": {
                    "HASH<z>": "?number"
                },
                "min": {
                    "HASH<z>": "?number"
                },
                "sort": {
                    "HASH<sortVal>": 1,
                    "HASH<otherSort>": -1
                },
                "limit": "?number",
                "skip": "?number",
                "singleBatch": true,
                "allowDiskUse": false,
                "showRecordId": true,
                "mirrored": true
            },
            "collectionType": "collection",
            "hint": {
                "HASH<z>": 1,
                "HASH<c>": 1
            },
            "allowPartialResults": false,
            "noCursorTimeout": "?bool",
            "maxTimeMS": "?number",
            "batchSize": "?number"
        })",
        key);

    FindCommandRequest fcr2(
        NamespaceStringOrUUID(NamespaceString::createNamespaceString_forTest("testDB.testColl")));
    fcr2.setAwaitData(true);
    fcr2.setTailable(true);
    fcr2.setSort(BSON("$natural" << 1));
    key = makeQueryStatsKeyFindRequest(fcr2, expCtx, true);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "HASH<testDB>",
                    "coll": "HASH<testColl>"
                },
                "command": "find",
                "filter": {},
                "tailable": true,
                "awaitData": true
            },
            "collectionType": "collection",
            "hint": {
                "$natural": 1
            }
        })",
        key);
}

TEST_F(QueryStatsStoreTest, CorrectlyRedactsFindCommandRequestEmptyFields) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    FindCommandRequest fcr(
        NamespaceStringOrUUID(NamespaceString::createNamespaceString_forTest("testDB.testColl")));
    fcr.setFilter(BSONObj());
    fcr.setSort(BSONObj());
    fcr.setProjection(BSONObj());

    auto hmacApplied = makeQueryStatsKeyFindRequest(fcr, expCtx, true);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "HASH<testDB>",
                    "coll": "HASH<testColl>"
                },
                "command": "find",
                "filter": {}
            },
            "collectionType": "collection"
        })",
        hmacApplied);  // NOLINT (test auto-update)
}

TEST_F(QueryStatsStoreTest, CorrectlyRedactsHintsWithOptions) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    FindCommandRequest fcr(
        NamespaceStringOrUUID(NamespaceString::createNamespaceString_forTest("testDB.testColl")));

    fcr.setFilter(BSON("b" << 1));
    fcr.setHint(BSON("z" << 1 << "c" << 1));
    fcr.setMax(BSON("z" << 25));
    fcr.setMin(BSON("z" << 80));

    auto key = makeQueryStatsKeyFindRequest(fcr, expCtx, false);

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
                "max": {
                    "z": "?number"
                },
                "min": {
                    "z": "?number"
                }
            },
            "collectionType": "collection",
            "hint": {
                "z": 1,
                "c": 1
            }
        })",
        key);
    // Test with a string hint. Note that this is the internal representation of the string hint
    // generated at parse time.
    fcr.setHint(BSON("$hint"
                     << "z"));

    key = makeQueryStatsKeyFindRequest(fcr, expCtx, false);
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
                "max": {
                    "z": "?number"
                },
                "min": {
                    "z": "?number"
                }
            },
            "collectionType": "collection",
            "hint": {
                "$hint": "z"
            }
        })",
        key);

    fcr.setHint(BSON("z" << 1 << "c" << 1));
    key = makeQueryStatsKeyFindRequest(fcr, expCtx, true);
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
                "max": {
                    "HASH<z>": "?number"
                },
                "min": {
                    "HASH<z>": "?number"
                }
            },
            "collectionType": "collection",
            "hint": {
                "HASH<z>": 1,
                "HASH<c>": 1
            }
        })",
        key);

    // Test that $natural comes through unmodified.
    fcr.setHint(BSON("$natural" << -1));
    key = makeQueryStatsKeyFindRequest(fcr, expCtx, true);
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
                "max": {
                    "HASH<z>": "?number"
                },
                "min": {
                    "HASH<z>": "?number"
                }
            },
            "collectionType": "collection",
            "hint": {
                "$natural": -1
            }
        })",
        key);
}

TEST_F(QueryStatsStoreTest, DefinesLetVariables) {
    // Test that the expression context we use to apply hmac will understand the 'let' part of
    // the find command while parsing the other pieces of the command.

    // Note that this ExpressionContext will not have the let variables defined - we expect the
    // 'makeQueryStatsKey' call to do that.
    auto opCtx = makeOperationContext();
    auto fcr = std::make_unique<FindCommandRequest>(
        NamespaceStringOrUUID(NamespaceString::createNamespaceString_forTest("testDB.testColl")));
    fcr->setLet(BSON("var" << 2));
    fcr->setFilter(fromjson("{$expr: [{$eq: ['$a', '$$var']}]}"));
    fcr->setProjection(fromjson("{varIs: '$$var'}"));

    auto expCtx = make_intrusive<ExpressionContextForTest>(opCtx.get());
    expCtx->variables.seedVariablesWithLetParameters(expCtx.get(), *fcr->getLet());
    auto hmacApplied = makeQueryStatsKeyFindRequest(*fcr, expCtx, false);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "testDB",
                    "coll": "testColl"
                },
                "let": {
                    "var": "?number"
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
                "projection": {
                    "varIs": "$$var",
                    "_id": true
                }
            },
            "collectionType": "collection"
        })",
        hmacApplied);

    hmacApplied = makeQueryStatsKeyFindRequest(*fcr, expCtx, true);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "HASH<testDB>",
                    "coll": "HASH<testColl>"
                },
                "let": {
                    "HASH<var>": "?number"
                },
                "command": "find",
                "filter": {
                    "$expr": [
                        {
                            "$eq": [
                                "$HASH<a>",
                                "$$HASH<var>"
                            ]
                        }
                    ]
                },
                "projection": {
                    "HASH<varIs>": "$$HASH<var>",
                    "HASH<_id>": true
                }
            },
            "collectionType": "collection"
        })",
        hmacApplied);
}

TEST_F(QueryStatsStoreTest, CorrectlyTokenizesAggregateCommandRequestAllFieldsSimplePipeline) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    AggregateCommandRequest acr(NamespaceString::createNamespaceString_forTest("testDB.testColl"));
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
                            "db": "testDB"
                        }
                    }
                ]
            },
            "collectionType": "collection"
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
                        "$limit": "?number"
                    },
                    {
                        "$out": {
                            "coll": "HASH<outColl>",
                            "db": "HASH<testDB>"
                        }
                    }
                ]
            },
            "collectionType": "collection"
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
                "collation": {
                    "locale": "simple"
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
                        "$limit": "?number"
                    },
                    {
                        "$out": {
                            "coll": "HASH<outColl>",
                            "db": "HASH<testDB>"
                        }
                    }
                ],
                "explain": true,
                "allowDiskUse": false
            },
            "collectionType": "collection",
            "hint": {
                "HASH<z>": 1,
                "HASH<c>": 1
            }
        })",
        shapified);

    // Add let.
    acr.setLet(BSON("var1" << BSON("$literal"
                                   << "$foo")
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
                "collation": {
                    "locale": "simple"
                },
                "let": {
                    "HASH<var1>": "?string",
                    "HASH<var2>": "?string"
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
                        "$limit": "?number"
                    },
                    {
                        "$out": {
                            "coll": "HASH<outColl>",
                            "db": "HASH<testDB>"
                        }
                    }
                ],
                "explain": true,
                "allowDiskUse": false
            },
            "collectionType": "collection",
            "hint": {
                "HASH<z>": 1,
                "HASH<c>": 1
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
                "collation": {
                    "locale": "simple"
                },
                "let": {
                    "HASH<var1>": "?string",
                    "HASH<var2>": "?string"
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
                        "$limit": "?number"
                    },
                    {
                        "$out": {
                            "coll": "HASH<outColl>",
                            "db": "HASH<testDB>"
                        }
                    }
                ],
                "explain": true,
                "allowDiskUse": false
            },
            "comment": "?string",
            "collectionType": "collection",
            "hint": {
                "HASH<z>": 1,
                "HASH<c>": 1
            },
            "cursor": {
                "batchSize": "?number"
            },
            "maxTimeMS": "?number",
            "bypassDocumentValidation": "?bool"
        })",
        shapified);
}
TEST_F(QueryStatsStoreTest, CorrectlyTokenizesAggregateCommandRequestEmptyFields) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    AggregateCommandRequest acr(NamespaceString::createNamespaceString_forTest("testDB.testColl"));
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
            },
            "collectionType": "collection"
        })",
        shapified);  // NOLINT (test auto-update)
}

TEST_F(QueryStatsStoreTest,
       CorrectlyTokenizesAggregateCommandRequestPipelineWithSecondaryNamespaces) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto nsToUnionWith =
        NamespaceString::createNamespaceString_forTest(expCtx->ns.dbName(), "otherColl");
    expCtx->addResolvedNamespaces({nsToUnionWith});

    AggregateCommandRequest acr(
        NamespaceString::createNamespaceString_forTest(expCtx->ns.dbName(), "testColl"));
    auto unionWithStage = fromjson(R"({
            $unionWith: {
                coll: "otherColl",
                pipeline: [{$match: {val: "foo"}}]
            }
        })");
    auto sortStage = fromjson("{$sort: {age: 1}}");
    auto rawPipeline = {unionWithStage, sortStage};
    acr.setPipeline(rawPipeline);
    auto pipeline = Pipeline::parse(rawPipeline, expCtx);

    auto shapified = makeTelemetryKeyAggregateRequest(
        acr, *pipeline, expCtx, true, LiteralSerializationPolicy::kToDebugTypeString);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "HASH<test>",
                    "coll": "HASH<testColl>"
                },
                "command": "aggregate",
                "pipeline": [
                    {
                        "$unionWith": {
                            "coll": "HASH<otherColl>",
                            "pipeline": [
                                {
                                    "$match": {
                                        "HASH<val>": {
                                            "$eq": "?string"
                                        }
                                    }
                                }
                            ]
                        }
                    },
                    {
                        "$sort": {
                            "HASH<age>": 1
                        }
                    }
                ]
            },
            "collectionType": "collection",
            "otherNss": [
                {
                    "db": "HASH<test>",
                    "coll": "HASH<otherColl>"
                }
            ]
        })",
        shapified);
}
}  // namespace mongo::query_stats
