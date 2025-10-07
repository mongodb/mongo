/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

// IWYU pragma: no_include "boost/intrusive/detail/iterator.hpp"
// IWYU pragma: no_include "boost/move/algo/detail/set_difference.hpp"
#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/database.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/validate/collection_validation.h"
#include "mongo/db/validate/validate_adaptor.h"
#include "mongo/db/validate/validate_results.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/dbtests/storage_debug_util.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/shared_buffer_fragment.h"

#include <cmath>
#include <string>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace ValidateTests {
namespace {

using CollectionValidation::ValidationOptions;

Timestamp timestampToUse = Timestamp(1, 1);
void advanceTimestamp() {
    timestampToUse = Timestamp(timestampToUse.getSecs() + 1, timestampToUse.getInc() + 1);
}

}  // namespace

/**
 * Test fixture to run extended validate, i.e. validate that produces full and partial collection
 * hashes.
 *
 * Extended validate is typically run on multiple nodes and the hash produced on each node is
 * compared. Since this is a unittest, we instead run validate on multiple collections instead
 * where each collection represents the contents of a collection on different nodes.
 */
class ExtendedValidateBase {
public:
    explicit ExtendedValidateBase(bool expectSameHash,
                                  std::vector<BSONObj> coll1Contents,
                                  std::vector<BSONObj> coll2Contents)
        : _expectSameHash(expectSameHash),
          _coll1Contents(coll1Contents),
          _coll2Contents(coll2Contents) {

        // Disable table logging. When table logging is enabled, timestamps are discarded by the
        // storage engine.
        storageGlobalParams.forceDisableTableLogging = true;
        _opCtx.getServiceContext()->getStorageEngine()->setInitialDataTimestamp(timestampToUse);
    }

    ~ExtendedValidateBase() {
        AutoGetDb autoDb(&_opCtx, _nss1.dbName(), MODE_X);
        auto db = autoDb.getDb();
        ASSERT_TRUE(db);

        beginTransaction();
        ASSERT_OK(db->dropCollection(&_opCtx, _nss1));
        ASSERT_OK(db->dropCollection(&_opCtx, _nss2));
        commitTransaction();
        getGlobalServiceContext()->unsetKillAllOperations();
    }

public:
    void run() {
        // validate() will set a kProvided read source. Callers continue to do operations after
        // running validate, so we must reset the read source back to normal before returning.
        auto originalReadSource =
            shard_role_details::getRecoveryUnit(&_opCtx)->getTimestampReadSource();
        ON_BLOCK_EXIT([&] {
            shard_role_details::getRecoveryUnit(&_opCtx)->abandonSnapshot();
            shard_role_details::getRecoveryUnit(&_opCtx)->setTimestampReadSource(
                originalReadSource);
        });

        // Initialize two collections with the provided contents.
        {
            AutoGetCollection autoColl1(&_opCtx, _nss1, MODE_IX);
            AutoGetCollection autoColl2(&_opCtx, _nss2, MODE_IX);
            auto db = autoColl1.ensureDbExists(&_opCtx);
            beginTransaction();

            // Create both collections
            auto coll1 =
                db->createCollection(&_opCtx, _nss1, CollectionOptions(), /*createIdIndex*/ true);
            ASSERT_TRUE(coll1) << _nss1.toStringForErrorMsg();
            auto coll2 =
                db->createCollection(&_opCtx, _nss2, CollectionOptions(), /*createIdIndex*/ true);
            ASSERT_TRUE(coll2) << _nss2.toStringForErrorMsg();

            // Insert documents on both collections.
            for (const auto& doc : _coll1Contents) {
                ASSERT_OK(collection_internal::insertDocument(
                    &_opCtx, coll1, InsertStatement(doc), /*opDebug=*/nullptr));
            }
            for (const auto& doc : _coll2Contents) {
                ASSERT_OK(collection_internal::insertDocument(
                    &_opCtx, coll2, InsertStatement(doc), /*opDebug=*/nullptr));
            }
            commitTransaction();
        }

        ValidateResults results1;
        ValidateResults results2;
        ScopeGuard dumpOnErrorGuard([&] {
            LOGV2(10994000, "Encountered failure. Printing results of validate on coll1.");
            StorageDebugUtil::printValidateResults(results1);
            StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, _nss1);
            LOGV2(10994001, "Printing results of validate on coll2.");
            StorageDebugUtil::printValidateResults(results2);
            StorageDebugUtil::printCollectionAndIndexTableEntries(&_opCtx, _nss2);
        });

        ASSERT_OK(CollectionValidation::validate(
            &_opCtx,
            _nss1,
            ValidationOptions{CollectionValidation::ValidateMode::kCollectionHash,
                              CollectionValidation::RepairMode::kNone,
                              /*logDiagnostics=*/true},
            &results1));
        ASSERT_TRUE(results1.isValid()) << "Validation failed when it should've worked.";

        ASSERT_OK(CollectionValidation::validate(
            &_opCtx,
            _nss2,
            ValidationOptions{CollectionValidation::ValidateMode::kCollectionHash,
                              CollectionValidation::RepairMode::kNone,
                              /*logDiagnostics=*/true},
            &results2));
        ASSERT_TRUE(results2.isValid()) << "Validation failed when it should've worked.";

        // Ensure that the hashes match up.
        if (_expectSameHash) {
            ASSERT_EQ(results1.getCollectionHash(), results2.getCollectionHash());
        } else {
            ASSERT_NE(results1.getCollectionHash(), results2.getCollectionHash());
        }

        dumpOnErrorGuard.dismiss();
    }

private:
    void beginTransaction() {
        advanceTimestamp();
        _wuow = std::make_unique<WriteUnitOfWork>(&_opCtx);
        ASSERT_OK(shard_role_details::getRecoveryUnit(&_opCtx)->setTimestamp(timestampToUse));
    }

    void commitTransaction() {
        _wuow->commit();
        _opCtx.getServiceContext()->getStorageEngine()->setStableTimestamp(timestampToUse);
    }

    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_txnPtr;
    bool _expectSameHash;
    const NamespaceString _nss1 = NamespaceString::createNamespaceString_forTest("test.coll1");
    const NamespaceString _nss2 = NamespaceString::createNamespaceString_forTest("test.coll2");
    std::vector<BSONObj> _coll1Contents;
    std::vector<BSONObj> _coll2Contents;
    std::unique_ptr<WriteUnitOfWork> _wuow;
};

class ExtendedValidateSameContentsEmptyColl : public ExtendedValidateBase {
public:
    ExtendedValidateSameContentsEmptyColl()
        : ExtendedValidateBase(
              /*expectSameHash=*/true,
              /*coll1Contents=*/{},
              /*coll2Contents=*/{}) {}
};

class ExtendedValidateDifferentContentsEmptyColl : public ExtendedValidateBase {
public:
    ExtendedValidateDifferentContentsEmptyColl()
        : ExtendedValidateBase(
              /*expectSameHash=*/false,
              /*coll1Contents=*/{BSON("_id" << 1)},
              /*coll2Contents=*/{}) {}
};

class ExtendedValidateSameContents : public ExtendedValidateBase {
public:
    ExtendedValidateSameContents()
        : ExtendedValidateBase(
              /*expectSameHash=*/true,
              /*coll1Contents=*/{BSON("_id" << 1), BSON("_id" << 2), BSON("_id" << 3)},
              /*coll2Contents=*/{BSON("_id" << 1), BSON("_id" << 2), BSON("_id" << 3)}) {}
};

class ExtendedValidateSameContentsDifferentOrder : public ExtendedValidateBase {
public:
    ExtendedValidateSameContentsDifferentOrder()
        : ExtendedValidateBase(
              /*expectSameHash=*/true,
              /*coll1Contents=*/{BSON("_id" << 1), BSON("_id" << 3), BSON("_id" << 2)},
              /*coll2Contents=*/{BSON("_id" << 1), BSON("_id" << 2), BSON("_id" << 3)}) {}
};

class ExtendedValidateDifferentId : public ExtendedValidateBase {
public:
    ExtendedValidateDifferentId()
        : ExtendedValidateBase(
              /*expectSameHash=*/false,
              /*coll1Contents=*/{BSON("_id" << 1), BSON("_id" << 2), BSON("_id" << 3)},
              /*coll2Contents=*/{BSON("_id" << 1), BSON("_id" << 2), BSON("_id" << 4)}) {}
};

class ExtendedValidateMissingDoc : public ExtendedValidateBase {
public:
    ExtendedValidateMissingDoc()
        : ExtendedValidateBase(
              /*expectSameHash=*/false,
              /*coll1Contents=*/{BSON("_id" << 1), BSON("_id" << 2), BSON("_id" << 3)},
              /*coll2Contents=*/{BSON("_id" << 1), BSON("_id" << 2)}) {}
};

class ExtendedValidateExtraField : public ExtendedValidateBase {
public:
    ExtendedValidateExtraField()
        : ExtendedValidateBase(
              /*expectSameHash=*/false,
              /*coll1Contents=*/{BSON("_id" << 1), BSON("_id" << 2), BSON("_id" << 3)},
              /*coll2Contents=*/
              {BSON("_id" << 1), BSON("_id" << 2), BSON("_id" << 3 << "a" << 3)}) {}
};

class ExtendedValidateGetNumberOfAdditionalCharacters {
public:
    void run() {
        auto maxSize = BSONObjMaxUserSize - 50 * 1024;
        auto someHash = SHA256Block().toHexString();
        auto constructPartialObj = [&](size_t numBuckets, size_t bucketKeyLength) {
            BSONObjBuilder bob;
            for (size_t i = 0; i < numBuckets; i++) {
                bob.append(std::string(bucketKeyLength, 'a'),
                           BSON("hash" << someHash << "count" << 1));
            }
            return bob.obj();
        };

        size_t hashPrefixLengthCases[] = {0, 1, 2, 4, 8, 16, 32, someHash.size()};
        size_t numPrefixesCases[] = {
            1, 2, 3, 4, 10, 50, 100, 250, 200, 400, 800, 2000, 4000, 8000, 10000, 20000, 40000};
        for (auto hashPrefixLength : hashPrefixLengthCases) {
            for (auto numPrefixes : numPrefixesCases) {
                size_t N = CollectionValidation::getNumberOfAdditionalCharactersForHashDrillDown(
                    numPrefixes, hashPrefixLength);
                size_t numBuckets = numPrefixes * std::pow(16, N);
                auto bucketKeyLength = std::min(hashPrefixLength + N, someHash.size());
                auto response = constructPartialObj(numBuckets, bucketKeyLength);
                auto responseSize = response.objsize();
                LOGV2(10994400,
                      "Value of N returned",
                      "hashPrefixLength"_attr = hashPrefixLength,
                      "numPrefixes"_attr = numPrefixes,
                      "N"_attr = N,
                      "bucketKeyLength"_attr = bucketKeyLength,
                      "numBuckets"_attr = numBuckets,
                      "responseSize"_attr = responseSize);
                ASSERT_LTE(responseSize, maxSize);
            }
        }
    }
};

class ExtendedValidateTests : public unittest::OldStyleSuiteSpecification {
public:
    ExtendedValidateTests() : OldStyleSuiteSpecification("extended_validate_tests") {}

    void setupTests() override {
        add<ExtendedValidateSameContentsEmptyColl>();
        add<ExtendedValidateDifferentContentsEmptyColl>();
        add<ExtendedValidateSameContents>();
        add<ExtendedValidateSameContentsDifferentOrder>();
        add<ExtendedValidateDifferentId>();
        add<ExtendedValidateMissingDoc>();
        add<ExtendedValidateExtraField>();
        add<ExtendedValidateGetNumberOfAdditionalCharacters>();
    }
};

unittest::OldStyleSuiteInitializer<ExtendedValidateTests> extendedValidateTests;

}  // namespace ValidateTests
}  // namespace mongo
