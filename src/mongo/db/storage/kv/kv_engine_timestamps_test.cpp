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

#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/kv/kv_engine_test_harness.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/snapshot_manager.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

class SnapshotManagerTests : public unittest::Test, public ScopedGlobalServiceContextForTest {
public:
    /**
     * Usable as an OperationContext* but owns both the Client and the OperationContext.
     */
    class Operation {
    public:
        Operation() = default;
        Operation(ServiceContext::UniqueClient client, RecoveryUnit* ru)
            : _client(std::move(client)), _opCtx(_client->makeOperationContext()) {
            _opCtx->setRecoveryUnit(std::unique_ptr<RecoveryUnit>(ru),
                                    WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
        }


        Operation(Operation&& other) = default;

        Operation& operator=(Operation&& other) {
            // Need to assign to _opCtx first if active. Otherwise we'd destroy _client before
            // _opCtx.
            _opCtx = std::move(other._opCtx);
            _client = std::move(other._client);
            return *this;
        }

        OperationContext& operator*() const {
            return *_opCtx;
        }

        OperationContext* operator->() const {
            return _opCtx.get();
        }

        operator OperationContext*() const {
            return _opCtx.get();
        }

    private:
        ServiceContext::UniqueClient _client;
        ServiceContext::UniqueOperationContext _opCtx;
    };

    Operation makeOperation() {
        return Operation(getServiceContext()->makeClient(""),
                         helper->getEngine()->newRecoveryUnit());
    }

    // Returns the timestamp before incrementing.
    Timestamp fetchAndIncrementTimestamp() {
        auto preImage = _counter;
        _counter = Timestamp(_counter.getSecs() + 1, _counter.getInc());
        return preImage;
    }

    RecordId insertRecord(OperationContext* opCtx, std::string contents = "abcd") {
        Lock::GlobalLock globalLock(opCtx, MODE_IX);
        auto id = rs->insertRecord(opCtx, contents.c_str(), contents.length() + 1, _counter);
        ASSERT_OK(id);
        return id.getValue();
    }

    RecordId insertRecordAndCommit(std::string contents = "abcd") {
        auto op = makeOperation();
        Lock::GlobalLock globalLock(op, MODE_IX);
        WriteUnitOfWork wuow(op);
        auto id = insertRecord(op, contents);
        wuow.commit();
        return id;
    }

    void updateRecordAndCommit(RecordId id, std::string contents) {
        auto op = makeOperation();
        Lock::GlobalLock globalLock(op, MODE_IX);
        WriteUnitOfWork wuow(op);
        ASSERT_OK(op->recoveryUnit()->setTimestamp(_counter));
        ASSERT_OK(rs->updateRecord(op, id, contents.c_str(), contents.length() + 1));
        wuow.commit();
    }

    void deleteRecordAndCommit(RecordId id) {
        auto op = makeOperation();
        Lock::GlobalLock globalLock(op, MODE_IX);
        WriteUnitOfWork wuow(op);
        ASSERT_OK(op->recoveryUnit()->setTimestamp(_counter));
        rs->deleteRecord(op, id);
        wuow.commit();
    }

    /**
     * Returns the number of records seen iterating rs using the passed-in OperationContext.
     */
    int itCountOn(OperationContext* opCtx) {
        Lock::GlobalLock globalLock(opCtx, MODE_IS);
        auto cursor = rs->getCursor(opCtx);
        int count = 0;
        while (auto record = cursor->next()) {
            count++;
        }
        return count;
    }

    int itCountCommitted() {
        auto op = makeOperation();
        op->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kMajorityCommitted);
        ASSERT_OK(op->recoveryUnit()->majorityCommittedSnapshotAvailable());
        return itCountOn(op);
    }

    int itCountLastApplied() {
        auto op = makeOperation();
        op->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kNoOverlap);
        return itCountOn(op);
    }

    boost::optional<Record> readRecordOn(OperationContext* op, RecordId id) {
        Lock::GlobalLock globalLock(op, MODE_IS);
        auto cursor = rs->getCursor(op);
        auto record = cursor->seekExact(id);
        if (record)
            record->data.makeOwned();
        return record;
    }
    boost::optional<Record> readRecordCommitted(RecordId id) {
        auto op = makeOperation();
        Lock::GlobalLock globalLock(op, MODE_IS);
        op->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kMajorityCommitted);
        ASSERT_OK(op->recoveryUnit()->majorityCommittedSnapshotAvailable());
        return readRecordOn(op, id);
    }

    std::string readStringCommitted(RecordId id) {
        auto record = readRecordCommitted(id);
        ASSERT(record);
        return std::string(record->data.data());
    }

    boost::optional<Record> readRecordLastApplied(RecordId id) {
        auto op = makeOperation();
        op->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kNoOverlap);
        return readRecordOn(op, id);
    }

    std::string readStringLastApplied(RecordId id) {
        auto record = readRecordLastApplied(id);
        ASSERT(record);
        return std::string(record->data.data());
    }

    void setUp() override {
        helper = KVHarnessHelper::create(getGlobalServiceContext());
        engine = helper->getEngine();
        snapshotManager = helper->getEngine()->getSnapshotManager();

        auto op = makeOperation();
        WriteUnitOfWork wuow(op);
        std::string ns = "a.b";
        ASSERT_OK(engine->createRecordStore(
            op, NamespaceString::createNamespaceString_forTest(ns), ns, CollectionOptions()));
        rs = engine->getRecordStore(
            op, NamespaceString::createNamespaceString_forTest(ns), ns, CollectionOptions());
        ASSERT(rs);
    }

    std::unique_ptr<KVHarnessHelper> helper;
    KVEngine* engine;
    SnapshotManager* snapshotManager;
    std::unique_ptr<RecordStore> rs;
    Operation snapshotOperation;

private:
    Timestamp _counter = Timestamp(1, 0);
};

}  // namespace

TEST_F(SnapshotManagerTests, ConsistentIfNotSupported) {
    if (snapshotManager)
        return;  // This test is only for engines that DON'T support SnapshotManagers.

    auto op = makeOperation();
    auto ru = op->recoveryUnit();
    auto readSource = ru->getTimestampReadSource();
    ASSERT(readSource != RecoveryUnit::ReadSource::kMajorityCommitted);
    ASSERT(!ru->getPointInTimeReadTimestamp(op));
}

TEST_F(SnapshotManagerTests, FailsWithNoCommittedSnapshot) {
    if (!snapshotManager)
        return;  // This test is only for engines that DO support SnapshotManagers.

    auto op = makeOperation();
    auto ru = op->recoveryUnit();
    op->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kMajorityCommitted);

    // Before first snapshot is created.
    ASSERT_EQ(ru->majorityCommittedSnapshotAvailable(),
              ErrorCodes::ReadConcernMajorityNotAvailableYet);

    // There is a snapshot but it isn't committed.
    auto snap = fetchAndIncrementTimestamp();
    ASSERT_EQ(ru->majorityCommittedSnapshotAvailable(),
              ErrorCodes::ReadConcernMajorityNotAvailableYet);

    // Now there is a committed snapshot.
    snapshotManager->setCommittedSnapshot(snap);
    ASSERT_OK(ru->majorityCommittedSnapshotAvailable());

    // Not anymore!
    snapshotManager->clearCommittedSnapshot();
    ASSERT_EQ(ru->majorityCommittedSnapshotAvailable(),
              ErrorCodes::ReadConcernMajorityNotAvailableYet);
}

TEST_F(SnapshotManagerTests, FailsAfterDropAllSnapshotsWhileYielded) {
    if (!snapshotManager)
        return;  // This test is only for engines that DO support SnapshotManagers.

    auto op = makeOperation();
    op->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kMajorityCommitted);

    // Hold the outer most global lock throughout the test to avoid getting snapshots abandoned when
    // inner global locks are destructed.
    Lock::GlobalLock globalLock(op, MODE_IS);

    // Start an operation using a committed snapshot.
    auto snap = fetchAndIncrementTimestamp();
    snapshotManager->setCommittedSnapshot(snap);
    ASSERT_OK(op->recoveryUnit()->majorityCommittedSnapshotAvailable());
    ASSERT_EQ(itCountOn(op), 0);  // acquires a snapshot.

    // Everything still works until we abandon our snapshot.
    snapshotManager->clearCommittedSnapshot();
    ASSERT_EQ(itCountOn(op), 0);

    // Now it doesn't.
    op->recoveryUnit()->abandonSnapshot();
    ASSERT_THROWS_CODE(
        itCountOn(op), AssertionException, ErrorCodes::ReadConcernMajorityNotAvailableYet);
}

TEST_F(SnapshotManagerTests, BasicFunctionality) {
    if (!snapshotManager)
        return;  // This test is only for engines that DO support SnapshotManagers.

    auto snap0 = fetchAndIncrementTimestamp();
    snapshotManager->setCommittedSnapshot(snap0);
    ASSERT_EQ(itCountCommitted(), 0);

    insertRecordAndCommit();

    ASSERT_EQ(itCountCommitted(), 0);


    auto snap1 = fetchAndIncrementTimestamp();

    insertRecordAndCommit();
    insertRecordAndCommit();
    auto snap3 = fetchAndIncrementTimestamp();

    {
        auto op = makeOperation();
        WriteUnitOfWork wuow(op);
        insertRecord(op);
        // rolling back wuow
    }

    insertRecordAndCommit();
    auto snap4 = fetchAndIncrementTimestamp();

    // If these fail, everything is busted.
    ASSERT_EQ(itCountCommitted(), 0);
    snapshotManager->setCommittedSnapshot(snap1);
    ASSERT_EQ(itCountCommitted(), 1);
    snapshotManager->setCommittedSnapshot(snap3);
    ASSERT_EQ(itCountCommitted(), 3);

    // Hold the outer most global lock throughout the remainder of the test to avoid getting
    // snapshots abandoned when inner global locks are destructed. This op should keep its original
    // snapshot until abandoned.
    auto longOp = makeOperation();
    Lock::GlobalLock globalLock(longOp, MODE_IS);
    longOp->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kMajorityCommitted);
    ASSERT_OK(longOp->recoveryUnit()->majorityCommittedSnapshotAvailable());
    ASSERT_EQ(itCountOn(longOp), 3);

    // If this fails, the snapshot contains writes that were rolled back.
    snapshotManager->setCommittedSnapshot(snap4);
    ASSERT_EQ(itCountCommitted(), 4);

    // If this fails, longOp changed snapshots at an illegal time.
    ASSERT_EQ(itCountOn(longOp), 3);

    // If this fails, longOp didn't get a new snapshot when it should have.
    longOp->recoveryUnit()->abandonSnapshot();
    ASSERT_EQ(itCountOn(longOp), 4);
}

TEST_F(SnapshotManagerTests, UpdateAndDelete) {
    if (!snapshotManager)
        return;  // This test is only for engines that DO support SnapshotManagers.

    auto snapBeforeInsert = fetchAndIncrementTimestamp();

    auto id = insertRecordAndCommit("Dog");
    auto snapDog = fetchAndIncrementTimestamp();

    updateRecordAndCommit(id, "Cat");
    auto snapCat = fetchAndIncrementTimestamp();

    deleteRecordAndCommit(id);
    auto snapAfterDelete = fetchAndIncrementTimestamp();

    snapshotManager->setCommittedSnapshot(snapBeforeInsert);
    ASSERT_EQ(itCountCommitted(), 0);
    ASSERT(!readRecordCommitted(id));

    snapshotManager->setCommittedSnapshot(snapDog);
    ASSERT_EQ(itCountCommitted(), 1);
    ASSERT_EQ(readStringCommitted(id), "Dog");

    snapshotManager->setCommittedSnapshot(snapCat);
    ASSERT_EQ(itCountCommitted(), 1);
    ASSERT_EQ(readStringCommitted(id), "Cat");

    snapshotManager->setCommittedSnapshot(snapAfterDelete);
    ASSERT_EQ(itCountCommitted(), 0);
    ASSERT(!readRecordCommitted(id));
}

TEST_F(SnapshotManagerTests, InsertAndReadOnLastAppliedSnapshot) {
    if (!snapshotManager)
        return;  // This test is only for engines that DO support SnapshotManagers.

    auto beforeInsert = fetchAndIncrementTimestamp();

    auto id = insertRecordAndCommit();
    auto afterInsert = fetchAndIncrementTimestamp();

    // Not reading on the last applied timestamp returns the most recent data.
    auto op = makeOperation();
    auto ru = op->recoveryUnit();
    ru->setTimestampReadSource(RecoveryUnit::ReadSource::kNoTimestamp);
    ASSERT_EQ(itCountOn(op), 1);
    ASSERT(readRecordOn(op, id));

    deleteRecordAndCommit(id);
    auto afterDelete = fetchAndIncrementTimestamp();

    // Reading at the last applied snapshot timestamps returns data in order.
    snapshotManager->setLastApplied(beforeInsert);
    ASSERT_EQ(itCountLastApplied(), 0);
    ASSERT(!readRecordLastApplied(id));

    snapshotManager->setLastApplied(afterInsert);
    ASSERT_EQ(itCountLastApplied(), 1);
    ASSERT(readRecordLastApplied(id));

    snapshotManager->setLastApplied(afterDelete);
    ASSERT_EQ(itCountLastApplied(), 0);
    ASSERT(!readRecordLastApplied(id));
}

TEST_F(SnapshotManagerTests, UpdateAndDeleteOnLocalSnapshot) {
    if (!snapshotManager)
        return;  // This test is only for engines that DO support SnapshotManagers.

    auto beforeInsert = fetchAndIncrementTimestamp();

    auto id = insertRecordAndCommit("Aardvark");
    auto afterInsert = fetchAndIncrementTimestamp();

    updateRecordAndCommit(id, "Blue spotted stingray");
    auto afterUpdate = fetchAndIncrementTimestamp();

    // Not reading on the last local timestamp returns the most recent data.
    auto op = makeOperation();
    auto ru = op->recoveryUnit();
    ru->setTimestampReadSource(RecoveryUnit::ReadSource::kNoTimestamp);
    ASSERT_EQ(itCountOn(op), 1);
    auto record = readRecordOn(op, id);
    ASSERT_EQ(std::string(record->data.data()), "Blue spotted stingray");

    deleteRecordAndCommit(id);
    auto afterDelete = fetchAndIncrementTimestamp();

    snapshotManager->setLastApplied(beforeInsert);
    ASSERT_EQ(itCountLastApplied(), 0);
    ASSERT(!readRecordLastApplied(id));

    snapshotManager->setLastApplied(afterInsert);
    ASSERT_EQ(itCountLastApplied(), 1);
    ASSERT_EQ(readStringLastApplied(id), "Aardvark");

    snapshotManager->setLastApplied(afterUpdate);
    ASSERT_EQ(itCountLastApplied(), 1);
    ASSERT_EQ(readStringLastApplied(id), "Blue spotted stingray");

    snapshotManager->setLastApplied(afterDelete);
    ASSERT_EQ(itCountLastApplied(), 0);
    ASSERT(!readRecordLastApplied(id));
}
}  // namespace mongo
