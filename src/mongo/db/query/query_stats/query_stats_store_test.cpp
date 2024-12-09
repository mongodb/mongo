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
#include "mongo/db/collection_type.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/query_stats/agg_key.h"
#include "mongo/db/query/query_stats/find_key.h"
#include "mongo/db/query/query_stats/key.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo::query_stats {

int countAllEntries(const QueryStatsStore& store) {
    int numKeys = 0;
    store.forEach([&](auto&& key, auto&& entry) { numKeys++; });
    return numKeys;
}

static const NamespaceStringOrUUID kDefaultTestNss = NamespaceString("testDB.testColl");
class QueryStatsStoreTest : public ServiceContextTest {
public:
    static std::unique_ptr<const Key> makeFindKeyFromQuery(BSONObj filter) {
        auto expCtx = make_intrusive<ExpressionContextForTest>();
        auto fcr = std::make_unique<FindCommandRequest>(kDefaultTestNss);
        fcr->setFilter(filter.getOwned());
        auto parsedFind = uassertStatusOK(parsed_find_command::parse(expCtx, std::move(fcr)));
        return std::make_unique<FindKey>(expCtx, *parsedFind, collectionType);
    }

    static constexpr auto collectionType = query_shape::CollectionType::kCollection;
    BSONObj makeQueryStatsKeyFindRequest(const FindCommandRequest& fcr,
                                         const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         bool applyHmac) {
        auto fcrCopy = std::make_unique<FindCommandRequest>(fcr);
        auto parsedFind = uassertStatusOK(parsed_find_command::parse(expCtx, std::move(fcrCopy)));
        FindKey findKey(expCtx, *parsedFind, collectionType);
        SerializationOptions opts = SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST;
        if (!applyHmac) {
            opts.transformIdentifiers = false;
            opts.transformIdentifiersCallback = defaultHmacStrategy;
        }
        return findKey.toBson(expCtx->opCtx, opts);
    }

    BSONObj makeQueryStatsKeyAggregateRequest(AggregateCommandRequest acr,
                                              const Pipeline& pipeline,
                                              const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                              LiteralSerializationPolicy literalPolicy,
                                              bool applyHmac = false) {
        auto aggKey = std::make_unique<AggKey>(acr,
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
        return aggKey->toBson(expCtx->opCtx, opts);
    }
};

TEST_F(QueryStatsStoreTest, BasicUsage) {
    QueryStatsStore queryStatsStore{5000000, 1000};

    auto getMetrics = [&](BSONObj query) {
        auto key = makeFindKeyFromQuery(query);
        auto lookupResult = queryStatsStore.lookup(absl::Hash<query_stats::Key>{}(*key));
        ASSERT_OK(lookupResult);
        return *lookupResult.getValue();
    };

    auto collectMetrics = [&](BSONObj query) {
        auto key = makeFindKeyFromQuery(query);
        auto lookupHash = absl::Hash<query_stats::Key>{}(*key);
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
        auto key = makeFindKeyFromQuery(filter);
        auto [lookupResult, lock] =
            queryStatsStore.getWithPartitionLock(absl::Hash<query_stats::Key>{}(*key));
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
    auto key = makeFindKeyFromQuery(query);

    const size_t cacheSize = key->size() + sizeof(QueryStatsEntry) + 100;
    const auto numPartitions = 1;
    QueryStatsStore queryStatsStore{cacheSize, numPartitions};

    auto hash = absl::Hash<query_stats::Key>{}(*key);
    queryStatsStore.put(hash, QueryStatsEntry{std::move(key)});
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
        auto fcr = std::make_unique<FindCommandRequest>(
            NamespaceStringOrUUID(NamespaceString("testDB.testColl")));
        fcr->setLet(BSON("var" << 2));
        fcr->setFilter(fromjson("{$expr: [{$eq: ['$a', '$$var']}]}"));
        fcr->setProjection(fromjson("{varIs: '$$var'}"));
        fcr->setLimit(5);
        fcr->setSkip(2);
        fcr->setBatchSize(25);
        fcr->setMaxTimeMS(1000);
        fcr->setNoCursorTimeout(false);
        opCtx->setComment(BSON("comment"
                               << " foo bar baz"));
        fcr->setSingleBatch(false);
        fcr->setAllowDiskUse(false);
        fcr->setAllowPartialResults(true);
        fcr->setAllowDiskUse(false);
        fcr->setShowRecordId(true);
        fcr->setHint(BSON("z" << 1 << "c" << 1));
        fcr->setMax(BSON("z" << 25));
        fcr->setMin(BSON("z" << 80));
        fcr->setSort(BSON("sortVal" << 1 << "otherSort" << -1));
        auto&& [expCtx, parsedFind] =
            uassertStatusOK(parsed_find_command::parse(opCtx.get(), std::move(fcr)));

        key = std::make_unique<query_stats::FindKey>(expCtx, *parsedFind, collectionType);
        auto lookupHash = absl::Hash<query_stats::Key>{}(*key);
        QueryStatsEntry testMetrics{std::move(key)};
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
    const NamespaceString nss = NamespaceString("testDB.testColl");
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

    auto&& globalQueryStatsStoreManager = QueryStatsStoreManager::get(opCtx->getServiceContext());
    globalQueryStatsStoreManager = std::make_unique<QueryStatsStoreManager>(500000, 1000);

    // The shapification process will bloat the input query over the 16 MB memory limit. Assert that
    // calling registerRequest() doesn't throw and that the opDebug isn't registered with a key hash
    // (thus metrics won't be tracked for this query).
    ASSERT_DOES_NOT_THROW(query_stats::registerRequest(opCtx.get(), nss, [&]() {
        return std::make_unique<query_stats::FindKey>(
            parsedFindPair.first, *parsedFindPair.second, query_shape::CollectionType::kCollection);
    }));
    auto& opDebug = CurOp::get(*opCtx)->debug();
    ASSERT_FALSE(opDebug.queryStatsInfo.keyHash.has_value());
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
            "maxTimeMS": "?number",
            "noCursorTimeout": false,
            "batchSize": "?number"
        })",
        key);

    // Add the fields that shouldn't be hmacApplied.
    fcr.setSingleBatch(true);
    fcr.setAllowDiskUse(false);
    fcr.setAllowPartialResults(true);
    fcr.setAllowDiskUse(false);
    fcr.setShowRecordId(true);
    auto readPreference = BSON("mode"
                               << "nearest"
                               << "tags"
                               << BSON_ARRAY(BSON("some"
                                                  << "tag")
                                             << BSON("some"
                                                     << "other tag")));
    ReadPreferenceSetting::get(expCtx->opCtx) =
        uassertStatusOK(ReadPreferenceSetting::fromInnerBSON(readPreference));
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
                "showRecordId": true
            },
            "$readPreference": { 
                "mode": "nearest", 
                "tags": [ { "some": "other tag" }, { "some": "tag" } ], 
                "hedge": { "enabled": true } 
            },
            "collectionType": "collection",
            "hint": {
                "HASH<z>": 1,
                "HASH<c>": 1
            },
            "maxTimeMS": "?number",
            "allowPartialResults": true,
            "noCursorTimeout": false,
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
                "showRecordId": true
            },
            "$readPreference": { 
                "mode": "nearest", 
                "tags": [ { "some": "other tag" }, { "some": "tag" } ], 
                "hedge": { "enabled": true } 
            },
            "collectionType": "collection",
            "hint": {
                "HASH<z>": 1,
                "HASH<c>": 1
            },
            "maxTimeMS": "?number",
            "allowPartialResults": false,
            "noCursorTimeout": false,
            "batchSize": "?number"
        })",
        key);
}

TEST_F(QueryStatsStoreTest, CorrectlyRedactsTailableFindCommandRequest) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();

    FindCommandRequest fcr(NamespaceStringOrUUID(NamespaceString("testDB.testColl")));
    fcr.setAwaitData(true);
    fcr.setTailable(true);
    fcr.setSort(BSON("$natural" << 1));
    auto key = makeQueryStatsKeyFindRequest(fcr, expCtx, true);
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
    FindCommandRequest fcr(NamespaceStringOrUUID(NamespaceString("testDB.testColl")));
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
    FindCommandRequest fcr(NamespaceStringOrUUID(NamespaceString("testDB.testColl")));

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
    auto fcr = std::make_unique<FindCommandRequest>(NamespaceString("testDB.testColl"));
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
    auto expCtx = make_intrusive<ExpressionContextForTest>(*kDefaultTestNss.nss());
    AggregateCommandRequest acr(*kDefaultTestNss.nss());
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

    auto shapified = makeQueryStatsKeyAggregateRequest(
        acr, *pipeline, expCtx, LiteralSerializationPolicy::kToDebugTypeString, true);
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
    acr.setAllowDiskUse(false);
    acr.setHint(BSON("z" << 1 << "c" << 1));
    acr.setCollation(BSON("locale"
                          << "simple"));
    shapified = makeQueryStatsKeyAggregateRequest(
        acr, *pipeline, expCtx, LiteralSerializationPolicy::kToDebugTypeString, true);
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
    shapified = makeQueryStatsKeyAggregateRequest(
        acr, *pipeline, expCtx, LiteralSerializationPolicy::kToDebugTypeString, true);
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
    shapified = makeQueryStatsKeyAggregateRequest(
        acr, *pipeline, expCtx, LiteralSerializationPolicy::kToDebugTypeString, true);
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
                "allowDiskUse": false
            },
            "comment": "?string",
            "collectionType": "collection",
            "hint": {
                "HASH<z>": 1,
                "HASH<c>": 1
            },
            "maxTimeMS": "?number",
            "bypassDocumentValidation": true,
            "cursor": {
                "batchSize": "?number"
            }
        })",
        shapified);

    // Test again but with the representative query shape.
    shapified = makeQueryStatsKeyAggregateRequest(
        acr, *pipeline, expCtx, LiteralSerializationPolicy::kToRepresentativeParseableValue, true);
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
                    "HASH<var1>": {
                        "$const": "?"
                    },
                    "HASH<var2>": {
                        "$const": "?"
                    }
                },
                "command": "aggregate",
                "pipeline": [
                    {
                        "$match": {
                            "$and": [
                                {
                                    "HASH<foo>": {
                                        "$in": [
                                            "?"
                                        ]
                                    }
                                },
                                {
                                    "HASH<bar>": {
                                        "$gte": {"$date":"1970-01-01T00:00:00.000Z"}
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
                                "$sum": {
                                    "$const": 1
                                }
                            }
                        }
                    },
                    {
                        "$limit": 1
                    },
                    {
                        "$out": {
                            "coll": "HASH<outColl>",
                            "db": "HASH<testDB>"
                        }
                    }
                ],
                "allowDiskUse": false
            },
            "comment": "?",
            "collectionType": "collection",
            "hint": {
                "HASH<z>": 1,
                "HASH<c>": 1
            },
            "maxTimeMS": 1,
            "bypassDocumentValidation": true,
            "cursor": {
                "batchSize": 1
            }
        })",
        shapified);
}

TEST_F(QueryStatsStoreTest, CorrectlyTokenizesAggregateCommandRequestEmptyFields) {
    auto expCtx = make_intrusive<ExpressionContextForTest>(*kDefaultTestNss.nss());
    AggregateCommandRequest acr(*kDefaultTestNss.nss());
    acr.setPipeline({});
    auto pipeline = Pipeline::parse({}, expCtx);

    auto shapified = makeQueryStatsKeyAggregateRequest(
        acr, *pipeline, expCtx, LiteralSerializationPolicy::kToDebugTypeString, true);
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

    // Test again with the representative query shape.
    shapified = makeQueryStatsKeyAggregateRequest(
        acr, *pipeline, expCtx, LiteralSerializationPolicy::kToRepresentativeParseableValue, true);
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
    auto expCtx = make_intrusive<ExpressionContextForTest>(*kDefaultTestNss.nss());
    auto nsToUnionWith = NamespaceString(expCtx->ns.db(), "otherColl");
    expCtx->addResolvedNamespaces({nsToUnionWith});

    AggregateCommandRequest acr(*kDefaultTestNss.nss());
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

    auto shapified = makeQueryStatsKeyAggregateRequest(
        acr, *pipeline, expCtx, LiteralSerializationPolicy::kToDebugTypeString, true);
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
                    "db": "HASH<testDB>",
                    "coll": "HASH<otherColl>"
                }
            ]
        })",
        shapified);

    // Do the same thing with the representative query shape.
    shapified = makeQueryStatsKeyAggregateRequest(
        acr, *pipeline, expCtx, LiteralSerializationPolicy::kToRepresentativeParseableValue, true);
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
                        "$unionWith": {
                            "coll": "HASH<otherColl>",
                            "pipeline": [
                                {
                                    "$match": {
                                        "HASH<val>": {
                                            "$eq": "?"
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
                    "db": "HASH<testDB>",
                    "coll": "HASH<otherColl>"
                }
            ]
        })",
        shapified);
}

BSONObj toBSON(AggregatedMetric am) {
    BSONObjBuilder builder;
    am.appendTo(builder, "m");
    return builder.obj();
}

TEST_F(QueryStatsStoreTest, SumOfSquaresOverflowTest) {
    // Ensure sumOfSquares is initialized correctly.
    AggregatedMetric aggMetric;
    auto res = toBSON(aggMetric).getObjectField("m").getField("sumOfSquares").Decimal();

    ASSERT_EQ(res, Decimal128());

    // Aggregating with the maximum int value does not overflow the sumOfSquares field.
    auto maxVal = std::numeric_limits<uint64_t>::max();
    aggMetric.aggregate(maxVal);
    res = toBSON(aggMetric).getObjectField("m").getField("sumOfSquares").Decimal();

    ASSERT_EQ(res, Decimal128(maxVal).power(Decimal128(2.0)));
}
}  // namespace mongo::query_stats
