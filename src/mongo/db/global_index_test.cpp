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

#include "mongo/db/global_index.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex

namespace mongo {
namespace {

class GlobalIndexTest : public ServiceContextMongoDTest {
public:
    explicit GlobalIndexTest(Options options = {}) : ServiceContextMongoDTest(std::move(options)) {}

    OperationContext* operationContext() {
        return _opCtx.get();
    }
    repl::StorageInterface* storageInterface() {
        return _storage.get();
    }

protected:
    void setUp() override {
        // Set up mongod.
        ServiceContextMongoDTest::setUp();

        auto service = getServiceContext();
        _storage = std::make_unique<repl::StorageInterfaceImpl>();
        _opCtx = cc().makeOperationContext();

        // Set up ReplicationCoordinator and ensure that we are primary.
        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(service);
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
        repl::ReplicationCoordinator::set(service, std::move(replCoord));

        // Set up oplog collection. If the WT storage engine is used, the oplog collection is
        // expected to exist when fetching the next opTime (LocalOplogInfo::getNextOpTimes) to use
        // for a write.
        repl::createOplog(operationContext());
    }

    void tearDown() override {
        _storage.reset();
        _opCtx.reset();

        // Tear down mongod.
        ServiceContextMongoDTest::tearDown();
    }

private:
    std::unique_ptr<repl::StorageInterface> _storage;
    ServiceContext::UniqueOperationContext _opCtx;
};

// Verify that the index key's KeyString and optional TypeBits stored in the 'indexEntry' object
// matches the BSON 'key'.
void verifyStoredKeyMatchesIndexKey(const BSONObj& key,
                                    const BSONObj& indexEntry,
                                    bool expectTypeBits = false) {
    // The index entry's ik field stores the BinData(KeyString(key)) and the index entry's
    // 'tb' field stores the BinData(TypeBits(key)). The 'tb' field is not present if there are
    // no TypeBits.

    auto entryIndexKeySize = indexEntry["ik"].size();
    const auto entryIndexKeyBinData = indexEntry["ik"].binData(entryIndexKeySize);

    const auto hasTypeBits = indexEntry.hasElement("tb");
    ASSERT_EQ(expectTypeBits, hasTypeBits);

    auto tb = KeyString::TypeBits(KeyString::Version::V1);
    if (hasTypeBits) {
        auto entryTypeBitsSize = indexEntry["tb"].size();
        auto entryTypeBitsBinData = indexEntry["tb"].binData(entryTypeBitsSize);
        auto entryTypeBitsReader = BufReader(entryTypeBitsBinData, entryTypeBitsSize);
        tb = KeyString::TypeBits::fromBuffer(KeyString::Version::V1, &entryTypeBitsReader);
        ASSERT(!tb.isAllZeros());
    }

    const auto rehydratedKey =
        KeyString::toBson(entryIndexKeyBinData, entryIndexKeySize, KeyString::ALL_ASCENDING, tb);

    ASSERT_BSONOBJ_EQ(rehydratedKey, key);
    LOGV2(6789401,
          "The rehydrated index key matches the inserted index key",
          "rehydrated"_attr = rehydratedKey,
          "original"_attr = key,
          "typeBitsPresent"_attr = hasTypeBits);
}

TEST_F(GlobalIndexTest, StorageFormat) {
    const auto uuid = UUID::gen();

    global_index::createContainer(operationContext(), uuid);

    // Single field index.
    {
        const auto key = BSON(""
                              << "hola");
        const auto docKey = BSON("shk0" << 0 << "shk1" << 0 << "_id" << 0);
        const auto entryId = BSON("_id" << docKey);
        global_index::insertKey(operationContext(), uuid, key, docKey);

        // Validate that the document key is stored in the index entry's _id field.
        StatusWith<BSONObj> status = storageInterface()->findById(
            operationContext(), NamespaceString::makeGlobalIndexNSS(uuid), entryId["_id"]);
        ASSERT_OK(status.getStatus());
        const auto indexEntry = status.getValue();

        // Validate the index key.
        verifyStoredKeyMatchesIndexKey(key, indexEntry);
    }

    // Compound index.
    {
        const auto key = BSON(""
                              << "hola"
                              << "" << 1);
        const auto docKey = BSON("shk0" << 1 << "shk1" << 1 << "_id" << 1);
        const auto entryId = BSON("_id" << docKey);
        global_index::insertKey(operationContext(), uuid, key, docKey);

        // Validate that the document key is stored in the index entry's _id field.
        StatusWith<BSONObj> status = storageInterface()->findById(
            operationContext(), NamespaceString::makeGlobalIndexNSS(uuid), entryId["_id"]);
        ASSERT_OK(status.getStatus());
        const auto indexEntry = status.getValue();

        // Validate the index key.
        verifyStoredKeyMatchesIndexKey(key, indexEntry);
    }

    // Compound index with non-empty TypeBits (NumberLong).
    {
        const auto key = BSON(""
                              << "hola"
                              << "" << 2LL);
        const auto docKey = BSON("shk0" << 2 << "shk1" << 2 << "_id" << 2);
        const auto entryId = BSON("_id" << docKey);
        global_index::insertKey(operationContext(), uuid, key, docKey);

        // Validate that the document key is stored in the index entry's _id field.
        StatusWith<BSONObj> status = storageInterface()->findById(
            operationContext(), NamespaceString::makeGlobalIndexNSS(uuid), entryId["_id"]);
        ASSERT_OK(status.getStatus());
        const auto indexEntry = status.getValue();

        // Validate the index key.
        verifyStoredKeyMatchesIndexKey(key, indexEntry, true /* expectTypeBits */);
    }

    // Compound index with non-empty TypeBits (Decimal).
    {
        const auto key = BSON(""
                              << "hola"
                              << "" << 3.0);
        const auto docKey = BSON("shk0" << 2 << "shk1" << 3 << "_id" << 3);
        const auto entryId = BSON("_id" << docKey);
        global_index::insertKey(operationContext(), uuid, key, docKey);

        // Validate that the document key is stored in the index entry's _id field.
        StatusWith<BSONObj> status = storageInterface()->findById(
            operationContext(), NamespaceString::makeGlobalIndexNSS(uuid), entryId["_id"]);
        ASSERT_OK(status.getStatus());
        const auto indexEntry = status.getValue();

        // Validate the index key.
        verifyStoredKeyMatchesIndexKey(key, indexEntry, true /* expectTypeBits */);
    }
}

TEST_F(GlobalIndexTest, DuplicateKey) {
    const auto uuid = UUID::gen();
    global_index::createContainer(operationContext(), uuid);
    global_index::insertKey(
        operationContext(), uuid, BSON("" << 1), BSON("shk0" << 1 << "_id" << 1));

    // Duplicate index key.
    ASSERT_THROWS_CODE(
        global_index::insertKey(
            operationContext(), uuid, BSON("" << 1), BSON("shk0" << 123 << "_id" << 123)),
        DBException,
        ErrorCodes::DuplicateKey);
    // Duplicate index key - Decimal.
    ASSERT_THROWS_CODE(
        global_index::insertKey(
            operationContext(), uuid, BSON("" << 1.0), BSON("shk0" << 123 << "_id" << 123)),
        DBException,
        ErrorCodes::DuplicateKey);
    // Duplicate index key - NumberLong.
    ASSERT_THROWS_CODE(
        global_index::insertKey(
            operationContext(), uuid, BSON("" << 1LL), BSON("shk0" << 123 << "_id" << 123)),
        DBException,
        ErrorCodes::DuplicateKey);
}

TEST_F(GlobalIndexTest, DuplicateDocumentKey) {
    const auto uuid = UUID::gen();
    global_index::createContainer(operationContext(), uuid);
    global_index::insertKey(
        operationContext(), uuid, BSON("" << 1), BSON("shk0" << 1 << "_id" << 1));

    // Duplicate document key.
    ASSERT_THROWS_CODE(
        global_index::insertKey(
            operationContext(), uuid, BSON("" << 2), BSON("shk0" << 1 << "_id" << 1)),
        DBException,
        ErrorCodes::DuplicateKey);
    // Duplicate document key - NumberLong.
    ASSERT_THROWS_CODE(
        global_index::insertKey(
            operationContext(), uuid, BSON("" << 2), BSON("shk0" << 1LL << "_id" << 1)),
        DBException,
        ErrorCodes::DuplicateKey);
}

}  // namespace
}  // namespace mongo
