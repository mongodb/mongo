// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/express/express_plan.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/db/exec/mutable_bson/element.h"
#include "mongo/db/index/index_constants.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_parser.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_options_gen.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <string_view>
#include <utility>
#include <variant>

namespace mongo::express {
namespace {
using namespace std::literals::string_view_literals;
class ExpressPlanTest : public CatalogTestFixture {
public:
    CollectionAcquisition createAndPopulateTestCollectionWithOptions(
        CollectionOptions options,
        std::vector<BSONObj> indexSpecList,
        std::vector<BSONObj> documentList) {
        auto nss = NamespaceString::createNamespaceString_forTest("ExpressPlanTest.TestCollection");
        ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, options));

        CollectionAcquisition collection = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest(nss,
                                         PlacementConcern(boost::none, boost::none),
                                         repl::ReadConcernArgs::get(operationContext()),
                                         AcquisitionPrerequisites::kRead),
            MODE_X);
        ASSERT(collection.exists());

        for (const auto& indexSpec : indexSpecList) {
            WriteUnitOfWork wuow(operationContext());
            CollectionWriter writer{operationContext(), &collection};

            auto writeableCollection = writer.getWritableCollection(operationContext());
            auto* indexCatalog = writeableCollection->getIndexCatalog();
            ASSERT_OK(indexCatalog->createIndexOnEmptyCollection(
                operationContext(), writeableCollection, indexSpec));
            wuow.commit();
        }

        {
            WriteUnitOfWork wuow(operationContext());
            ASSERT_OK(
                Helpers::insert(operationContext(), collection.getCollectionPtr(), documentList));
            wuow.commit();
        }

        auto isIdIndexExpected =
            !bool(options.clusteredIndex) && options.autoIndexId != CollectionOptions::NO;
        int expectedNumIndexes = (isIdIndexExpected ? 1 : 0) + indexSpecList.size();
        ASSERT_EQ(collection.getCollectionPtr()->getIndexCatalog()->numIndexesTotal(),
                  expectedNumIndexes);

        return collection;
    }

    // std::convertible_to is not yet universally supported.
    CollectionAcquisition createAndPopulateTestCollection(
        /* std::convertible_to<std::string_view> */ auto... documents) {
        return createAndPopulateTestCollectionWithOptions(
            CollectionOptions(), {}, {fromjson(documents)...});
    }

    CollectionAcquisition createAndPopulateTestClusteredCollection(
        /* std::convertible_to<std::string_view> */ auto... documents) {
        bool unique = true;
        bool legacyFormat = false;
        return createAndPopulateTestCollectionWithOptions(
            CollectionOptions{.clusteredIndex = ClusteredCollectionInfo(
                                  ClusteredIndexSpec(fromjson("{_id: 1}"), unique), legacyFormat)},
            {},
            {fromjson(documents)...});
    }

    CollectionAcquisition createAndPopulateTestCollectionWithIndex(
        BSONObj indexSpec, /* std::convertible_to<std::string_view> */ auto... documents) {
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
    auto result = iterator.consumeOne(opCtx,
                                      [&](const CollectionAcquisition,
                                          RecordId,
                                          Snapshotted<BSONObj> obj,
                                          const SeekableRecordCursor*) {
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
        opCtx,
        [](const CollectionAcquisition, RecordId, Snapshotted<BSONObj>, const SeekableRecordCursor*)
            -> PlanProgress { MONGO_UNREACHABLE; });
}

TEST_F(ExpressPlanTest, TestIdLookupViaIndexWithMatchingQuery) {
    auto collection =
        createAndPopulateTestCollection("{_id: 0, a: 2}"sv, "{_id: 1, a: 3}"sv, "{_id: 2, a: 5}"sv);

    IteratorStats iteratorStats;
    IdLookupViaIndex iterator(fromjson("{_id: 2}"));
    iterator.open(operationContext(), collection, /*forWrite=*/false, &iteratorStats);

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
    ASSERT_BSONOBJ_EQ(iteratorStats.indexKeyPattern(), BSON("_id" << 1));
}

TEST_F(ExpressPlanTest,
       TestIdLookupViaIndexHandsContinuationNonOwningBsonAndLiveCursorWhenReusingCursor) {
    // On the cursor-reuse path (default WiredTiger), 'IdLookupViaIndex::consumeOne' is expected to
    // skip the defensive 'record->data.makeOwned()' copy and instead pass a non-owning 'BSONObj'
    // view together with the still-alive cursor pointer to its continuation. This avoids paying a
    // full-record memcpy for every fetched document
    auto& provider =
        rss::ReplicatedStorageService::get(operationContext()).getPersistenceProvider();
    ASSERT(provider.supportsCursorReuseForExpressPathQueries());

    unittest::ServerParameterGuard reuseGuard("internalQueryReuseCursorForExpressPathUpdates",
                                              true);

    auto collection =
        createAndPopulateTestCollection("{_id: 0, a: 2}"sv, "{_id: 1, a: 3}"sv, "{_id: 2, a: 5}"sv);

    IteratorStats iteratorStats;
    IdLookupViaIndex iterator(fromjson("{_id: 2}"));
    iterator.open(operationContext(), collection, /*forWrite=*/false, &iteratorStats);

    boost::optional<bool> seenIsOwned;
    bool seenCursor = false;
    auto result = iterator.consumeOne(operationContext(),
                                      [&](const CollectionAcquisition,
                                          RecordId,
                                          Snapshotted<BSONObj> obj,
                                          const SeekableRecordCursor* cursor) -> PlanProgress {
                                          seenIsOwned = obj.value().isOwned();
                                          seenCursor = (cursor != nullptr);
                                          return Ready();
                                      });
    ASSERT(std::holds_alternative<Exhausted>(result));
    ASSERT(seenIsOwned.has_value());
    ASSERT_FALSE(*seenIsOwned)
        << "Expected the BSONObj passed to the continuation to be a non-owning view";
    ASSERT_TRUE(seenCursor)
        << "Expected the cursor pointer passed to the continuation to be non-null";
}

TEST_F(ExpressPlanTest, TestIdLookupViaIndexBsonRemainsValidAndOwnableAfterConsumeOneReturns) {
    // The executor reads (and may 'makeOwned') the BSONObj produced by the iterator after
    // 'consumeOne' returns.
    auto collection =
        createAndPopulateTestCollection("{_id: 0, a: 2}"sv, "{_id: 1, a: 3}"sv, "{_id: 2, a: 5}"sv);

    IteratorStats iteratorStats;
    IdLookupViaIndex iterator(fromjson("{_id: 2}"));
    iterator.open(operationContext(), collection, /*forWrite=*/false, &iteratorStats);

    boost::optional<BSONObj> captured;
    auto result = iterator.consumeOne(operationContext(),
                                      [&](const CollectionAcquisition,
                                          RecordId,
                                          Snapshotted<BSONObj> obj,
                                          const SeekableRecordCursor*) -> PlanProgress {
                                          captured.emplace(std::move(obj.value()));
                                          return Ready();
                                      });
    ASSERT(std::holds_alternative<Exhausted>(result));
    ASSERT(captured.has_value());

    // Read after 'consumeOne' has returned. Safe because the iterator's cursor is still alive.
    ASSERT_BSONOBJ_EQ(*captured, fromjson("{_id: 2, a: 5}"));

    // Take ownership and read again -- mirrors what 'PlanExecutorExpress::getNext' does after its
    // scoped timer has ended on callers that request owned BSON.
    captured->makeOwned();
    ASSERT_TRUE(captured->isOwned());
    ASSERT_BSONOBJ_EQ(*captured, fromjson("{_id: 2, a: 5}"));
}

TEST_F(ExpressPlanTest, TestIdLookupViaIndexWithNonMatchingQuery) {
    auto collection =
        createAndPopulateTestCollection("{_id: 0, a: 2}"sv, "{_id: 1, a: 3}"sv, "{_id: 2, a: 5}"sv);

    IteratorStats iteratorStats;
    IdLookupViaIndex iterator(fromjson("{_id: 4}"));
    iterator.open(operationContext(), collection, /*forWrite=*/false, &iteratorStats);

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
    ASSERT_BSONOBJ_EQ(iteratorStats.indexKeyPattern(), BSON("_id" << 1));
}

TEST_F(ExpressPlanTest, TestIdLookupOnClusteredCollectionWithMatchingQuery) {
    auto collection = createAndPopulateTestClusteredCollection(
        "{_id: 0, a: 2}"sv, "{_id: 1, a: 3}"sv, "{_id: 2, a: 5}"sv);

    IteratorStats iteratorStats;
    IdLookupOnClusteredCollection iterator(fromjson("{_id: 2}"));
    iterator.open(operationContext(), collection, /*forWrite=*/false, &iteratorStats);

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
    ASSERT(iteratorStats.indexKeyPattern().isEmpty());
}

TEST_F(ExpressPlanTest, TestIdLookupOnClusteredCollectionWithNonMatchingQuery) {
    auto collection = createAndPopulateTestClusteredCollection(
        "{_id: 0, a: 2}"sv, "{_id: 1, a: 3}"sv, "{_id: 2, a: 5}"sv);

    IteratorStats iteratorStats;
    IdLookupOnClusteredCollection iterator(fromjson("{_id: 4}"));
    iterator.open(operationContext(), collection, /*forWrite=*/false, &iteratorStats);

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
    ASSERT(iteratorStats.indexKeyPattern().isEmpty());
}

TEST_F(ExpressPlanTest, TestLookupViaUserIndexWithMatchingQuery) {
    std::string_view indexName = "a_1"sv;
    auto indexSpec = BSON("v" << 2 << "name" << indexName << "key" << BSON("a" << 1));
    auto collection = createAndPopulateTestCollectionWithIndex(
        indexSpec, "{_id: 0, a: 2}"sv, "{_id: 1, a: 3}"sv, "{_id: 2, a: 5}"sv);
    const CollectionPtr& collectionPtr = collection.getCollectionPtr();

    auto indexEntry =
        collectionPtr->getIndexCatalog()->findIndexByName(operationContext(), indexName);

    IteratorStats iteratorStats;
    auto filter = fromjson("{a: 5}");
    CollatorInterface* collator = nullptr;
    LookupViaUserIndex<FetchFromCollectionCallback> iterator(
        filter.firstElement(), indexEntry->getIdent(), std::string{indexName}, collator, nullptr);
    iterator.open(operationContext(), collection, /*forWrite=*/false, &iteratorStats);

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
    ASSERT_BSONOBJ_EQ(iteratorStats.indexKeyPattern(), BSON("a" << 1));
}

TEST_F(ExpressPlanTest, TestLookupViaUserIndexWithMatchingQueryUsingCollator) {
    std::string_view indexName = "a_1"sv;
    auto collationSpec = BSON("locale" << "en_US"
                                       << "strength" << 2);
    auto indexSpec = BSON("v" << 2 << "name" << indexName << "key" << BSON("a" << 1) << "collation"
                              << collationSpec);
    auto collection = createAndPopulateTestCollectionWithIndex(
        indexSpec, "{_id: 0, a: 'II'}"sv, "{_id: 1, a: 'III'}"sv, "{_id: 2, a: 'V'}"sv);
    const CollectionPtr& collectionPtr = collection.getCollectionPtr();

    auto indexEntry =
        collectionPtr->getIndexCatalog()->findIndexByName(operationContext(), indexName);
    auto collator = indexEntry->getCollator();

    IteratorStats iteratorStats;
    auto filter = fromjson("{a: 'iii'}");
    LookupViaUserIndex<FetchFromCollectionCallback> iterator(
        filter.firstElement(), indexEntry->getIdent(), std::string{indexName}, collator, nullptr);
    iterator.open(operationContext(), collection, /*forWrite=*/false, &iteratorStats);

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
    ASSERT_BSONOBJ_EQ(iteratorStats.indexKeyPattern(), BSON("a" << 1));
}

TEST_F(ExpressPlanTest, TestLookupViaUserIndexWWithNonMatchingQuery) {
    std::string_view indexName = "a_1"sv;
    auto indexSpec = BSON("v" << 2 << "name" << indexName << "key" << BSON("a" << 1));
    auto collection = createAndPopulateTestCollectionWithIndex(
        indexSpec, "{_id: 0, a: 2}"sv, "{_id: 1, a: 3}"sv, "{_id: 2, a: 5}"sv);
    const CollectionPtr& collectionPtr = collection.getCollectionPtr();

    auto indexEntry =
        collectionPtr->getIndexCatalog()->findIndexByName(operationContext(), indexName);

    IteratorStats iteratorStats;
    auto filter = fromjson("{a: 7}");
    CollatorInterface* collator = nullptr;
    LookupViaUserIndex<FetchFromCollectionCallback> iterator(
        filter.firstElement(), indexEntry->getIdent(), std::string{indexName}, collator, nullptr);
    iterator.open(operationContext(), collection, /*forWrite=*/false, &iteratorStats);

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
    ASSERT_BSONOBJ_EQ(iteratorStats.indexKeyPattern(), BSON("a" << 1));
}

projection_ast::Projection parseProjection(OperationContext* opCtx, BSONObj projection) {
    return projection_ast::parseAndAnalyze(new ExpressionContextForTest(opCtx),
                                           projection,
                                           ProjectionPolicies::findProjectionPolicies());
}

TEST_F(ExpressPlanTest, TestLookupViaUserIndexWithCoveredProjection) {
    std::string_view indexName = "a_1_b_1_c_1"sv;
    auto indexSpec =
        BSON("v" << 2 << "name" << indexName << "key" << BSON("a" << 1 << "b" << 1 << "c" << 1));
    auto collection = createAndPopulateTestCollectionWithIndex(indexSpec,
                                                               "{_id: 0, a: 2, b: 3, c: 4}"sv,
                                                               "{_id: 1, a: 5, b: 6, c: 7}"sv,
                                                               "{_id: 2, a: 8, b: 9, c: 10}"sv);
    const CollectionPtr& collectionPtr = collection.getCollectionPtr();

    auto indexEntry =
        collectionPtr->getIndexCatalog()->findIndexByName(operationContext(), indexName);

    IteratorStats iteratorStats;
    auto filter = fromjson("{a: 5}");
    CollatorInterface* collator = nullptr;

    auto projection = parseProjection(operationContext(), fromjson("{_id: 0, a: 1, c: 1}"));

    LookupViaUserIndex<CreateDocumentFromIndexKey> iterator(filter.firstElement(),
                                                            indexEntry->getIdent(),
                                                            std::string{indexName},
                                                            collator,
                                                            &projection);
    iterator.open(operationContext(), collection, /*forWrite=*/false, &iteratorStats);

    // The first call to 'consumeOne()' should provide a document and return 'Exhausted' to indicate
    // that it will be the last document.
    auto [result, obj] = iterateAndExpectDocument(operationContext(), iterator);
    ASSERT(std::holds_alternative<Exhausted>(result));
    ASSERT_BSONOBJ_EQ(obj, fromjson("{a: 5, c: 7}"));

    // Additional calls to 'consumeOne()' should just return an 'Exhausted' result.
    result = iterateButExpectNoDocument(operationContext(), iterator);
    ASSERT(std::holds_alternative<Exhausted>(result));

    ASSERT_EQ(iteratorStats.stageName(), "EXPRESS_IXSCAN");
    ASSERT_EQ(iteratorStats.numKeysExamined(), 1);
    ASSERT_EQ(iteratorStats.numDocumentsFetched(), 0);
    ASSERT_EQ(iteratorStats.indexName(), "a_1_b_1_c_1");
    ASSERT_BSONOBJ_EQ(iteratorStats.indexKeyPattern(), BSON("a" << 1 << "b" << 1 << "c" << 1));
    ASSERT_EQ(iteratorStats.projectionCovered(), true);
}

TEST_F(ExpressPlanTest, AssertFetchedRecordIsValidBsonAcceptsWellFormedDocument) {
    auto nss = NamespaceString::createNamespaceString_forTest("test.coll");
    BSONObj obj = fromjson(R"({_id: 1, a: "hello", b: [1, 2, 3]})");
    ASSERT_DOES_NOT_THROW(
        assertFetchedRecordIsValidBson(obj.objdata(), obj.objsize(), nss, RecordId(1)));
}

TEST_F(ExpressPlanTest, AssertFetchedRecordIsValidBsonRejectsTruncatedRecord) {
    // Simulate a torn or truncated page read (as can happen when a record is materialized from
    // disaggregated storage): the buffer is shorter than the document's own embedded objsize, so
    // an element overruns the buffer. This is the corruption that, left undetected, triggered an
    // opaque mutablebson invariant deep inside the update machinery.
    auto nss = NamespaceString::createNamespaceString_forTest("test.coll");
    BSONObj obj = fromjson(R"({_id: 1, a: "hello"})");
    ASSERT_THROWS_CODE(
        assertFetchedRecordIsValidBson(obj.objdata(), obj.objsize() - 4, nss, RecordId(2)),
        DBException,
        ErrorCodes::InvalidBSON);
}

TEST_F(ExpressPlanTest, AssertFetchedRecordIsValidBsonRejectsElementOverrunningObjsize) {
    // Construct a record whose string element claims a length that extends past the document's own
    // objsize -- exactly the inconsistency (offset + element size > objsize) that fired the
    // invariant in mutablebson's getElementOffset().
    auto nss = NamespaceString::createNamespaceString_forTest("test.coll");
    BSONObj obj = BSON("a" << "hello");

    auto buffer = std::make_unique<char[]>(obj.objsize());
    memcpy(buffer.get(), obj.objdata(), obj.objsize());

    // 'BSONElement::value()' points at the 4-byte little-endian length prefix of the string value.
    const auto valueOffset = obj["a"].value() - obj.objdata();
    const int32_t corruptLength = 1 << 20;
    memcpy(buffer.get() + valueOffset, &corruptLength, sizeof(corruptLength));

    ASSERT_THROWS_CODE(
        assertFetchedRecordIsValidBson(buffer.get(), obj.objsize(), nss, RecordId(3)),
        DBException,
        ErrorCodes::InvalidBSON);
}

DEATH_TEST_REGEX(ExpressUpdateMalformedRecord,
                 MalformedFetchedRecordTripsUpdateInPlaceInvariant,
                 R"(offset \+ elt\.size)") {
    // Deterministic repro of the crash: a record whose string element length overruns objsize (as
    // from a torn disagg page read) trips the getElementOffset() invariant once the update
    // machinery builds an in-place mutablebson::Document over it. The boundary validation added in
    // this change (see the AssertFetchedRecordIsValidBson* tests) rejects it before this point.
    BSONObj obj = BSON("a" << "hello");

    auto buffer = std::make_unique<char[]>(obj.objsize());
    memcpy(buffer.get(), obj.objdata(), obj.objsize());

    const auto valueOffset = obj["a"].value() - obj.objdata();
    const int32_t corruptLength = 1 << 20;
    memcpy(buffer.get() + valueOffset, &corruptLength, sizeof(corruptLength));
    BSONObj corrupt(buffer.get());

    // Mirror update::transformDocument(): build the in-place Document and walk children to force
    // lazy materialization and the offset computation.
    mutablebson::Document doc(corrupt, mutablebson::Document::kInPlaceEnabled);
    for (auto child = doc.root().leftChild(); child.ok(); child = child.rightSibling()) {
        (void)child.getFieldName();
    }
}

}  // namespace
}  // namespace mongo::express
