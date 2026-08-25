// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index/index_access_method.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/client.h"
#include "mongo/db/index/btree_access_method.h"
#include "mongo/db/index_builds/index_build_interceptor.h"
#include "mongo/db/index_builds/index_build_test_helpers.h"
#include "mongo/db/index_builds/index_builds_common.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/lazy_record_store.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/validate/validate_results.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/shared_buffer_fragment.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

namespace {
KeyStringSet makeKeyStringSet(std::initializer_list<BSONObj> objs) {
    KeyStringSet keyStrings;
    for (auto& obj : objs) {
        key_string::HeapBuilder keyString(
            key_string::Version::kLatestVersion, obj, Ordering::make(BSONObj()));
        keyStrings.insert(keyString.release());
    }
    return keyStrings;
}

TEST(IndexAccessMethodSetDifference, EmptyInputsShouldHaveNoDifference) {
    KeyStringSet left;
    KeyStringSet right;
    auto diff = SortedDataIndexAccessMethod::setDifference(left, right);
    EXPECT_EQ(0UL, diff.first.size());
    EXPECT_EQ(0UL, diff.second.size());
}

TEST(IndexAccessMethodSetDifference, EmptyLeftShouldHaveNoDifference) {
    KeyStringSet left;
    auto right = makeKeyStringSet({BSON("" << 0)});

    auto diff = SortedDataIndexAccessMethod::setDifference(left, right);
    EXPECT_EQ(0UL, diff.first.size());
    EXPECT_EQ(1UL, diff.second.size());
}

TEST(IndexAccessMethodSetDifference, EmptyRightShouldReturnAllOfLeft) {
    auto left = makeKeyStringSet({BSON("" << 0), BSON("" << 1)});
    KeyStringSet right;

    auto diff = SortedDataIndexAccessMethod::setDifference(left, right);
    EXPECT_EQ(2UL, diff.first.size());
    EXPECT_EQ(0UL, diff.second.size());
}

TEST(IndexAccessMethodSetDifference, IdenticalSetsShouldHaveNoDifference) {
    auto left = makeKeyStringSet({BSON("" << 0), BSON("" << "string"), BSON("" << BSONNULL)});
    auto right = makeKeyStringSet({BSON("" << 0), BSON("" << "string"), BSON("" << BSONNULL)});

    auto diff = SortedDataIndexAccessMethod::setDifference(left, right);
    EXPECT_EQ(0UL, diff.first.size());
    EXPECT_EQ(0UL, diff.second.size());
}

//
// Number type comparisons.
//

void assertDistinct(BSONObj left, BSONObj right) {
    auto leftSet = makeKeyStringSet({left});
    auto rightSet = makeKeyStringSet({right});
    auto diff = SortedDataIndexAccessMethod::setDifference(leftSet, rightSet);
    EXPECT_EQ(1UL, diff.first.size());
    EXPECT_EQ(1UL, diff.second.size());
}

TEST(IndexAccessMethodSetDifference, ZerosOfDifferentTypesAreNotEquivalent) {
    const BSONObj intObj = BSON("" << static_cast<int>(0));
    const BSONObj longObj = BSON("" << static_cast<long long>(0));
    const BSONObj doubleObj = BSON("" << static_cast<double>(0.0));

    // These should compare equal with woCompare(), but should not be treated equal by the index.
    EXPECT_EQ(0, intObj.woCompare(longObj));
    EXPECT_EQ(0, longObj.woCompare(doubleObj));

    assertDistinct(intObj, longObj);
    assertDistinct(intObj, doubleObj);

    assertDistinct(longObj, intObj);
    assertDistinct(longObj, doubleObj);

    assertDistinct(doubleObj, intObj);
    assertDistinct(doubleObj, longObj);

    const BSONObj decimalObj = fromjson("{'': NumberDecimal('0')}");

    EXPECT_EQ(0, doubleObj.woCompare(decimalObj));

    assertDistinct(intObj, decimalObj);
    assertDistinct(longObj, decimalObj);
    assertDistinct(doubleObj, decimalObj);

    assertDistinct(decimalObj, intObj);
    assertDistinct(decimalObj, longObj);
    assertDistinct(decimalObj, doubleObj);
}

TEST(IndexAccessMethodSetDifference, ShouldDetectOneDifferenceAmongManySimilarities) {
    auto left = makeKeyStringSet({BSON("" << 0),
                                  BSON("" << "string"),
                                  BSON("" << BSONNULL),
                                  BSON("" << static_cast<long long>(1)),  // This is different.
                                  BSON("" << BSON("sub" << "document")),
                                  BSON("" << BSON_ARRAY(1 << "hi" << 42))});
    auto right = makeKeyStringSet({BSON("" << 0),
                                   BSON("" << "string"),
                                   BSON("" << BSONNULL),
                                   BSON("" << static_cast<double>(1.0)),  // This is different.
                                   BSON("" << BSON("sub" << "document")),
                                   BSON("" << BSON_ARRAY(1 << "hi" << 42))});
    auto diff = SortedDataIndexAccessMethod::setDifference(left, right);
    EXPECT_EQ(1UL, diff.first.size());
    EXPECT_EQ(1UL, diff.second.size());
}

TEST(IndexAccessMethodSetDifference, SingleObjInLeftShouldFindCorrespondingObjInRight) {
    auto left = makeKeyStringSet({BSON("" << 2)});
    auto right = makeKeyStringSet({BSON("" << 1), BSON("" << 2), BSON("" << 3)});
    auto diff = SortedDataIndexAccessMethod::setDifference(left, right);
    EXPECT_EQ(0UL, diff.first.size());
    EXPECT_EQ(2UL, diff.second.size());
}

TEST(IndexAccessMethodSetDifference, SingleObjInRightShouldFindCorrespondingObjInLeft) {
    auto left = makeKeyStringSet({BSON("" << 1), BSON("" << 2), BSON("" << 3)});
    auto right = makeKeyStringSet({BSON("" << 2)});
    auto diff = SortedDataIndexAccessMethod::setDifference(left, right);
    EXPECT_EQ(2UL, diff.first.size());
    EXPECT_EQ(0UL, diff.second.size());
}

TEST(IndexAccessMethodSetDifference, LeftSetAllSmallerThanRightShouldBeDisjoint) {
    auto left = makeKeyStringSet({BSON("" << 1), BSON("" << 2), BSON("" << 3)});
    auto right = makeKeyStringSet({BSON("" << 4), BSON("" << 5), BSON("" << 6)});
    auto diff = SortedDataIndexAccessMethod::setDifference(left, right);
    EXPECT_EQ(3UL, diff.first.size());
    EXPECT_EQ(3UL, diff.second.size());
    for (auto&& obj : diff.first) {
        ASSERT(left.find(obj) != left.end());
    }
    for (auto&& obj : diff.second) {
        ASSERT(right.find(obj) != right.end());
    }
}

TEST(IndexAccessMethodSetDifference, LeftSetAllLargerThanRightShouldBeDisjoint) {
    auto left = makeKeyStringSet({BSON("" << 4), BSON("" << 5), BSON("" << 6)});
    auto right = makeKeyStringSet({BSON("" << 1), BSON("" << 2), BSON("" << 3)});
    auto diff = SortedDataIndexAccessMethod::setDifference(left, right);
    EXPECT_EQ(3UL, diff.first.size());
    EXPECT_EQ(3UL, diff.second.size());
    for (auto&& obj : diff.first) {
        ASSERT(left.find(obj) != left.end());
    }
    for (auto&& obj : diff.second) {
        ASSERT(right.find(obj) != right.end());
    }
}

TEST(IndexAccessMethodSetDifference, ShouldNotReportOverlapsFromNonDisjointSets) {
    auto left = makeKeyStringSet({BSON("" << 0), BSON("" << 1), BSON("" << 4), BSON("" << 6)});
    auto right = makeKeyStringSet(
        {BSON("" << -1), BSON("" << 1), BSON("" << 3), BSON("" << 4), BSON("" << 7)});
    auto diff = SortedDataIndexAccessMethod::setDifference(left, right);
    EXPECT_EQ(2UL, diff.first.size());   // 0, 6.
    EXPECT_EQ(3UL, diff.second.size());  // -1, 3, 7.
    for (auto&& keyString : diff.first) {
        ASSERT(left.find(keyString) != left.end());
        // Make sure it's not in the intersection.
        auto obj = key_string::toBson(keyString, Ordering::make(BSONObj()));
        ASSERT_BSONOBJ_NE(obj, BSON("" << 1));
        ASSERT_BSONOBJ_NE(obj, BSON("" << 4));
    }
    for (auto&& keyString : diff.second) {
        ASSERT(right.find(keyString) != right.end());
        // Make sure it's not in the intersection.
        auto obj = key_string::toBson(keyString, Ordering::make(BSONObj()));
        ASSERT_BSONOBJ_NE(obj, BSON("" << 1));
        ASSERT_BSONOBJ_NE(obj, BSON("" << 4));
    }
}

class IndexAccessMethodInsertKeys : public ServiceContextMongoDTest {};

TEST_F(IndexAccessMethodInsertKeys, DuplicatesCheckingOnSecondaryUniqueIndexes) {
    ServiceContext::UniqueOperationContext opCtxRaii = cc().makeOperationContext();
    OperationContext* opCtx = opCtxRaii.get();
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        "unittests.DuplicatesCheckingOnSecondaryUniqueIndexes");
    auto indexName = "a_1";
    auto indexSpec = BSON("name" << indexName << "key" << BSON("a" << 1) << "unique" << true << "v"
                                 << static_cast<int>(IndexDescriptor::IndexVersion::kV2));
    ASSERT_OK(createIndexFromSpec(opCtx, nss.ns_forTest(), indexSpec));

    auto acq = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kWrite),
        LockMode::MODE_X);
    const auto& coll = acq.getCollectionPtr();
    auto indexEntry = coll->getIndexCatalog()->findIndexByName(opCtx, indexName);
    auto indexAccessMethod = indexEntry->accessMethod()->asSortedData();

    key_string::HeapBuilder keyString1(
        key_string::Version::kLatestVersion, BSON("" << 1), Ordering::make(BSONObj()), RecordId(1));
    key_string::HeapBuilder keyString2(
        key_string::Version::kLatestVersion, BSON("" << 1), Ordering::make(BSONObj()), RecordId(2));
    KeyStringSet keys{keyString1.release(), keyString2.release()};
    struct InsertDeleteOptions options; /* options.dupsAllowed = false */
    int64_t numInserted;

    // Checks duplicates and returns the error code when constraints are enforced.
    const auto initDuplicateKeyErrors =
        SortedDataIndexAccessMethod::getDuplicateKeyErrors_forTest();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    auto status =
        indexAccessMethod->insertKeys(opCtx, ru, coll, indexEntry, keys, options, {}, &numInserted);
    EXPECT_EQ(status.code(), ErrorCodes::DuplicateKey);
    EXPECT_EQ(numInserted, 0);
    EXPECT_EQ(SortedDataIndexAccessMethod::getDuplicateKeyErrors_forTest(),
              initDuplicateKeyErrors + 1);

    // Skips the check on duplicates when constraints are not enforced.
    opCtx->setEnforceConstraints(false);
    ASSERT_OK(indexAccessMethod->insertKeys(
        opCtx, ru, coll, indexEntry, keys, options, {}, &numInserted));
    EXPECT_EQ(numInserted, 2);
    EXPECT_EQ(SortedDataIndexAccessMethod::getDuplicateKeyErrors_forTest(),
              initDuplicateKeyErrors + 1);
}

TEST_F(IndexAccessMethodInsertKeys, InsertWhenPrepareUnique) {
    ServiceContext::UniqueOperationContext opCtxRaii = cc().makeOperationContext();
    OperationContext* opCtx = opCtxRaii.get();
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("unittests.InsertWhenPrepareUnique");
    auto indexName = "a_1";
    auto indexSpec = BSON("name" << indexName << "key" << BSON("a" << 1) << "prepareUnique" << true
                                 << "v" << static_cast<int>(IndexDescriptor::IndexVersion::kV2));
    ASSERT_OK(createIndexFromSpec(opCtx, nss.ns_forTest(), indexSpec));

    auto acq = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kWrite),
        LockMode::MODE_X);
    const auto& coll = acq.getCollectionPtr();
    auto indexEntry = coll->getIndexCatalog()->findIndexByName(opCtx, indexName);
    auto indexAccessMethod = indexEntry->accessMethod()->asSortedData();

    key_string::HeapBuilder keyString1(
        key_string::Version::kLatestVersion, BSON("" << 1), Ordering::make(BSONObj()), RecordId(1));
    key_string::HeapBuilder keyString2(
        key_string::Version::kLatestVersion, BSON("" << 1), Ordering::make(BSONObj()), RecordId(2));
    KeyStringSet keys{keyString1.release(), keyString2.release()};
    struct InsertDeleteOptions options;
    int64_t numInserted;
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);

    const auto initDuplicateKeyErrors =
        SortedDataIndexAccessMethod::getDuplicateKeyErrors_forTest();
    // Disallows new duplicates in a regular index and rejects the insert.
    auto status =
        indexAccessMethod->insertKeys(opCtx, ru, coll, indexEntry, keys, options, {}, &numInserted);
    EXPECT_EQ(status.code(), ErrorCodes::DuplicateKey);
    EXPECT_EQ(numInserted, 0);
    EXPECT_EQ(SortedDataIndexAccessMethod::getDuplicateKeyErrors_forTest(),
              initDuplicateKeyErrors + 1);
}

class IndexAccessMethodUpdateKeys : public ServiceContextMongoDTest {};
TEST_F(IndexAccessMethodUpdateKeys, UpdateWhenPrepareUnique) {
    ServiceContext::UniqueOperationContext opCtxRaii = cc().makeOperationContext();
    OperationContext* opCtx = opCtxRaii.get();
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("unittests.UpdateWhenPrepareUnique");
    auto indexName = "a_1";
    auto indexSpec = BSON("name" << indexName << "key" << BSON("a" << 1) << "prepareUnique" << true
                                 << "v" << static_cast<int>(IndexDescriptor::IndexVersion::kV2));
    ASSERT_OK(createIndexFromSpec(opCtx, nss.ns_forTest(), indexSpec));

    auto acq = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kWrite),
        LockMode::MODE_X);
    const auto& coll = acq.getCollectionPtr();
    auto indexEntry = coll->getIndexCatalog()->findIndexByName(opCtx, indexName);
    auto indexAccessMethod = indexEntry->accessMethod()->asSortedData();

    key_string::HeapBuilder keyString1(
        key_string::Version::kLatestVersion, BSON("" << 1), Ordering::make(BSONObj()), RecordId(1));
    key_string::HeapBuilder keyString2_old(
        key_string::Version::kLatestVersion, BSON("" << 2), Ordering::make(BSONObj()), RecordId(2));
    key_string::HeapBuilder keyString2_new(
        key_string::Version::kLatestVersion, BSON("" << 1), Ordering::make(BSONObj()), RecordId(2));
    KeyStringSet key1{keyString1.release()};
    KeyStringSet key2_old{keyString2_old.release()};
    KeyStringSet key2_new{keyString2_new.release()};
    struct InsertDeleteOptions options;
    UpdateTicket ticket{true, {}, {}, {}, key2_old, key2_new, RecordId(2), true, {}};
    int64_t numInserted;
    int64_t numDeleted;
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);

    const auto initDuplicateKeyErrors =
        SortedDataIndexAccessMethod::getDuplicateKeyErrors_forTest();
    // Inserts two keys.
    ASSERT_OK(indexAccessMethod->insertKeys(
        opCtx, ru, coll, indexEntry, key1, options, {}, &numInserted));
    EXPECT_EQ(numInserted, 1);
    ASSERT_OK(indexAccessMethod->insertKeys(
        opCtx, ru, coll, indexEntry, key2_old, options, {}, &numInserted));
    EXPECT_EQ(numInserted, 1);
    EXPECT_EQ(SortedDataIndexAccessMethod::getDuplicateKeyErrors_forTest(), initDuplicateKeyErrors);

    // Disallows new duplicates in a regular index and rejects the update.
    auto status =
        indexAccessMethod->doUpdate(opCtx, ru, coll, indexEntry, ticket, &numInserted, &numDeleted);
    EXPECT_EQ(status.code(), ErrorCodes::DuplicateKey);
    EXPECT_EQ(numInserted, 0);
    EXPECT_EQ(numDeleted, 0);
    EXPECT_EQ(SortedDataIndexAccessMethod::getDuplicateKeyErrors_forTest(),
              initDuplicateKeyErrors + 1);
}

class IndexAccessMethodBulkBuilder : public ServiceContextMongoDTest {};

TEST_F(IndexAccessMethodBulkBuilder, CommitRejectsZeroInterval) {
    ServiceContext::UniqueOperationContext opCtxRaii = cc().makeOperationContext();
    OperationContext* opCtx = opCtxRaii.get();
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("unittests.CommitRejectsZeroInterval");
    auto indexName = "a_1";
    auto indexSpec = BSON("name" << indexName << "key" << BSON("a" << 1) << "v"
                                 << static_cast<int>(IndexDescriptor::IndexVersion::kV2));
    ASSERT_OK(createIndexFromSpec(opCtx, nss.ns_forTest(), indexSpec));

    auto acq = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kWrite),
        LockMode::MODE_X);
    const auto& coll = acq.getCollectionPtr();
    auto indexEntry = coll->getIndexCatalog()->findIndexByName(opCtx, indexName);
    auto indexAccessMethod = indexEntry->accessMethod()->asSortedData();

    auto bulk = indexAccessMethod->initiateBulk(opCtx,
                                                coll,
                                                indexEntry,
                                                /*spiller=*/nullptr,
                                                /*maxMemoryUsageBytes=*/128 * 1024 * 1024,
                                                /*stateInfo=*/boost::none,
                                                nss.dbName(),
                                                ContainerWriteBehavior::kDoNotReplicate);
    ASSERT(bulk);

    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    ASSERT_THROWS_CODE(bulk->commit(opCtx,
                                    ru,
                                    &coll,
                                    indexEntry,
                                    /*dupsAllowed=*/true,
                                    /*yieldIterations=*/0,
                                    IndexAccessMethod::KeyHandlerFn{
                                        [](const CollectionPtr&, const key_string::View&) {
                                            return Status::OK();
                                        }},
                                    IndexAccessMethod::RecordIdHandlerFn{},
                                    IndexAccessMethod::YieldFn{},
                                    IndexAccessMethod::OnNKeysLoadedFn{[]() {
                                    }},
                                    IndexAccessMethod::OnBytesWrittenFn{[](int64_t) {
                                    }},
                                    /*onNKeysLoadedFnInterval=*/0,
                                    /*keyBatchSize=*/1,
                                    /*keyBatchBytes=*/1024),
                       DBException,
                       ErrorCodes::BadValue);
}

TEST_F(IndexAccessMethodBulkBuilder, DoneCalledTwiceThrows) {
    ServiceContext::UniqueOperationContext opCtxRaii = cc().makeOperationContext();
    auto* opCtx = opCtxRaii.get();
    auto nss = NamespaceString::createNamespaceString_forTest(
        "IndexAccessMethodBulkBuilder.DoneCalledTwiceThrows");
    auto indexName = "a_1";
    auto indexSpec = BSON("name" << indexName << "key" << BSON("a" << 1) << "v"
                                 << static_cast<int>(IndexDescriptor::IndexVersion::kV2));
    ASSERT_OK(createIndexFromSpec(opCtx, nss.ns_forTest(), indexSpec));

    AutoGetCollection autoColl(opCtx, nss, LockMode::MODE_X);
    auto indexEntry = autoColl->getIndexCatalog()->findIndexByName(opCtx, indexName);
    auto indexAccessMethod = indexEntry->accessMethod()->asSortedData();

    auto bulk = indexAccessMethod->initiateBulk(opCtx,
                                                *autoColl,
                                                indexEntry,
                                                /*spiller=*/nullptr,
                                                /*maxMemoryUsageBytes=*/128 * 1024 * 1024,
                                                /*stateInfo=*/boost::none,
                                                nss.dbName(),
                                                ContainerWriteBehavior::kDoNotReplicate);
    ASSERT(bulk);

    bulk->done();
    ASSERT_THROWS_WITH_CHECK(bulk->done(), DBException, [](const DBException& ex) {
        EXPECT_EQ(ex.code(), 12723200);
        assertionCount.tripwire.subtractAndFetch(1);
    });
}

TEST_F(IndexAccessMethodBulkBuilder, CommitWithoutDoneThrows) {
    ServiceContext::UniqueOperationContext opCtxRaii = cc().makeOperationContext();
    auto* opCtx = opCtxRaii.get();
    auto nss = NamespaceString::createNamespaceString_forTest(
        "IndexAccessMethodBulkBuilder.CommitWithoutDoneThrows");
    auto indexName = "a_1";
    auto indexSpec = BSON("name" << indexName << "key" << BSON("a" << 1) << "v"
                                 << static_cast<int>(IndexDescriptor::IndexVersion::kV2));
    ASSERT_OK(createIndexFromSpec(opCtx, nss.ns_forTest(), indexSpec));

    AutoGetCollection autoColl{opCtx, nss, LockMode::MODE_X};
    auto indexEntry = autoColl->getIndexCatalog()->findIndexByName(opCtx, indexName);
    auto indexAccessMethod = indexEntry->accessMethod()->asSortedData();

    auto bulk = indexAccessMethod->initiateBulk(opCtx,
                                                *autoColl,
                                                indexEntry,
                                                /*spiller=*/nullptr,
                                                /*maxMemoryUsageBytes=*/128 * 1024 * 1024,
                                                /*stateInfo=*/boost::none,
                                                nss.dbName(),
                                                ContainerWriteBehavior::kDoNotReplicate);
    ASSERT(bulk);

    ASSERT_THROWS_WITH_CHECK(bulk->commit(opCtx,
                                          *shard_role_details::getRecoveryUnit(opCtx),
                                          &*autoColl,
                                          indexEntry,
                                          /*dupsAllowed=*/true,
                                          /*yieldIterations=*/0,
                                          IndexAccessMethod::KeyHandlerFn{
                                              [](const CollectionPtr&, const key_string::View&) {
                                                  return Status::OK();
                                              }},
                                          IndexAccessMethod::RecordIdHandlerFn{},
                                          IndexAccessMethod::YieldFn{},
                                          IndexAccessMethod::OnNKeysLoadedFn{[]() {
                                          }},
                                          IndexAccessMethod::OnBytesWrittenFn{[](int64_t) {
                                          }},
                                          /*onNKeysLoadedFnInterval=*/1,
                                          /*keyBatchSize=*/1,
                                          /*keyBatchBytes=*/1024),
                             DBException,
                             [](const DBException& ex) {
                                 EXPECT_EQ(ex.code(), 12723201);
                                 assertionCount.tripwire.subtractAndFetch(1);
                             });
}

/**
 * Returns the leading index key field of a generated key. The generated keys have the RecordId
 * appended to them, which is not of interest when checking which keys were written.
 */
template <typename KeyString>
BSONObj indexKeyToBson(const KeyString& keyString) {
    return key_string::toBson(keyString, Ordering::allAscending()).firstElement().wrap("");
}

/**
 * Matches a keystring whose leading index key field equals 'expected'.
 */
MATCHER_P(HasIndexKey, expected, "") {
    return indexKeyToBson(arg).woCompare(expected) == 0;
}

class MockSortedDataInterface : public SortedDataInterface {
public:
    MockSortedDataInterface()
        : SortedDataInterface(
              kDataFormatV2KeyStringV1IndexVersionV2, Ordering::make(BSONObj()), KeyFormat::Long) {
        ON_CALL(*this, insert).WillByDefault(::testing::Return(Status::OK()));
    }

    MOCK_METHOD((std::variant<Status, DuplicateKey>),
                insert,
                (OperationContext * opCtx,
                 RecoveryUnit& ru,
                 const key_string::View& keyString,
                 bool dupsAllowed,
                 IncludeDuplicateRecordId includeDuplicateRecordId),
                (override));
    MOCK_METHOD(void,
                unindex,
                (OperationContext * opCtx,
                 RecoveryUnit& ru,
                 const key_string::View& keyString,
                 bool dupsAllowed),
                (override));
    MOCK_METHOD(bool, isIdIndex, (), (const, override));
    MOCK_METHOD(bool, unique, (), (const, override));
    MOCK_METHOD(std::unique_ptr<SortedDataBuilderInterface>,
                makeBulkBuilder,
                (OperationContext * opCtx, RecoveryUnit& ru),
                (override));
    MOCK_METHOD(boost::optional<DuplicateKey>,
                dupKeyCheck,
                (OperationContext * opCtx, RecoveryUnit& ru, const key_string::View& keyString),
                (override));
    MOCK_METHOD(boost::optional<RecordId>,
                findLoc,
                (OperationContext * opCtx, RecoveryUnit& ru, std::span<const char> keyString),
                (const, override));
    MOCK_METHOD(IndexValidateResults,
                validate,
                (OperationContext * opCtx,
                 RecoveryUnit& ru,
                 const collection_validation::ValidationOptions& options),
                (const, override));
    MOCK_METHOD(bool,
                appendCustomStats,
                (OperationContext * opCtx, RecoveryUnit& ru, BSONObjBuilder* output, double scale),
                (const, override));
    MOCK_METHOD(long long,
                getSpaceUsedBytes,
                (OperationContext * opCtx, RecoveryUnit& ru),
                (const, override));
    MOCK_METHOD(long long,
                getFreeStorageBytes,
                (OperationContext * opCtx, RecoveryUnit& ru),
                (const, override));
    MOCK_METHOD(bool, isEmpty, (OperationContext * opCtx, RecoveryUnit& ru), (override));
    MOCK_METHOD(int64_t,
                numEntries,
                (OperationContext * opCtx, RecoveryUnit& ru),
                (const, override));
    MOCK_METHOD(void,
                printIndexEntryMetadata,
                (OperationContext * opCtx, RecoveryUnit& ru, const key_string::View& keyString),
                (const, override));
    MOCK_METHOD(std::unique_ptr<Cursor>,
                newCursor,
                (OperationContext * opCtx, RecoveryUnit& ru, bool isForward),
                (const, override));
    MOCK_METHOD(Status, initAsEmpty, (), (override));
    MOCK_METHOD(Status, truncate, (OperationContext * opCtx, RecoveryUnit& ru), (override));
    MOCK_METHOD(StringKeyedContainer&, getContainer, (), (override));
    MOCK_METHOD(const StringKeyedContainer&, getContainer, (), (const, override));
};

/**
 * Verifies that the high-level insert()/remove()/update() entry points of
 * SortedDataIndexAccessMethod only write to the underlying SortedDataInterface for documents which
 * match the index's partial filter expression.
 *
 * The index is built with a partial filter of {a: {$gt: 5}}, and the access method under test wraps
 * a MockSortedDataInterface rather than the real storage-engine index so that the writes it issues
 * can be observed directly.
 */
class IndexAccessMethodPartialFilter : public ServiceContextMongoDTest {
protected:
    static constexpr auto kIndexName = "a_1";
    static constexpr auto kIndexedValue = 5;
    static constexpr auto kExcludedValue = 4;

    void setUp() override {
        ServiceContextMongoDTest::setUp();

        _opCtxHolder = cc().makeOperationContext();
        _nss = NamespaceString::createNamespaceString_forTest(
            "unittests.IndexAccessMethodPartialFilter");

        _indexSpec =
            BSON("name" << kIndexName << "key" << BSON("a" << 1) << "v"
                        << static_cast<int>(IndexDescriptor::IndexVersion::kV2)
                        << "partialFilterExpression" << BSON("a" << BSON("$gte" << kIndexedValue)));
        ASSERT_OK(createIndexFromSpec(opCtx(), _nss.ns_forTest(), _indexSpec));
    }

    void tearDown() override {
        if (_interceptor) {
            WriteUnitOfWork wuow{opCtx()};
            _interceptor->dropTemporaryTables(opCtx(), StorageEngine::Immediate{});
            wuow.commit();
            _interceptor.reset();
        }
        ServiceContextMongoDTest::tearDown();
    }

    OperationContext* opCtx() {
        return _opCtxHolder.get();
    }

    /**
     * Acquires the collection, builds a BtreeAccessMethod backed by a MockSortedDataInterface with
     * the expectations queued by expectWrites() installed on it, and invokes 'fn' with it inside a
     * WriteUnitOfWork.
     */
    template <typename F>
    void runWithAccessMethod(F&& fn) {
        auto acq = acquireCollection(opCtx(),
                                     CollectionAcquisitionRequest::fromOpCtx(
                                         opCtx(), _nss, AcquisitionPrerequisites::kWrite),
                                     LockMode::MODE_X);
        const auto& coll = acq.getCollectionPtr();
        auto indexEntry = coll->getIndexCatalog()->findIndexByName(opCtx(), kIndexName);
        ASSERT(indexEntry);
        ASSERT(indexEntry->getFilterExpression());

        if (_useInterceptor) {
            _installInterceptor(indexEntry);
        }

        auto mock = std::make_unique<::testing::StrictMock<MockSortedDataInterface>>();
        for (const auto& setExpectation : _expectations) {
            setExpectation(*mock);
        }
        BtreeAccessMethod accessMethod{const_cast<IndexCatalogEntry*>(indexEntry), std::move(mock)};

        WriteUnitOfWork wuow{opCtx()};
        fn(accessMethod, coll, indexEntry);
        wuow.commit();

        if (_useInterceptor) {
            _readSideWrites();
            const_cast<IndexCatalogEntry*>(indexEntry)->setIndexBuildInterceptor(nullptr);
        }
    }

    Status insertDoc(const BSONObj& doc, const RecordId& loc, int64_t* numInserted) {
        Status status = Status::OK();
        runWithAccessMethod([&](SortedDataIndexAccessMethod& am,
                                const CollectionPtr& coll,
                                const IndexCatalogEntry* entry) {
            SharedBufferFragmentBuilder pooledBuilder{
                key_string::HeapBuilder::kHeapAllocatorDefaultBytes};
            std::vector<BsonRecord> records{BsonRecord{loc, Timestamp(), &doc}};
            status = am.insert(opCtx(), pooledBuilder, coll, entry, records, _options, numInserted);
        });
        return status;
    }

    void removeDoc(const BSONObj& doc, const RecordId& loc, int64_t* numDeleted) {
        runWithAccessMethod([&](SortedDataIndexAccessMethod& am,
                                const CollectionPtr& coll,
                                const IndexCatalogEntry* entry) {
            SharedBufferFragmentBuilder pooledBuilder{
                key_string::HeapBuilder::kHeapAllocatorDefaultBytes};
            am.remove(opCtx(),
                      pooledBuilder,
                      coll,
                      entry,
                      doc,
                      loc,
                      /*logIfError=*/false,
                      _options,
                      numDeleted,
                      CheckRecordId::Off);
        });
    }

    Status updateDoc(const BSONObj& oldDoc,
                     const BSONObj& newDoc,
                     const RecordId& loc,
                     int64_t* numInserted,
                     int64_t* numDeleted) {
        Status status = Status::OK();
        runWithAccessMethod([&](SortedDataIndexAccessMethod& am,
                                const CollectionPtr& coll,
                                const IndexCatalogEntry* entry) {
            SharedBufferFragmentBuilder pooledBuilder{
                key_string::HeapBuilder::kHeapAllocatorDefaultBytes};
            status = am.update(opCtx(),
                               *shard_role_details::getRecoveryUnit(opCtx()),
                               pooledBuilder,
                               oldDoc,
                               newDoc,
                               loc,
                               coll,
                               entry,
                               _options,
                               numInserted,
                               numDeleted);
        });
        return status;
    }

    /**
     * Queues expectations on the mock SortedDataInterface: each key in 'unindexed' must be passed
     * to unindex() exactly once, each key in 'inserted' must be passed to insert() exactly once,
     * and no other writes may reach the index. Must be called before the operation under test.
     */
    void expectWrites(std::vector<BSONObj> unindexed, std::vector<BSONObj> inserted) {
        _expectations.push_back([unindexed = std::move(unindexed),
                                 inserted = std::move(inserted)](MockSortedDataInterface& mock) {
            // Declared first so that it only catches the writes which do not match one of the
            // expected keys below.
            EXPECT_CALL(mock, unindex).Times(0);
            EXPECT_CALL(mock, insert).Times(0);

            using ::testing::_;
            for (const auto& key : unindexed) {
                EXPECT_CALL(mock, unindex(_, _, HasIndexKey(key), _)).Times(1);
            }
            for (const auto& key : inserted) {
                EXPECT_CALL(mock, insert(_, _, HasIndexKey(key), _, _)).Times(1);
            }
        });
    }

    void expectNoWrites() {
        expectWrites({}, {});
    }

    /**
     * Asserts that the contents of the interceptor's side writes table contains the expected
     * operations. Only meaningful for tests which run with an interceptor installed.
     */
    void assertSideWrites(const std::vector<BSONObj>& expectedDeleted,
                          const std::vector<BSONObj>& expectedInserted) {
        ASSERT_EQ(expectedDeleted.size(), _sideWriteDeletes.size());
        for (size_t i = 0; i < _sideWriteDeletes.size(); ++i) {
            ASSERT_BSONOBJ_EQ(expectedDeleted[i], _sideWriteDeletes[i]);
        }

        ASSERT_EQ(expectedInserted.size(), _sideWriteInserts.size());
        for (size_t i = 0; i < _sideWriteInserts.size(); ++i) {
            ASSERT_BSONOBJ_EQ(expectedInserted[i], _sideWriteInserts[i]);
        }
    }

    void assertNoSideWrites() {
        ASSERT_EQ(0U, _sideWriteInserts.size());
        ASSERT_EQ(0U, _sideWriteDeletes.size());
    }

    InsertDeleteOptions _options;
    bool _useInterceptor = false;

private:
    /**
     * Creates an IndexBuildInterceptor along with its temporary tables and attaches it to
     * 'indexEntry', so that the access method writes to the side writes table rather than directly
     * to the index.
     */
    void _installInterceptor(const IndexCatalogEntry* indexEntry) {
        auto* storageEngine = opCtx()->getServiceContext()->getStorageEngine();

        WriteUnitOfWork wuow{opCtx()};
        _indexBuildInfo.emplace(_indexSpec, *storageEngine, _nss.dbName());
        _interceptor =
            std::make_shared<IndexBuildInterceptor>(opCtx(),
                                                    *_indexBuildInfo,
                                                    LazyRecordStore::CreateMode::immediate,
                                                    /*unique=*/false);
        const_cast<IndexCatalogEntry*>(indexEntry)->setIndexBuildInterceptor(_interceptor);
        wuow.commit();
    }

    void _readSideWrites() {
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx());
        auto table =
            opCtx()->getServiceContext()->getStorageEngine()->getEngine()->getInternalRecordStore(
                ru, *_indexBuildInfo->sideWritesIdent, KeyFormat::Long);

        _sideWriteInserts.clear();
        _sideWriteDeletes.clear();
        auto cursor = table->getCursor(opCtx(), ru);
        while (auto record = cursor->next()) {
            auto obj = record->data.toBson();

            int keyLen;
            const char* binKey = obj["key"].binData(keyLen);
            BufReader reader(binKey, keyLen);
            auto keyString = key_string::Value::deserialize(
                reader, key_string::Version::kLatestVersion, KeyFormat::Long);

            if (obj.getStringField("op") == "i") {
                _sideWriteInserts.push_back(indexKeyToBson(keyString).getOwned());
            } else {
                _sideWriteDeletes.push_back(indexKeyToBson(keyString).getOwned());
            }
        }
    }

    std::vector<std::function<void(MockSortedDataInterface&)>> _expectations;
    ServiceContext::UniqueOperationContext _opCtxHolder;
    NamespaceString _nss;
    BSONObj _indexSpec;
    boost::optional<IndexBuildInfo> _indexBuildInfo;
    std::shared_ptr<IndexBuildInterceptor> _interceptor;
    std::vector<BSONObj> _sideWriteInserts;
    std::vector<BSONObj> _sideWriteDeletes;
};

TEST_F(IndexAccessMethodPartialFilter, InsertMatchingDocumentIndexesKeys) {
    expectWrites(/*unindexed=*/{}, /*inserted=*/{BSON("" << kIndexedValue)});

    int64_t numInserted = 0;
    ASSERT_OK(insertDoc(BSON("_id" << 0 << "a" << kIndexedValue), RecordId(1), &numInserted));
    EXPECT_EQ(1, numInserted);
}

TEST_F(IndexAccessMethodPartialFilter, InsertNonMatchingDocumentDoesNothing) {
    expectNoWrites();

    int64_t numInserted = 0;
    ASSERT_OK(insertDoc(BSON("_id" << 0 << "a" << kExcludedValue), RecordId(1), &numInserted));
    EXPECT_EQ(0, numInserted);
}

TEST_F(IndexAccessMethodPartialFilter, InsertOnlyIndexesMatchingDocuments) {
    // The filter is applied per-record, so a batch containing both matching and non-matching
    // documents only produces writes for the matching ones.
    expectWrites(/*unindexed=*/{}, /*inserted=*/{BSON("" << kIndexedValue)});

    auto matching = BSON("_id" << 0 << "a" << kIndexedValue);
    auto nonMatching = BSON("_id" << 1 << "a" << kExcludedValue);
    int64_t numInserted = 0;
    runWithAccessMethod([&](SortedDataIndexAccessMethod& am,
                            const CollectionPtr& coll,
                            const IndexCatalogEntry* entry) {
        SharedBufferFragmentBuilder pooledBuilder{
            key_string::HeapBuilder::kHeapAllocatorDefaultBytes};
        std::vector records{BsonRecord{RecordId(1), Timestamp(), &matching},
                            BsonRecord{RecordId(2), Timestamp(), &nonMatching}};
        ASSERT_OK(am.insert(opCtx(), pooledBuilder, coll, entry, records, _options, &numInserted));
    });

    EXPECT_EQ(1, numInserted);
}

TEST_F(IndexAccessMethodPartialFilter, RemoveMatchingDocumentUnindexesKeys) {
    expectWrites(/*unindexed=*/{BSON("" << kIndexedValue)}, /*inserted=*/{});

    int64_t numDeleted = 0;
    removeDoc(BSON("_id" << 0 << "a" << kIndexedValue), RecordId(1), &numDeleted);
    EXPECT_EQ(1, numDeleted);
}

TEST_F(IndexAccessMethodPartialFilter, RemoveNonMatchingDocumentDoesNothing) {
    expectNoWrites();

    int64_t numDeleted = 0;
    removeDoc(BSON("_id" << 0 << "a" << kExcludedValue), RecordId(1), &numDeleted);
    EXPECT_EQ(0, numDeleted);
}

TEST_F(IndexAccessMethodPartialFilter, UpdateWhenBothDocumentsMatch) {
    expectWrites(/*unindexed=*/{BSON("" << kIndexedValue + 1)},
                 /*inserted=*/{BSON("" << kIndexedValue + 2)});

    int64_t numInserted = 0;
    int64_t numDeleted = 0;
    ASSERT_OK(updateDoc(BSON("_id" << 0 << "a" << kIndexedValue + 1),
                        BSON("_id" << 0 << "a" << kIndexedValue + 2),
                        RecordId(1),
                        &numInserted,
                        &numDeleted));

    EXPECT_EQ(1, numInserted);
    EXPECT_EQ(1, numDeleted);
}

TEST_F(IndexAccessMethodPartialFilter, UpdateWhenOnlyOldDocumentMatches) {
    expectWrites(/*unindexed=*/{BSON("" << kIndexedValue)}, /*inserted=*/{});

    int64_t numInserted = 0;
    int64_t numDeleted = 0;
    ASSERT_OK(updateDoc(BSON("_id" << 0 << "a" << kIndexedValue),
                        BSON("_id" << 0 << "a" << kExcludedValue),
                        RecordId(1),
                        &numInserted,
                        &numDeleted));
    EXPECT_EQ(0, numInserted);
    EXPECT_EQ(1, numDeleted);
}

TEST_F(IndexAccessMethodPartialFilter, UpdateWhenOnlyNewDocumentMatches) {
    expectWrites(/*unindexed=*/{}, /*inserted=*/{BSON("" << kIndexedValue)});

    int64_t numInserted = 0;
    int64_t numDeleted = 0;
    ASSERT_OK(updateDoc(BSON("_id" << 0 << "a" << 1),
                        BSON("_id" << 0 << "a" << kIndexedValue),
                        RecordId(1),
                        &numInserted,
                        &numDeleted));
    EXPECT_EQ(1, numInserted);
    EXPECT_EQ(0, numDeleted);
}

TEST_F(IndexAccessMethodPartialFilter, UpdateWhenNeitherDocumentMatches) {
    expectNoWrites();

    int64_t numInserted = 0;
    int64_t numDeleted = 0;
    ASSERT_OK(updateDoc(BSON("_id" << 0 << "a" << kExcludedValue),
                        BSON("_id" << 0 << "a" << kExcludedValue - 1),
                        RecordId(1),
                        &numInserted,
                        &numDeleted));
    EXPECT_EQ(0, numInserted);
    EXPECT_EQ(0, numDeleted);
}

TEST_F(IndexAccessMethodPartialFilter, UpdateWhenBothDocumentsMatchWithUnchangedKey) {
    expectNoWrites();

    int64_t numInserted = 0;
    int64_t numDeleted = 0;
    ASSERT_OK(updateDoc(BSON("_id" << 0 << "a" << kIndexedValue << "b" << 1),
                        BSON("_id" << 0 << "a" << kIndexedValue << "b" << 2),
                        RecordId(1),
                        &numInserted,
                        &numDeleted));
    EXPECT_EQ(0, numInserted);
    EXPECT_EQ(0, numDeleted);
}

/**
 * The same set of cases as IndexAccessMethodPartialFilter, but with an IndexBuildInterceptor
 * installed on the index. In this configuration the access method must not touch the index itself;
 * everything it would have written goes to the interceptor's side writes table instead, and the
 * partial filter must be honored there as well.
 */
class IndexAccessMethodPartialFilterWithInterceptor : public IndexAccessMethodPartialFilter {
protected:
    void setUp() override {
        IndexAccessMethodPartialFilter::setUp();
        _useInterceptor = true;
        expectNoWrites();
    }
};

TEST_F(IndexAccessMethodPartialFilterWithInterceptor, InsertMatchingDocumentSideWritesKeys) {
    int64_t numInserted = 0;
    ASSERT_OK(insertDoc(BSON("_id" << 0 << "a" << kIndexedValue), RecordId(1), &numInserted));

    assertSideWrites(/*deleted=*/{}, /*inserted=*/{BSON("" << kIndexedValue)});
    EXPECT_EQ(1, numInserted);
}

TEST_F(IndexAccessMethodPartialFilterWithInterceptor, InsertNonMatchingDocumentDoesNothing) {
    int64_t numInserted = 0;
    ASSERT_OK(insertDoc(BSON("_id" << 0 << "a" << kExcludedValue), RecordId(1), &numInserted));

    assertNoSideWrites();
    EXPECT_EQ(0, numInserted);
}

TEST_F(IndexAccessMethodPartialFilterWithInterceptor, InsertOnlySideWritesMatchingDocuments) {
    auto matching = BSON("_id" << 0 << "a" << kIndexedValue);
    auto nonMatching = BSON("_id" << 1 << "a" << kExcludedValue);
    int64_t numInserted = 0;
    runWithAccessMethod([&](SortedDataIndexAccessMethod& am,
                            const CollectionPtr& coll,
                            const IndexCatalogEntry* entry) {
        SharedBufferFragmentBuilder pooledBuilder{
            key_string::HeapBuilder::kHeapAllocatorDefaultBytes};
        std::vector<BsonRecord> records{BsonRecord{RecordId(1), Timestamp(), &matching},
                                        BsonRecord{RecordId(2), Timestamp(), &nonMatching}};
        ASSERT_OK(am.insert(opCtx(), pooledBuilder, coll, entry, records, _options, &numInserted));
    });

    assertSideWrites(/*deleted=*/{}, /*inserted=*/{BSON("" << kIndexedValue)});
    EXPECT_EQ(1, numInserted);
}

TEST_F(IndexAccessMethodPartialFilterWithInterceptor, RemoveMatchingDocumentSideWritesDelete) {
    int64_t numDeleted = 0;
    removeDoc(BSON("_id" << 0 << "a" << kIndexedValue), RecordId(1), &numDeleted);

    assertSideWrites(/*deleted=*/{BSON("" << kIndexedValue)}, /*inserted=*/{});
    EXPECT_EQ(1, numDeleted);
}

TEST_F(IndexAccessMethodPartialFilterWithInterceptor, RemoveNonMatchingDocumentDoesNothing) {
    int64_t numDeleted = 0;
    removeDoc(BSON("_id" << 0 << "a" << 1), RecordId(1), &numDeleted);

    assertNoSideWrites();
    EXPECT_EQ(0, numDeleted);
}

TEST_F(IndexAccessMethodPartialFilterWithInterceptor, UpdateWhenBothDocumentsMatch) {
    int64_t numInserted = 0;
    int64_t numDeleted = 0;
    ASSERT_OK(updateDoc(BSON("_id" << 0 << "a" << kIndexedValue),
                        BSON("_id" << 0 << "a" << kIndexedValue + 1),
                        RecordId(1),
                        &numInserted,
                        &numDeleted));

    assertSideWrites(/*deleted=*/{BSON("" << kIndexedValue)},
                     /*inserted=*/{BSON("" << kIndexedValue + 1)});
    EXPECT_EQ(1, numInserted);
    EXPECT_EQ(1, numDeleted);
}

TEST_F(IndexAccessMethodPartialFilterWithInterceptor, UpdateWhenOnlyOldDocumentMatches) {
    int64_t numInserted = 0;
    int64_t numDeleted = 0;
    ASSERT_OK(updateDoc(BSON("_id" << 0 << "a" << kIndexedValue),
                        BSON("_id" << 0 << "a" << kExcludedValue),
                        RecordId(1),
                        &numInserted,
                        &numDeleted));

    assertSideWrites(/*deleted=*/{BSON("" << kIndexedValue)}, /*inserted=*/{});
    EXPECT_EQ(0, numInserted);
    EXPECT_EQ(1, numDeleted);
}

TEST_F(IndexAccessMethodPartialFilterWithInterceptor, UpdateWhenOnlyNewDocumentMatches) {
    int64_t numInserted = 0;
    int64_t numDeleted = 0;
    ASSERT_OK(updateDoc(BSON("_id" << 0 << "a" << kExcludedValue),
                        BSON("_id" << 0 << "a" << kIndexedValue),
                        RecordId(1),
                        &numInserted,
                        &numDeleted));

    assertSideWrites(/*deleted=*/{}, /*inserted=*/{BSON("" << kIndexedValue)});
    EXPECT_EQ(1, numInserted);
    EXPECT_EQ(0, numDeleted);
}

TEST_F(IndexAccessMethodPartialFilterWithInterceptor, UpdateWhenNeitherDocumentMatches) {
    int64_t numInserted = 0;
    int64_t numDeleted = 0;
    ASSERT_OK(updateDoc(BSON("_id" << 0 << "a" << kExcludedValue),
                        BSON("_id" << 0 << "a" << kExcludedValue - 1),
                        RecordId(1),
                        &numInserted,
                        &numDeleted));

    assertNoSideWrites();
    EXPECT_EQ(0, numInserted);
    EXPECT_EQ(0, numDeleted);
}

TEST_F(IndexAccessMethodPartialFilterWithInterceptor,
       UpdateWhenBothDocumentsMatchWithUnchangedKey) {
    int64_t numInserted = 0;
    int64_t numDeleted = 0;
    ASSERT_OK(updateDoc(BSON("_id" << 0 << "a" << kIndexedValue << "b" << 1),
                        BSON("_id" << 0 << "a" << kIndexedValue << "b" << 2),
                        RecordId(1),
                        &numInserted,
                        &numDeleted));

    assertNoSideWrites();
    EXPECT_EQ(0, numInserted);
    EXPECT_EQ(0, numDeleted);
}

}  // namespace

}  // namespace mongo
