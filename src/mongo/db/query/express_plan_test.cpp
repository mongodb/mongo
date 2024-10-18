/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include <utility>
#include <variant>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/clustered_collection_options_gen.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/express/express_plan.h"
#include "mongo/db/index/index_constants.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"

namespace mongo::express {
namespace {
class ExpressPlanTest : public CatalogTestFixture {
public:
    AutoGetCollection createAndPopulateTestCollectionWithOptions(
        CollectionOptions options,
        std::vector<BSONObj> indexSpecList,
        std::vector<BSONObj> documentList) {
        auto nss = NamespaceString::createNamespaceString_forTest("ExpressPlanTest.TestCollection");
        ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, options));

        AutoGetCollection collection(operationContext(), nss, MODE_X);
        ASSERT(collection);

        for (const auto& indexSpec : indexSpecList) {
            WriteUnitOfWork wuow(operationContext());
            auto writeableCollection = collection.getWritableCollection(operationContext());
            auto* indexCatalog = writeableCollection->getIndexCatalog();
            ASSERT_OK(indexCatalog->createIndexOnEmptyCollection(
                operationContext(), writeableCollection, indexSpec));
            wuow.commit();
        }

        OpDebug* const nullOpDebug = nullptr;
        for (const auto& document : documentList) {
            WriteUnitOfWork wuow(operationContext());
            ASSERT_OK(collection_internal::insertDocument(
                operationContext(), *collection, InsertStatement(document), nullOpDebug));
            wuow.commit();
        }

        auto isIdIndexExpected =
            !bool(options.clusteredIndex) && options.autoIndexId != CollectionOptions::NO;
        int expectedNumIndexes = (isIdIndexExpected ? 1 : 0) + indexSpecList.size();
        ASSERT_EQ(collection->getIndexCatalog()->numIndexesTotal(), expectedNumIndexes);

        return collection;
    }

    // std::convertible_to is not yet universally supported.
    AutoGetCollection createAndPopulateTestCollection(
        /* std::convertible_to<StringData> */ auto... documents) {
        return createAndPopulateTestCollectionWithOptions(
            CollectionOptions(), {}, {fromjson(documents)...});
    }

    AutoGetCollection createAndPopulateTestClusteredCollection(
        /* std::convertible_to<StringData> */ auto... documents) {
        bool unique = true;
        bool legacyFormat = false;
        return createAndPopulateTestCollectionWithOptions(
            CollectionOptions{.clusteredIndex = ClusteredCollectionInfo(
                                  ClusteredIndexSpec(fromjson("{_id: 1}"), unique), legacyFormat)},
            {},
            {fromjson(documents)...});
    }

    AutoGetCollection createAndPopulateTestCollectionWithIndex(
        BSONObj indexSpec, /* std::convertible_to<StringData> */ auto... documents) {
        return createAndPopulateTestCollectionWithOptions(
            CollectionOptions(), {std::move(indexSpec)}, {fromjson(documents)...});
    }
};

// Call 'consumeOne()' on 'iterator' with a continuation that verifies it is called exactly once and
// always returns 'continuationReturnValue' as its result. Return a pair with the return value of
// 'consumeOne()' and the BSONObj given to the continuation.
static std::pair<PlanProgress, BSONObj> iterateAndExpectDocument(
    OperationContext* opCtx, auto& iterator, PlanProgress continuationReturnValue = Ready()) {
    boost::optional<BSONObj> producedObj;
    auto result =
        iterator.consumeOne(opCtx, [&](const CollectionPtr*, RecordId, Snapshotted<BSONObj> obj) {
            ASSERT(!bool(producedObj));
            producedObj.emplace(std::move(obj.value()));
            return std::move(continuationReturnValue);
        });
    ASSERT(bool(producedObj));
    return {std::move(result), std::move(*producedObj)};
}

// Call 'consumeOne()' on 'iterator' and verify that it does not call its continuation (i.e., does
// not produce an output document).
static PlanProgress iterateButExpectNoDocument(OperationContext* opCtx, auto& iterator) {
    return iterator.consumeOne(
        opCtx, [](const CollectionPtr*, RecordId, Snapshotted<BSONObj>) -> PlanProgress {
            MONGO_UNREACHABLE;
        });
}

TEST_F(ExpressPlanTest, TestIdLookupViaIndexWithMatchingQuery) {
    auto collection = createAndPopulateTestCollection(
        "{_id: 0, a: 2}"_sd, "{_id: 1, a: 3}"_sd, "{_id: 2, a: 5}"_sd);
    const CollectionPtr& collectionPtr = *collection;

    IteratorStats iteratorStats;
    IdLookupViaIndex<const CollectionPtr*> iterator(fromjson("{_id: 2}"));
    iterator.open(operationContext(), &collectionPtr, &iteratorStats);

    // The first call to 'consumeOne()' should provide a document and return 'Exhausted' to indicate
    // that it will be the last document.
    auto [result, obj] = iterateAndExpectDocument(operationContext(), iterator);
    ASSERT(std::holds_alternative<Exhausted>(result));
    ASSERT_BSONOBJ_EQ(obj, fromjson("{_id: 2, a: 5}"));

    // Additional calls to 'consumeOne()' should just return an 'Exhausted' result.
    result = iterateButExpectNoDocument(operationContext(), iterator);
    ASSERT(std::holds_alternative<Exhausted>(result));

    ASSERT_EQ(iteratorStats.stageName(), "EXPRESS_IXSCAN");
    ASSERT_EQ(iteratorStats.numKeysExamined(), 1);
    ASSERT_EQ(iteratorStats.numDocumentsFetched(), 1);
    ASSERT_EQ(iteratorStats.indexName(), IndexConstants::kIdIndexName);
    ASSERT_EQ(iteratorStats.indexKeyPattern(), "{ _id: 1 }");
}

TEST_F(ExpressPlanTest, TestIdLookupViaIndexWithNonMatchingQuery) {
    auto collection = createAndPopulateTestCollection(
        "{_id: 0, a: 2}"_sd, "{_id: 1, a: 3}"_sd, "{_id: 2, a: 5}"_sd);
    const CollectionPtr& collectionPtr = *collection;

    IteratorStats iteratorStats;
    IdLookupViaIndex<const CollectionPtr*> iterator(fromjson("{_id: 4}"));
    iterator.open(operationContext(), &collectionPtr, &iteratorStats);

    // Any number of repeated calls to 'consumeOne()' should return an 'Exhausted' result without
    // producing any documents.
    for (size_t i = 0; i < 3; ++i) {
        auto result = iterateButExpectNoDocument(operationContext(), iterator);
        ASSERT(std::holds_alternative<Exhausted>(result));
    }

    ASSERT_EQ(iteratorStats.stageName(), "EXPRESS_IXSCAN");
    ASSERT_EQ(iteratorStats.numKeysExamined(), 0);
    ASSERT_EQ(iteratorStats.numDocumentsFetched(), 0);
    ASSERT_EQ(iteratorStats.indexName(), IndexConstants::kIdIndexName);
    ASSERT_EQ(iteratorStats.indexKeyPattern(), "{ _id: 1 }");
}

TEST_F(ExpressPlanTest, TestIdLookupOnClusteredCollectionWithMatchingQuery) {
    auto collection = createAndPopulateTestClusteredCollection(
        "{_id: 0, a: 2}"_sd, "{_id: 1, a: 3}"_sd, "{_id: 2, a: 5}"_sd);
    const CollectionPtr& collectionPtr = *collection;

    IteratorStats iteratorStats;
    IdLookupOnClusteredCollection<const CollectionPtr*> iterator(fromjson("{_id: 2}"));
    iterator.open(operationContext(), &collectionPtr, &iteratorStats);

    // The first call to 'consumeOne()' should provide a document and return 'Exhausted' to indicate
    // that it will be the last document.
    auto [result, obj] = iterateAndExpectDocument(operationContext(), iterator);
    ASSERT(std::holds_alternative<Exhausted>(result));
    ASSERT_BSONOBJ_EQ(obj, fromjson("{_id: 2, a: 5}"));

    // Additional calls to 'consumeOne()' should just return an 'Exhausted' result.
    result = iterateButExpectNoDocument(operationContext(), iterator);
    ASSERT(std::holds_alternative<Exhausted>(result));

    ASSERT_EQ(iteratorStats.stageName(), "EXPRESS_CLUSTERED_IXSCAN");
    ASSERT_EQ(iteratorStats.numKeysExamined(), 0);
    ASSERT_EQ(iteratorStats.numDocumentsFetched(), 1);
    ASSERT(iteratorStats.indexName().empty());
    ASSERT(iteratorStats.indexKeyPattern().empty());
}

TEST_F(ExpressPlanTest, TestIdLookupOnClusteredCollectionWithNonMatchingQuery) {
    auto collection = createAndPopulateTestClusteredCollection(
        "{_id: 0, a: 2}"_sd, "{_id: 1, a: 3}"_sd, "{_id: 2, a: 5}"_sd);
    const CollectionPtr& collectionPtr = *collection;

    IteratorStats iteratorStats;
    IdLookupOnClusteredCollection<const CollectionPtr*> iterator(fromjson("{_id: 4}"));
    iterator.open(operationContext(), &collectionPtr, &iteratorStats);

    // Any number of repeated calls to 'consumeOne()' should return an 'Exhausted' result without
    // producing any documents.
    for (size_t i = 0; i < 3; ++i) {
        auto result = iterateButExpectNoDocument(operationContext(), iterator);
        ASSERT(std::holds_alternative<Exhausted>(result));
    }

    ASSERT_EQ(iteratorStats.stageName(), "EXPRESS_CLUSTERED_IXSCAN");
    ASSERT_EQ(iteratorStats.numKeysExamined(), 0);
    ASSERT_EQ(iteratorStats.numDocumentsFetched(), 0);
    ASSERT(iteratorStats.indexName().empty());
    ASSERT(iteratorStats.indexKeyPattern().empty());
}

TEST_F(ExpressPlanTest, TestLookupViaUserIndexWithMatchingQuery) {
    StringData indexName = "a_1"_sd;
    auto indexSpec = BSON("v" << 2 << "name" << indexName << "key" << BSON("a" << 1));
    auto collection = createAndPopulateTestCollectionWithIndex(
        indexSpec, "{_id: 0, a: 2}"_sd, "{_id: 1, a: 3}"_sd, "{_id: 2, a: 5}"_sd);
    const CollectionPtr& collectionPtr = *collection;

    auto indexDescriptor =
        collectionPtr->getIndexCatalog()->findIndexByName(operationContext(), indexName);

    IteratorStats iteratorStats;
    auto filter = fromjson("{a: 5}");
    CollatorInterface* collator = nullptr;
    LookupViaUserIndex<const CollectionPtr*> iterator(filter.firstElement(),
                                                      indexDescriptor->getEntry()->getIdent(),
                                                      indexName.toString(),
                                                      collator);
    iterator.open(operationContext(), &collectionPtr, &iteratorStats);

    // The first call to 'consumeOne()' should provide a document and return 'Exhausted' to indicate
    // that it will be the last document.
    auto [result, obj] = iterateAndExpectDocument(operationContext(), iterator);
    ASSERT(std::holds_alternative<Exhausted>(result));
    ASSERT_BSONOBJ_EQ(obj, fromjson("{_id: 2, a: 5}"));

    // Additional calls to 'consumeOne()' should just return an 'Exhausted' result.
    result = iterateButExpectNoDocument(operationContext(), iterator);
    ASSERT(std::holds_alternative<Exhausted>(result));

    ASSERT_EQ(iteratorStats.stageName(), "EXPRESS_IXSCAN");
    ASSERT_EQ(iteratorStats.numKeysExamined(), 1);
    ASSERT_EQ(iteratorStats.numDocumentsFetched(), 1);
    ASSERT_EQ(iteratorStats.indexName(), "a_1");
    ASSERT_EQ(iteratorStats.indexKeyPattern(), "{ a: 1 }");
}

TEST_F(ExpressPlanTest, TestLookupViaUserIndexWithMatchingQueryUsingCollator) {
    StringData indexName = "a_1"_sd;
    auto collationSpec = BSON("locale"
                              << "en_US"
                              << "strength" << 2);
    auto indexSpec = BSON("v" << 2 << "name" << indexName << "key" << BSON("a" << 1) << "collation"
                              << collationSpec);
    auto collection = createAndPopulateTestCollectionWithIndex(
        indexSpec, "{_id: 0, a: 'II'}"_sd, "{_id: 1, a: 'III'}"_sd, "{_id: 2, a: 'V'}"_sd);
    const CollectionPtr& collectionPtr = *collection;

    auto indexDescriptor =
        collectionPtr->getIndexCatalog()->findIndexByName(operationContext(), indexName);
    auto collator = indexDescriptor->getEntry()->getCollator();

    IteratorStats iteratorStats;
    auto filter = fromjson("{a: 'iii'}");
    LookupViaUserIndex<const CollectionPtr*> iterator(filter.firstElement(),
                                                      indexDescriptor->getEntry()->getIdent(),
                                                      indexName.toString(),
                                                      collator);
    iterator.open(operationContext(), &collectionPtr, &iteratorStats);

    // The first call to 'consumeOne()' should provide a document and return 'Exhausted' to indicate
    // that it will be the last document.
    auto [result, obj] = iterateAndExpectDocument(operationContext(), iterator);
    ASSERT(std::holds_alternative<Exhausted>(result));
    ASSERT_BSONOBJ_EQ(obj, fromjson("{_id: 1, a: 'III'}"));

    // Additional calls to 'consumeOne()' should just return an 'Exhausted' result.
    result = iterateButExpectNoDocument(operationContext(), iterator);
    ASSERT(std::holds_alternative<Exhausted>(result));

    ASSERT_EQ(iteratorStats.stageName(), "EXPRESS_IXSCAN");
    ASSERT_EQ(iteratorStats.numKeysExamined(), 1);
    ASSERT_EQ(iteratorStats.numDocumentsFetched(), 1);
    ASSERT_EQ(iteratorStats.indexName(), "a_1");
    ASSERT_EQ(iteratorStats.indexKeyPattern(), "{ a: 1 }");
}

TEST_F(ExpressPlanTest, TestLookupViaUserIndexWWithNonMatchingQuery) {
    StringData indexName = "a_1"_sd;
    auto indexSpec = BSON("v" << 2 << "name" << indexName << "key" << BSON("a" << 1));
    auto collection = createAndPopulateTestCollectionWithIndex(
        indexSpec, "{_id: 0, a: 2}"_sd, "{_id: 1, a: 3}"_sd, "{_id: 2, a: 5}"_sd);
    const CollectionPtr& collectionPtr = *collection;

    auto indexDescriptor =
        collectionPtr->getIndexCatalog()->findIndexByName(operationContext(), indexName);

    IteratorStats iteratorStats;
    auto filter = fromjson("{a: 7}");
    CollatorInterface* collator = nullptr;
    LookupViaUserIndex<const CollectionPtr*> iterator(filter.firstElement(),
                                                      indexDescriptor->getEntry()->getIdent(),
                                                      indexName.toString(),
                                                      collator);
    iterator.open(operationContext(), &collectionPtr, &iteratorStats);

    // Any number of repeated calls to 'consumeOne()' should return an 'Exhausted' result without
    // producing any documents.
    for (size_t i = 0; i < 3; ++i) {
        auto result = iterateButExpectNoDocument(operationContext(), iterator);
        ASSERT(std::holds_alternative<Exhausted>(result));
    }

    ASSERT_EQ(iteratorStats.stageName(), "EXPRESS_IXSCAN");
    ASSERT_EQ(iteratorStats.numKeysExamined(), 0);
    ASSERT_EQ(iteratorStats.numDocumentsFetched(), 0);
    ASSERT_EQ(iteratorStats.indexName(), "a_1");
    ASSERT_EQ(iteratorStats.indexKeyPattern(), "{ a: 1 }");
}

TEST_F(ExpressPlanTest, TestIdLookupNullCollectionOnRestoreThrows) {
    auto collection = createAndPopulateTestCollection();
    const CollectionPtr& collectionPtr = *collection;
    IteratorStats iteratorStats;

    IdLookupViaIndex<const CollectionPtr*> iterator(fromjson("{_id: 2}"));
    iterator.open(operationContext(), &collectionPtr, &iteratorStats);

    auto nss = NamespaceString::createNamespaceString_forTest("ExpressPlanTest.TestCollection");
    const CollectionPtr nullCollection;

    iterator.releaseResources();
    ASSERT_THROWS(iterator.restoreResources(operationContext(), &nullCollection, nss),
                  ExceptionFor<ErrorCodes::QueryPlanKilled>);
}

TEST_F(ExpressPlanTest, TestLookupViaUserIndexNullCollectionOnRestoreThrows) {
    StringData indexName = "a_1"_sd;
    auto indexSpec = BSON("v" << 2 << "name" << indexName << "key" << BSON("a" << 1));
    auto collection = createAndPopulateTestCollectionWithIndex(indexSpec);
    const CollectionPtr& collectionPtr = *collection;
    IteratorStats iteratorStats;

    auto indexDescriptor =
        collectionPtr->getIndexCatalog()->findIndexByName(operationContext(), indexName);
    auto filter = fromjson("{a: 2}");
    CollatorInterface* collator = nullptr;
    LookupViaUserIndex<const CollectionPtr*> iterator(filter.firstElement(),
                                                      indexDescriptor->getEntry()->getIdent(),
                                                      indexName.toString(),
                                                      collator);
    iterator.open(operationContext(), &collectionPtr, &iteratorStats);
    auto nss = NamespaceString::createNamespaceString_forTest("ExpressPlanTest.TestCollection");
    const CollectionPtr nullCollection;

    iterator.releaseResources();
    ASSERT_THROWS(iterator.restoreResources(operationContext(), &nullCollection, nss),
                  ExceptionFor<ErrorCodes::QueryPlanKilled>);
}

TEST_F(ExpressPlanTest, TestLookupClusteredIdIndexNullCollectionOnRestoreThrows) {
    auto collection = createAndPopulateTestClusteredCollection();
    const CollectionPtr& collectionPtr = *collection;

    IteratorStats iteratorStats;
    IdLookupOnClusteredCollection<const CollectionPtr*> iterator(fromjson("{_id: 4}"));
    iterator.open(operationContext(), &collectionPtr, &iteratorStats);
    auto nss = NamespaceString::createNamespaceString_forTest("ExpressPlanTest.TestCollection");
    const CollectionPtr nullCollection;

    iterator.releaseResources();
    ASSERT_THROWS(iterator.restoreResources(operationContext(), &nullCollection, nss),
                  ExceptionFor<ErrorCodes::QueryPlanKilled>);
}

}  // namespace
}  // namespace mongo::express
