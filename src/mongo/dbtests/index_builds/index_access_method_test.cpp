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

// IWYU pragma: no_include "boost/container/detail/flat_tree.hpp"
#include "mongo/db/index/index_access_method.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/client.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <utility>

#include <boost/container/flat_set.hpp>
#include <boost/container/vector.hpp>

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
    ASSERT_EQ(0UL, diff.first.size());
    ASSERT_EQ(0UL, diff.second.size());
}

TEST(IndexAccessMethodSetDifference, EmptyLeftShouldHaveNoDifference) {
    KeyStringSet left;
    auto right = makeKeyStringSet({BSON("" << 0)});

    auto diff = SortedDataIndexAccessMethod::setDifference(left, right);
    ASSERT_EQ(0UL, diff.first.size());
    ASSERT_EQ(1UL, diff.second.size());
}

TEST(IndexAccessMethodSetDifference, EmptyRightShouldReturnAllOfLeft) {
    auto left = makeKeyStringSet({BSON("" << 0), BSON("" << 1)});
    KeyStringSet right;

    auto diff = SortedDataIndexAccessMethod::setDifference(left, right);
    ASSERT_EQ(2UL, diff.first.size());
    ASSERT_EQ(0UL, diff.second.size());
}

TEST(IndexAccessMethodSetDifference, IdenticalSetsShouldHaveNoDifference) {
    auto left = makeKeyStringSet({BSON("" << 0), BSON("" << "string"), BSON("" << BSONNULL)});
    auto right = makeKeyStringSet({BSON("" << 0), BSON("" << "string"), BSON("" << BSONNULL)});

    auto diff = SortedDataIndexAccessMethod::setDifference(left, right);
    ASSERT_EQ(0UL, diff.first.size());
    ASSERT_EQ(0UL, diff.second.size());
}

//
// Number type comparisons.
//

void assertDistinct(BSONObj left, BSONObj right) {
    auto leftSet = makeKeyStringSet({left});
    auto rightSet = makeKeyStringSet({right});
    auto diff = SortedDataIndexAccessMethod::setDifference(leftSet, rightSet);
    ASSERT_EQ(1UL, diff.first.size());
    ASSERT_EQ(1UL, diff.second.size());
}

TEST(IndexAccessMethodSetDifference, ZerosOfDifferentTypesAreNotEquivalent) {
    const BSONObj intObj = BSON("" << static_cast<int>(0));
    const BSONObj longObj = BSON("" << static_cast<long long>(0));
    const BSONObj doubleObj = BSON("" << static_cast<double>(0.0));

    // These should compare equal with woCompare(), but should not be treated equal by the index.
    ASSERT_EQ(0, intObj.woCompare(longObj));
    ASSERT_EQ(0, longObj.woCompare(doubleObj));

    assertDistinct(intObj, longObj);
    assertDistinct(intObj, doubleObj);

    assertDistinct(longObj, intObj);
    assertDistinct(longObj, doubleObj);

    assertDistinct(doubleObj, intObj);
    assertDistinct(doubleObj, longObj);

    const BSONObj decimalObj = fromjson("{'': NumberDecimal('0')}");

    ASSERT_EQ(0, doubleObj.woCompare(decimalObj));

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
    ASSERT_EQUALS(1UL, diff.first.size());
    ASSERT_EQUALS(1UL, diff.second.size());
}

TEST(IndexAccessMethodSetDifference, SingleObjInLeftShouldFindCorrespondingObjInRight) {
    auto left = makeKeyStringSet({BSON("" << 2)});
    auto right = makeKeyStringSet({BSON("" << 1), BSON("" << 2), BSON("" << 3)});
    auto diff = SortedDataIndexAccessMethod::setDifference(left, right);
    ASSERT_EQUALS(0UL, diff.first.size());
    ASSERT_EQUALS(2UL, diff.second.size());
}

TEST(IndexAccessMethodSetDifference, SingleObjInRightShouldFindCorrespondingObjInLeft) {
    auto left = makeKeyStringSet({BSON("" << 1), BSON("" << 2), BSON("" << 3)});
    auto right = makeKeyStringSet({BSON("" << 2)});
    auto diff = SortedDataIndexAccessMethod::setDifference(left, right);
    ASSERT_EQUALS(2UL, diff.first.size());
    ASSERT_EQUALS(0UL, diff.second.size());
}

TEST(IndexAccessMethodSetDifference, LeftSetAllSmallerThanRightShouldBeDisjoint) {
    auto left = makeKeyStringSet({BSON("" << 1), BSON("" << 2), BSON("" << 3)});
    auto right = makeKeyStringSet({BSON("" << 4), BSON("" << 5), BSON("" << 6)});
    auto diff = SortedDataIndexAccessMethod::setDifference(left, right);
    ASSERT_EQUALS(3UL, diff.first.size());
    ASSERT_EQUALS(3UL, diff.second.size());
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
    ASSERT_EQUALS(3UL, diff.first.size());
    ASSERT_EQUALS(3UL, diff.second.size());
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
    ASSERT_EQUALS(2UL, diff.first.size());   // 0, 6.
    ASSERT_EQUALS(3UL, diff.second.size());  // -1, 3, 7.
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

TEST(IndexAccessMethodInsertKeys, DuplicatesCheckingOnSecondaryUniqueIndexes) {
    ServiceContext::UniqueOperationContext opCtxRaii = cc().makeOperationContext();
    OperationContext* opCtx = opCtxRaii.get();
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        "unittests.DuplicatesCheckingOnSecondaryUniqueIndexes");
    auto indexName = "a_1";
    auto indexSpec = BSON("name" << indexName << "key" << BSON("a" << 1) << "unique" << true << "v"
                                 << static_cast<int>(IndexDescriptor::IndexVersion::kV2));
    ASSERT_OK(dbtests::createIndexFromSpec(opCtx, nss.ns_forTest(), indexSpec));

    AutoGetCollection autoColl(opCtx, nss, LockMode::MODE_X);
    const auto& coll = *autoColl;
    auto indexDescriptor = coll->getIndexCatalog()->findIndexByName(opCtx, indexName);
    auto indexAccessMethod =
        coll->getIndexCatalog()->getEntry(indexDescriptor)->accessMethod()->asSortedData();

    key_string::HeapBuilder keyString1(
        key_string::Version::kLatestVersion, BSON("" << 1), Ordering::make(BSONObj()), RecordId(1));
    key_string::HeapBuilder keyString2(
        key_string::Version::kLatestVersion, BSON("" << 1), Ordering::make(BSONObj()), RecordId(2));
    KeyStringSet keys{keyString1.release(), keyString2.release()};
    struct InsertDeleteOptions options; /* options.dupsAllowed = false */
    int64_t numInserted;

    // Checks duplicates and returns the error code when constraints are enforced.
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    auto status = indexAccessMethod->insertKeys(
        opCtx, ru, coll, indexDescriptor->getEntry(), keys, options, {}, &numInserted);
    ASSERT_EQ(status.code(), ErrorCodes::DuplicateKey);
    ASSERT_EQ(numInserted, 0);

    // Skips the check on duplicates when constraints are not enforced.
    opCtx->setEnforceConstraints(false);
    ASSERT_OK(indexAccessMethod->insertKeys(
        opCtx, ru, coll, indexDescriptor->getEntry(), keys, options, {}, &numInserted));
    ASSERT_EQ(numInserted, 2);
}

TEST(IndexAccessMethodInsertKeys, InsertWhenPrepareUnique) {
    ServiceContext::UniqueOperationContext opCtxRaii = cc().makeOperationContext();
    OperationContext* opCtx = opCtxRaii.get();
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("unittests.InsertWhenPrepareUnique");
    auto indexName = "a_1";
    auto indexSpec = BSON("name" << indexName << "key" << BSON("a" << 1) << "prepareUnique" << true
                                 << "v" << static_cast<int>(IndexDescriptor::IndexVersion::kV2));
    ASSERT_OK(dbtests::createIndexFromSpec(opCtx, nss.ns_forTest(), indexSpec));

    AutoGetCollection autoColl(opCtx, nss, LockMode::MODE_X);
    const auto& coll = *autoColl;
    auto indexDescriptor = coll->getIndexCatalog()->findIndexByName(opCtx, indexName);
    auto indexAccessMethod =
        coll->getIndexCatalog()->getEntry(indexDescriptor)->accessMethod()->asSortedData();

    key_string::HeapBuilder keyString1(
        key_string::Version::kLatestVersion, BSON("" << 1), Ordering::make(BSONObj()), RecordId(1));
    key_string::HeapBuilder keyString2(
        key_string::Version::kLatestVersion, BSON("" << 1), Ordering::make(BSONObj()), RecordId(2));
    KeyStringSet keys{keyString1.release(), keyString2.release()};
    struct InsertDeleteOptions options;
    int64_t numInserted;
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);

    // Disallows new duplicates in a regular index and rejects the insert.
    auto status = indexAccessMethod->insertKeys(
        opCtx, ru, coll, indexDescriptor->getEntry(), keys, options, {}, &numInserted);
    ASSERT_EQ(status.code(), ErrorCodes::DuplicateKey);
    ASSERT_EQ(numInserted, 0);
}

TEST(IndexAccessMethodUpdateKeys, UpdateWhenPrepareUnique) {
    ServiceContext::UniqueOperationContext opCtxRaii = cc().makeOperationContext();
    OperationContext* opCtx = opCtxRaii.get();
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("unittests.UpdateWhenPrepareUnique");
    auto indexName = "a_1";
    auto indexSpec = BSON("name" << indexName << "key" << BSON("a" << 1) << "prepareUnique" << true
                                 << "v" << static_cast<int>(IndexDescriptor::IndexVersion::kV2));
    ASSERT_OK(dbtests::createIndexFromSpec(opCtx, nss.ns_forTest(), indexSpec));

    AutoGetCollection autoColl(opCtx, nss, LockMode::MODE_X);
    const auto& coll = *autoColl;
    auto indexDescriptor = coll->getIndexCatalog()->findIndexByName(opCtx, indexName);
    auto indexAccessMethod =
        coll->getIndexCatalog()->getEntry(indexDescriptor)->accessMethod()->asSortedData();

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

    // Inserts two keys.
    ASSERT_OK(indexAccessMethod->insertKeys(
        opCtx, ru, coll, indexDescriptor->getEntry(), key1, options, {}, &numInserted));
    ASSERT_EQ(numInserted, 1);
    ASSERT_OK(indexAccessMethod->insertKeys(
        opCtx, ru, coll, indexDescriptor->getEntry(), key2_old, options, {}, &numInserted));
    ASSERT_EQ(numInserted, 1);

    // Disallows new duplicates in a regular index and rejects the update.
    auto status = indexAccessMethod->doUpdate(
        opCtx, ru, coll, indexDescriptor->getEntry(), ticket, &numInserted, &numDeleted);
    ASSERT_EQ(status.code(), ErrorCodes::DuplicateKey);
    ASSERT_EQ(numInserted, 0);
    ASSERT_EQ(numDeleted, 0);
}

}  // namespace

}  // namespace mongo
