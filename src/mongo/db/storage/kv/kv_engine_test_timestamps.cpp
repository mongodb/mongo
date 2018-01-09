/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/storage/kv/kv_engine_test_harness.h"

#include <memory>
#include <string>

#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/snapshot_manager.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {
namespace {

class SnapshotManagerTests : public unittest::Test {
public:
    /**
     * Usable as an OperationContext* but owns both the Client and the OperationContext.
     */
    class Operation {
    public:
        Operation() = default;
        Operation(ServiceContext::UniqueClient client, RecoveryUnit* ru)
            : _client(std::move(client)), _opCtx(_client->makeOperationContext()) {
            delete _opCtx->releaseRecoveryUnit();
            _opCtx->setRecoveryUnit(ru, OperationContext::kNotInUnitOfWork);
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
        return Operation(service.makeClient(""), helper->getEngine()->newRecoveryUnit());
    }

    // Returns the timestamp before incrementing.
    Timestamp fetchAndIncrementTimestamp() {
        auto preImage = _counter;
        _counter = Timestamp(_counter.getSecs() + 1, _counter.getInc());
        return preImage;
    }

    RecordId insertRecord(OperationContext* opCtx, std::string contents = "abcd") {
        auto id = rs->insertRecord(opCtx, contents.c_str(), contents.length() + 1, _counter, false);
        ASSERT_OK(id);
        return id.getValue();
    }

    RecordId insertRecordAndCommit(std::string contents = "abcd") {
        auto op = makeOperation();
        WriteUnitOfWork wuow(op);
        auto id = insertRecord(op, contents);
        wuow.commit();
        return id;
    }

    void updateRecordAndCommit(RecordId id, std::string contents) {
        auto op = makeOperation();
        WriteUnitOfWork wuow(op);
        ASSERT_OK(op->recoveryUnit()->setTimestamp(_counter));
        ASSERT_OK(
            rs->updateRecord(op, id, contents.c_str(), contents.length() + 1, false, nullptr));
        wuow.commit();
    }

    void deleteRecordAndCommit(RecordId id) {
        auto op = makeOperation();
        WriteUnitOfWork wuow(op);
        ASSERT_OK(op->recoveryUnit()->setTimestamp(_counter));
        rs->deleteRecord(op, id);
        wuow.commit();
    }

    /**
     * Returns the number of records seen iterating rs using the passed-in OperationContext.
     */
    int itCountOn(OperationContext* opCtx) {
        auto cursor = rs->getCursor(opCtx);
        int count = 0;
        while (auto record = cursor->next()) {
            count++;
        }
        return count;
    }

    int itCountCommitted() {
        auto op = makeOperation();
        ASSERT_OK(op->recoveryUnit()->setReadFromMajorityCommittedSnapshot());
        return itCountOn(op);
    }

    boost::optional<Record> readRecordCommitted(RecordId id) {
        auto op = makeOperation();
        ASSERT_OK(op->recoveryUnit()->setReadFromMajorityCommittedSnapshot());
        auto cursor = rs->getCursor(op);
        auto record = cursor->seekExact(id);
        if (record)
            record->data.makeOwned();
        return record;
    }

    std::string readStringCommitted(RecordId id) {
        auto record = readRecordCommitted(id);
        ASSERT(record);
        return std::string(record->data.data());
    }

    void setUp() override {
        helper = KVHarnessHelper::create();
        engine = helper->getEngine();
        snapshotManager = helper->getEngine()->getSnapshotManager();

        auto op = makeOperation();
        WriteUnitOfWork wuow(op);
        std::string ns = "a.b";
        ASSERT_OK(engine->createRecordStore(op, ns, ns, CollectionOptions()));
        rs = engine->getRecordStore(op, ns, ns, CollectionOptions());
        ASSERT(rs);
    }

    ServiceContextNoop service;
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
        return;  // This test is only for engines that DON'T support SnapshotMangers.

    auto op = makeOperation();
    auto ru = op->recoveryUnit();
    ASSERT(!ru->isReadingFromMajorityCommittedSnapshot());
    ASSERT(!ru->getMajorityCommittedSnapshot());
}

TEST_F(SnapshotManagerTests, FailsWithNoCommittedSnapshot) {
    if (!snapshotManager)
        return;  // This test is only for engines that DO support SnapshotMangers.

    auto op = makeOperation();
    auto ru = op->recoveryUnit();

    // Before first snapshot is created.
    ASSERT_EQ(ru->setReadFromMajorityCommittedSnapshot(),
              ErrorCodes::ReadConcernMajorityNotAvailableYet);

    // There is a snapshot but it isn't committed.
    auto snap = fetchAndIncrementTimestamp();
    ASSERT_EQ(ru->setReadFromMajorityCommittedSnapshot(),
              ErrorCodes::ReadConcernMajorityNotAvailableYet);

    // Now there is a committed snapshot.
    snapshotManager->setCommittedSnapshot(snap);
    ASSERT_OK(ru->setReadFromMajorityCommittedSnapshot());

    // Not anymore!
    snapshotManager->dropAllSnapshots();
    ASSERT_EQ(ru->setReadFromMajorityCommittedSnapshot(),
              ErrorCodes::ReadConcernMajorityNotAvailableYet);
}

TEST_F(SnapshotManagerTests, FailsAfterDropAllSnapshotsWhileYielded) {
    if (!snapshotManager)
        return;  // This test is only for engines that DO support SnapshotMangers.

    auto op = makeOperation();

    // Start an operation using a committed snapshot.
    auto snap = fetchAndIncrementTimestamp();
    snapshotManager->setCommittedSnapshot(snap);
    ASSERT_OK(op->recoveryUnit()->setReadFromMajorityCommittedSnapshot());
    ASSERT_EQ(itCountOn(op), 0);  // acquires a snapshot.

    // Everything still works until we abandon our snapshot.
    snapshotManager->dropAllSnapshots();
    ASSERT_EQ(itCountOn(op), 0);

    // Now it doesn't.
    op->recoveryUnit()->abandonSnapshot();
    ASSERT_THROWS_CODE(
        itCountOn(op), AssertionException, ErrorCodes::ReadConcernMajorityNotAvailableYet);
}

TEST_F(SnapshotManagerTests, BasicFunctionality) {
    if (!snapshotManager)
        return;  // This test is only for engines that DO support SnapshotMangers.

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

    // This op should keep its original snapshot until abandoned.
    auto longOp = makeOperation();
    ASSERT_OK(longOp->recoveryUnit()->setReadFromMajorityCommittedSnapshot());
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
        return;  // This test is only for engines that DO support SnapshotMangers.

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

}  // namespace mongo
