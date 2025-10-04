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

#include "mongo/db/storage/kv/kv_drop_pending_ident_reaper.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/client.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <functional>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

/**
 * Test-only implementation of KVEngine that tracks idents that have been dropped.
 */
class KVEngineMock : public KVEngine {
public:
    Status dropIdent(RecoveryUnit& ru,
                     StringData ident,
                     bool identHasSizeInfo,
                     const StorageEngine::DropIdentCallback& onDrop) override;

    void dropIdentForImport(Interruptible&, RecoveryUnit&, StringData ident) override {}

    // Unused KVEngine functions below.
    std::unique_ptr<RecoveryUnit> newRecoveryUnit() override {
        return nullptr;
    }
    std::unique_ptr<RecordStore> getRecordStore(OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                StringData ident,
                                                const RecordStore::Options& options,
                                                boost::optional<UUID> uuid) override {
        return {};
    }
    std::unique_ptr<SortedDataInterface> getSortedDataInterface(OperationContext* opCtx,
                                                                RecoveryUnit& ru,
                                                                const NamespaceString& nss,
                                                                const UUID& uuid,
                                                                StringData ident,
                                                                const IndexConfig& config,
                                                                KeyFormat keyFormat) override {
        return nullptr;
    }

    Status createRecordStore(const rss::PersistenceProvider&,
                             const NamespaceString& nss,
                             StringData ident,
                             const RecordStore::Options& options) override {
        return Status::OK();
    }

    std::unique_ptr<RecordStore> getTemporaryRecordStore(RecoveryUnit& ru,
                                                         StringData ident,
                                                         KeyFormat keyFormat) override {
        return {};
    }

    std::unique_ptr<RecordStore> makeTemporaryRecordStore(RecoveryUnit& ru,
                                                          StringData ident,
                                                          KeyFormat keyFormat) override {
        return {};
    }
    Status createSortedDataInterface(
        const rss::PersistenceProvider&,
        RecoveryUnit&,
        const NamespaceString& nss,
        const UUID& uuid,
        StringData ident,
        const IndexConfig& indexConfig,
        const boost::optional<mongo::BSONObj>& storageEngineIndexOptions) override {
        return Status::OK();
    }
    Status dropSortedDataInterface(RecoveryUnit&, StringData ident) override {
        return Status::OK();
    }
    int64_t getIdentSize(RecoveryUnit&, StringData ident) override {
        return 0;
    }
    Status repairIdent(RecoveryUnit&, StringData ident) override {
        return Status::OK();
    }

    bool isEphemeral() const override {
        return false;
    }
    bool hasIdent(RecoveryUnit&, StringData ident) const override {
        return false;
    }
    std::vector<std::string> getAllIdents(RecoveryUnit&) const override {
        return {};
    }
    void cleanShutdown(bool memLeakAllowed) override {}
    void setJournalListener(JournalListener* jl) override {}
    Timestamp getAllDurableTimestamp() const override {
        return {};
    }

    boost::optional<Timestamp> getOplogNeededForCrashRecovery() const override {
        return boost::none;
    }

    Timestamp getBackupCheckpointTimestamp() override {
        return Timestamp(0, 0);
    }

    StatusWith<Timestamp> pinOldestTimestamp(RecoveryUnit&,
                                             const std::string& requestingServiceName,
                                             Timestamp requestedTimestamp,
                                             bool roundUpIfTooOld) override {
        return Timestamp(0, 0);
    }

    void unpinOldestTimestamp(const std::string& requestingServiceName) override {}

    Timestamp getOldestTimestamp() const override {
        return Timestamp();
    }

    boost::optional<Timestamp> getRecoveryTimestamp() const override {
        return boost::none;
    }

    void setPinnedOplogTimestamp(const Timestamp& pinnedTimestamp) override {}

    Status oplogDiskLocRegister(RecoveryUnit&,
                                RecordStore* oplogRecordStore,
                                const Timestamp& opTime,
                                bool orderedCommit) override {
        return Status::OK();
    }

    void waitForAllEarlierOplogWritesToBeVisible(OperationContext* opCtx,
                                                 RecordStore* oplogRecordStore) const override {}


    bool waitUntilDurable(OperationContext* opCtx) override {
        return true;
    }

    bool waitUntilUnjournaledWritesDurable(OperationContext* opCtx,
                                           bool stableCheckpoint) override {
        return true;
    }

    bool underCachePressure(int concurrentOpOuts) override {
        return false;
    }

    BSONObj setFlagToStorageOptions(const BSONObj& storageEngineOptions,
                                    StringData flagName,
                                    boost::optional<bool> flagValue) const override {
        MONGO_UNREACHABLE;
    }

    boost::optional<bool> getFlagFromStorageOptions(const BSONObj& storageEngineOptions,
                                                    StringData flagName) const override {
        MONGO_UNREACHABLE;
    }

    void dump() const override {}

    Status insertIntoIdent(RecoveryUnit& ru,
                           StringData ident,
                           IdentKey key,
                           std::span<const char> value) override {
        MONGO_UNREACHABLE;
    }

    StatusWith<UniqueBuffer> getFromIdent(RecoveryUnit& ru,
                                          StringData ident,
                                          IdentKey key) override {
        MONGO_UNREACHABLE;
    }

    Status deleteFromIdent(RecoveryUnit& ru, StringData ident, IdentKey key) override {
        MONGO_UNREACHABLE;
    }

    bool hasDataBeenCheckpointed(StorageEngine::CheckpointIteration iteration) const override {
        return iteration < checkpointIteration;
    }

    // List of ident names removed using dropIdent().
    std::vector<std::string> droppedIdents;

    // Override to modify dropIdent() behavior.
    using DropIdentFn = std::function<Status(RecoveryUnit&, StringData)>;
    DropIdentFn dropIdentFn = [](RecoveryUnit&, StringData) {
        return Status::OK();
    };

    StorageEngine::CheckpointIteration checkpointIteration = StorageEngine::CheckpointIteration{0};
};

Status KVEngineMock::dropIdent(RecoveryUnit& ru,
                               StringData ident,
                               bool identHasSizeInfo,
                               const StorageEngine::DropIdentCallback& onDrop) {
    auto status = dropIdentFn(ru, ident);
    if (status.isOK()) {
        if (onDrop) {
            onDrop();
        }
        droppedIdents.push_back(std::string{ident});
    }
    return status;
}

class KVDropPendingIdentReaperTest : public ServiceContextTest {
private:
    void setUp() override;
    void tearDown() override;

public:
    /**
     * Returns mock KVEngine.
     */
    KVEngineMock* getEngine() const;

    /**
     * Returns OperationContext for tests.
     */
    OperationContext* getOpCtx() const;

private:
    std::unique_ptr<KVEngineMock> _engineMock;
};

Timestamp makeTimestampWithNextInc(const Timestamp& ts) {
    return {ts.getSecs(), ts.getInc() + 1};
}

void KVDropPendingIdentReaperTest::setUp() {
    ServiceContextTest::setUp();
    _engineMock = std::make_unique<KVEngineMock>();
}
void KVDropPendingIdentReaperTest::tearDown() {
    _engineMock = {};
    ServiceContextTest::tearDown();
}

KVEngineMock* KVDropPendingIdentReaperTest::getEngine() const {
    ASSERT(_engineMock);
    return _engineMock.get();
}

ServiceContext::UniqueOperationContext makeOpCtx() {
    return cc().makeOperationContext();
}

DEATH_TEST_F(KVDropPendingIdentReaperTest, DoubleDropIdentFails, "invariant") {
    const std::string identName = "ident";
    KVDropPendingIdentReaper reaper(nullptr);
    reaper.addDropPendingIdent(Timestamp(1, 0), std::make_shared<Ident>(identName));
    reaper.addDropPendingIdent(Timestamp(1, 0), std::make_shared<Ident>(identName));
}

DEATH_TEST_F(KVDropPendingIdentReaperTest, TimestampedDropAfterUnknownDrop, "invariant") {
    const std::string identName = "ident";
    KVDropPendingIdentReaper reaper(nullptr);
    reaper.dropUnknownIdent(Timestamp(1, 0), identName);
    reaper.addDropPendingIdent(Timestamp(1, 0), std::make_shared<Ident>(identName));
}

TEST_F(KVDropPendingIdentReaperTest, UntimestampedDropCanFollowUnknownDrop) {
    const std::string identName = "ident";
    KVDropPendingIdentReaper reaper(nullptr);
    auto ident = std::make_shared<Ident>(identName);
    reaper.dropUnknownIdent(Timestamp(1, 0), identName);
    reaper.addDropPendingIdent(Timestamp::min(), ident);
}

TEST_F(KVDropPendingIdentReaperTest, GetEarliestDropTimestampReturnsBoostNoneOnEmptyIdents) {
    KVDropPendingIdentReaper reaper(nullptr);
    ASSERT_FALSE(reaper.getEarliestDropTimestamp());
}

TEST_F(KVDropPendingIdentReaperTest, AddDropPendingIdentAcceptsNullDropTimestamp) {
    Timestamp nullDropTimestamp;
    const std::string identName = "myident";
    auto engine = getEngine();

    KVDropPendingIdentReaper reaper(engine);
    {
        // The reaper must have the only reference to the ident before it will drop it.
        std::shared_ptr<Ident> ident = std::make_shared<Ident>(identName);
        reaper.addDropPendingIdent(nullDropTimestamp, ident);
    }
    ASSERT_EQUALS(nullDropTimestamp, *reaper.getEarliestDropTimestamp());

    auto opCtx = makeOpCtx();
    reaper.dropIdentsOlderThan(opCtx.get(), {Seconds(100), 0});
    ASSERT_EQUALS(1U, engine->droppedIdents.size());
    ASSERT_EQUALS(identName, engine->droppedIdents.front());
}

TEST_F(KVDropPendingIdentReaperTest,
       AddDropPendingIdentWithDuplicateDropTimestampButDifferentIdent) {
    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);

    Timestamp dropTimestamp{Seconds(100), 0};
    std::string identName1 = "ident1";
    std::string identName2 = "ident2";

    {
        // The reaper must have the only references to the idents before it will drop them.
        std::shared_ptr<Ident> ident1 = std::make_shared<Ident>(identName1);
        std::shared_ptr<Ident> ident2 = std::make_shared<Ident>(identName2);
        reaper.addDropPendingIdent(dropTimestamp, ident1);
        reaper.addDropPendingIdent(dropTimestamp, ident2);
    }

    // getAllIdentNames() returns a set of drop-pending idents known to the reaper.
    auto dropPendingIdents = reaper.getAllIdentNames();
    ASSERT_EQUALS(dropPendingIdents, (std::vector{identName1, identName2}));

    // Check earliest drop timestamp.
    ASSERT_EQUALS(dropTimestamp, *reaper.getEarliestDropTimestamp());

    // This should have no effect.
    auto opCtx = makeOpCtx();
    reaper.dropIdentsOlderThan(opCtx.get(), dropTimestamp);
    ASSERT_EQUALS(0U, engine->droppedIdents.size());

    // Drop all idents managed by reaper and confirm number of drops.
    reaper.dropIdentsOlderThan(opCtx.get(), makeTimestampWithNextInc(dropTimestamp));
    ASSERT_EQUALS(2U, engine->droppedIdents.size());
    ASSERT_EQUALS(identName1, engine->droppedIdents.front());
    ASSERT_EQUALS(identName2, engine->droppedIdents.back());
}

TEST_F(KVDropPendingIdentReaperTest,
       DropIdentsOlderThanDropsIdentsWithDropTimestampsBeforeOldestTimestamp) {
    auto opCtx = makeOpCtx();

    // Generate timestamps with secs: 10, 20, ..., 50.
    const int n = 5;
    Timestamp ts[n];
    std::string identName[n];
    for (int i = 0; i < n; ++i) {
        ts[i] = {Seconds((i + 1) * 10), 0};
        identName[i] = str::stream() << "ident" << i;
    }

    // Add drop-pending ident with drop timestamp out of order and check that
    // getEarliestDropOpTime() returns earliest timestamp.
    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);
    ASSERT_FALSE(reaper.getEarliestDropTimestamp());
    for (int i = 0; i < n; ++i) {
        reaper.addDropPendingIdent(ts[i], std::make_shared<Ident>(identName[i]));
    }
    ASSERT_EQUALS(ts[0], *reaper.getEarliestDropTimestamp());

    // Committed optime before first drop optime has no effect.
    reaper.dropIdentsOlderThan(opCtx.get(), {Seconds(5), 0});
    ASSERT_EQUALS(ts[0], *reaper.getEarliestDropTimestamp());

    // Committed optime matching second drop optime will result in the first two drop-pending
    // collections being removed.
    reaper.dropIdentsOlderThan(opCtx.get(), makeTimestampWithNextInc(ts[1]));
    ASSERT_EQUALS(ts[2], *reaper.getEarliestDropTimestamp());
    ASSERT_EQUALS(2U, engine->droppedIdents.size());
    ASSERT_EQUALS(identName[0], engine->droppedIdents[0]);
    ASSERT_EQUALS(identName[1], engine->droppedIdents[1]);

    // Committed optime between third and fourth optimes will result in the third collection
    // being removed.
    reaper.dropIdentsOlderThan(opCtx.get(), {Seconds(35), 0});
    ASSERT_EQUALS(ts[3], *reaper.getEarliestDropTimestamp());
    ASSERT_EQUALS(3U, engine->droppedIdents.size());
    ASSERT_EQUALS(identName[2], engine->droppedIdents[2]);

    // Committed optime after last optime will result in all drop-pending collections being
    // removed.
    reaper.dropIdentsOlderThan(opCtx.get(), {Seconds(100), 0});
    ASSERT_FALSE(reaper.getEarliestDropTimestamp());
    ASSERT_EQUALS(5U, engine->droppedIdents.size());
    ASSERT_EQUALS(identName[3], engine->droppedIdents[3]);
    ASSERT_EQUALS(identName[4], engine->droppedIdents[4]);
}

TEST_F(KVDropPendingIdentReaperTest, DropIdentsOlderThanSkipsIdentsStillReferencedElsewhere) {
    auto opCtx = makeOpCtx();
    auto engine = getEngine();
    const Timestamp dropTimestamp{Seconds(100), 0};
    const Timestamp laterThanDropTimestamp{Seconds(200), 0};
    std::string identNames[4] = {"ident1", "ident2", "ident3", "ident4"};

    KVDropPendingIdentReaper reaper(engine);

    {
        // Keep references to 2 idents; and then make 2 more idents and release their references
        // after giving them to the reaper. The latter 2 should be droppable; the former 2
        // should not be droppable.
        std::shared_ptr<Ident> ident0 = std::make_shared<Ident>(identNames[0]);
        std::shared_ptr<Ident> ident1 = std::make_shared<Ident>(identNames[1]);
        {
            std::shared_ptr<Ident> ident2 = std::make_shared<Ident>(identNames[2]);
            std::shared_ptr<Ident> ident3 = std::make_shared<Ident>(identNames[3]);
            reaper.addDropPendingIdent(dropTimestamp, ident0);
            reaper.addDropPendingIdent(dropTimestamp, ident1);
            reaper.addDropPendingIdent(dropTimestamp, ident2);
            reaper.addDropPendingIdent(dropTimestamp, ident3);
        }

        // All the idents have dropTimestamps old enough to drop, but only ident2 and ident3
        // should be dropped because ident0 and ident1 are still referenced.
        reaper.dropIdentsOlderThan(opCtx.get(), laterThanDropTimestamp);
        ASSERT_EQUALS(2U, engine->droppedIdents.size());
        ASSERT_EQUALS(identNames[2], engine->droppedIdents[0]);
        ASSERT_EQUALS(identNames[3], engine->droppedIdents[1]);
        ASSERT_EQUALS(dropTimestamp, reaper.getEarliestDropTimestamp());
    }

    // Now the ident0 and ident1 references have been released and only the reaper retains
    // references to them and should be able to drop them..
    reaper.dropIdentsOlderThan(opCtx.get(), laterThanDropTimestamp);
    ASSERT_EQUALS(4U, engine->droppedIdents.size());
    ASSERT_EQUALS(identNames[0], engine->droppedIdents[2]);
    ASSERT_EQUALS(identNames[1], engine->droppedIdents[3]);
    ASSERT_FALSE(reaper.getEarliestDropTimestamp());
}

TEST_F(KVDropPendingIdentReaperTest, MarkUnknownIdentInUse) {
    const std::string identName = "ident";
    KVDropPendingIdentReaper reaper(getEngine());
    ASSERT_FALSE(reaper.markIdentInUse(identName));
}

TEST_F(KVDropPendingIdentReaperTest, MarkUnexpiredIdentInUse) {
    const std::string identName = "ident";
    const Timestamp dropTimestamp = Timestamp(50, 50);
    auto engine = getEngine();

    KVDropPendingIdentReaper reaper(engine);

    // The reaper will not have an expired reference to the ident.
    std::shared_ptr<Ident> ident = std::make_shared<Ident>(identName);
    reaper.addDropPendingIdent(dropTimestamp, ident);

    ASSERT_EQUALS(dropTimestamp, *reaper.getEarliestDropTimestamp());

    // Marking an unexpired ident as in-use will return a shared_ptr to that ident.
    std::shared_ptr<Ident> newIdent = reaper.markIdentInUse(identName);
    ASSERT_EQ(ident, newIdent);
    ASSERT_EQ(2, ident.use_count());

    auto opCtx = makeOpCtx();
    reaper.dropIdentsOlderThan(opCtx.get(), Timestamp::max());
    ASSERT_EQUALS(0U, engine->droppedIdents.size());

    // Remove the references to the ident so that the reaper can drop it the next time.
    ident.reset();
    newIdent.reset();

    reaper.dropIdentsOlderThan(opCtx.get(), Timestamp::max());
    ASSERT_EQUALS(1U, engine->droppedIdents.size());
    ASSERT_EQUALS(identName, engine->droppedIdents.front());
}

TEST_F(KVDropPendingIdentReaperTest, MarkExpiredIdentInUse) {
    const std::string identName = "ident";
    const Timestamp dropTimestamp = Timestamp(50, 50);
    auto engine = getEngine();

    KVDropPendingIdentReaper reaper(engine);
    {
        // The reaper will have an expired weak_ptr to the ident.
        std::shared_ptr<Ident> ident = std::make_shared<Ident>(identName);
        reaper.addDropPendingIdent(dropTimestamp, ident);
    }

    ASSERT_EQUALS(dropTimestamp, *reaper.getEarliestDropTimestamp());

    // Mark the ident as in use to prevent the reaper from dropping it.
    std::shared_ptr<Ident> ident = reaper.markIdentInUse(identName);
    ASSERT_EQ(1, ident.use_count());

    // The reaper should continue to return pointers to the same ident after creating a new one
    ASSERT_EQUALS(ident, reaper.markIdentInUse(identName));

    auto opCtx = makeOpCtx();
    reaper.dropIdentsOlderThan(opCtx.get(), Timestamp::max());
    ASSERT_EQUALS(0U, engine->droppedIdents.size());

    // Remove the reference to the ident so that the reaper can drop it the next time.
    ident.reset();

    reaper.dropIdentsOlderThan(opCtx.get(), Timestamp::max());
    ASSERT_EQUALS(1U, engine->droppedIdents.size());
    ASSERT_EQUALS(identName, engine->droppedIdents.front());
}

DEATH_TEST_F(KVDropPendingIdentReaperTest,
             DropIdentsOlderThanTerminatesIfKVEngineFailsToDropIdent,
             "Failed to remove drop-pending ident") {
    Timestamp dropTimestamp{Seconds{1}, 0};
    std::string identName = "myident";

    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);
    {
        // The reaper must have the only reference to the ident before it will drop it.
        std::shared_ptr<Ident> ident = std::make_shared<Ident>(identName);
        reaper.addDropPendingIdent(dropTimestamp, ident);
    }
    ASSERT_EQUALS(dropTimestamp, *reaper.getEarliestDropTimestamp());

    // Make KVEngineMock::dropIndent() fail.
    engine->dropIdentFn = [&identName](RecoveryUnit& ru, StringData identToDropName) {
        ASSERT_EQUALS(identName, identToDropName);
        return Status(ErrorCodes::OperationFailed, "Mock KV engine dropIndent() failed.");
    };

    auto opCtx = makeOpCtx();
    reaper.dropIdentsOlderThan(opCtx.get(), makeTimestampWithNextInc(dropTimestamp));
}

TEST_F(KVDropPendingIdentReaperTest, ImmediatelyDropUnknownIdent) {
    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);
    auto opCtx = makeOpCtx();
    ASSERT_OK(reaper.immediatelyCompletePendingDrop(opCtx.get(), "nonexistent ident"));
    ASSERT_EQUALS(0U, engine->droppedIdents.size());
}

TEST_F(KVDropPendingIdentReaperTest, ImmediatelyDropUntimestampedDrop) {
    const std::string identName = "ident";
    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);
    reaper.addDropPendingIdent(Timestamp::min(), std::make_shared<Ident>(identName));

    auto opCtx = makeOpCtx();
    ASSERT_OK(reaper.immediatelyCompletePendingDrop(opCtx.get(), identName));
    ASSERT_EQUALS(engine->droppedIdents, std::vector{identName});
    ASSERT_TRUE(reaper.getAllIdentNames().empty());
}

TEST_F(KVDropPendingIdentReaperTest, ImmediatelyDropTimestampedDrop) {
    const std::string identName = "ident";
    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);
    reaper.addDropPendingIdent(Timestamp(50, 50), std::make_shared<Ident>(identName));

    auto opCtx = makeOpCtx();
    ASSERT_EQUALS(reaper.immediatelyCompletePendingDrop(opCtx.get(), identName),
                  ErrorCodes::ObjectIsBusy);
    ASSERT_TRUE(engine->droppedIdents.empty());
    ASSERT_EQUALS(reaper.getAllIdentNames(), std::vector{identName});
}

TEST_F(KVDropPendingIdentReaperTest, ImmediatelyDropImpreciseTimestamp) {
    const std::string identName = "ident";
    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);
    reaper.dropUnknownIdent(Timestamp(50, 50), identName);

    auto opCtx = makeOpCtx();
    ASSERT_OK(reaper.immediatelyCompletePendingDrop(opCtx.get(), identName));
    ASSERT_EQUALS(engine->droppedIdents, std::vector{identName});
    ASSERT_TRUE(reaper.getAllIdentNames().empty());
}

TEST_F(KVDropPendingIdentReaperTest, ImmediatelyDropIdentNotInCatalog) {
    const std::string identName = "ident";
    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);
    reaper.dropUnknownIdent(Timestamp(50, 50), identName);

    auto opCtx = makeOpCtx();
    ASSERT_OK(reaper.immediatelyCompletePendingDrop(opCtx.get(), identName));
    ASSERT_EQUALS(engine->droppedIdents, std::vector{identName});
    ASSERT_TRUE(reaper.getAllIdentNames().empty());
}

TEST_F(KVDropPendingIdentReaperTest, ImmediatelyDropOnlyDropsTheRequestedIdent) {
    const std::string identName = "ident";
    const std::string otherIdentName = "ident2";
    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);
    reaper.addDropPendingIdent(Timestamp::min(), std::make_shared<Ident>(identName));
    reaper.addDropPendingIdent(Timestamp::min(), std::make_shared<Ident>(otherIdentName));

    auto opCtx = makeOpCtx();
    ASSERT_OK(reaper.immediatelyCompletePendingDrop(opCtx.get(), identName));
    ASSERT_EQUALS(engine->droppedIdents, std::vector{identName});
    ASSERT_EQUALS(reaper.getAllIdentNames(), std::vector{otherIdentName});
}

TEST_F(KVDropPendingIdentReaperTest, ImmediatelyDropCallsOnDropCallback) {
    const std::string identName = "ident";
    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);
    bool onDropCalled = false;
    reaper.addDropPendingIdent(
        Timestamp::min(), std::make_shared<Ident>(identName), [&] { onDropCalled = true; });

    auto opCtx = makeOpCtx();
    ASSERT_OK(reaper.immediatelyCompletePendingDrop(opCtx.get(), identName));
    ASSERT_EQUALS(engine->droppedIdents, std::vector{identName});
    ASSERT(onDropCalled);
}

TEST_F(KVDropPendingIdentReaperTest, ImmediatelyDropReportsDropErrors) {
    const std::string identName = "ident";
    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);
    reaper.addDropPendingIdent(Timestamp::min(), std::make_shared<Ident>(identName));

    auto opCtx = makeOpCtx();
    engine->dropIdentFn = [](RecoveryUnit&, StringData) {
        return Status(ErrorCodes::OperationFailed, "Mock KV engine dropIndent() failed.");
    };
    ASSERT_EQUALS(reaper.immediatelyCompletePendingDrop(opCtx.get(), identName),
                  ErrorCodes::OperationFailed);
    ASSERT_TRUE(engine->droppedIdents.empty());

    // If we had untracked the ident on error this would return Status::OK()
    ASSERT_EQUALS(reaper.immediatelyCompletePendingDrop(opCtx.get(), identName),
                  ErrorCodes::OperationFailed);
}

DEATH_TEST_F(KVDropPendingIdentReaperTest, ImmediatelyDropIdentInUse, "invariant") {
    auto ident = std::make_shared<Ident>("ident");
    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);
    reaper.addDropPendingIdent(Timestamp::min(), ident);

    auto opCtx = makeOpCtx();
    reaper.immediatelyCompletePendingDrop(opCtx.get(), ident->getIdent()).ignore();
}

TEST_F(KVDropPendingIdentReaperTest, RollbackDropsAfterStableTimestamp) {
    KVDropPendingIdentReaper reaper(nullptr);
    reaper.addDropPendingIdent(StorageEngine::CheckpointIteration(1),
                               std::make_shared<Ident>("checkpoint"));
    reaper.addDropPendingIdent(Timestamp::min(), std::make_shared<Ident>("Timestamp::min()"));
    reaper.addDropPendingIdent(Timestamp(1, 0), std::make_shared<Ident>("Timestamp(1, 0)"));
    reaper.addDropPendingIdent(Timestamp(2, 0), std::make_shared<Ident>("Timestamp(2, 0)"));
    reaper.addDropPendingIdent(Timestamp(3, 0), std::make_shared<Ident>("Timestamp(3, 0)"));

    auto opCtx = makeOpCtx();
    reaper.rollbackDropsAfterStableTimestamp(Timestamp(3, 0));
    ASSERT_EQ(reaper.getNumIdents(), 5);  // did not remove any
    reaper.rollbackDropsAfterStableTimestamp(Timestamp(1, 0));
    ASSERT_EQ(reaper.getAllIdentNames(),
              (std::vector<std::string>{"checkpoint", "Timestamp::min()", "Timestamp(1, 0)"}));
    reaper.rollbackDropsAfterStableTimestamp(Timestamp::min());
    ASSERT_EQ(reaper.getAllIdentNames(),
              (std::vector<std::string>{"checkpoint", "Timestamp::min()"}));
}

TEST_F(KVDropPendingIdentReaperTest, NullTimestampIsExpiredAtMin) {
    KVDropPendingIdentReaper reaper(nullptr);
    auto ident = std::make_shared<Ident>("ident");
    reaper.addDropPendingIdent(Timestamp::min(), ident);
    ASSERT_FALSE(reaper.hasExpiredIdents(Timestamp::min()));
    ident.reset();
    ASSERT_TRUE(reaper.hasExpiredIdents(Timestamp::min()));
}

TEST_F(KVDropPendingIdentReaperTest, TimestampIsExpiredAtGreaterTime) {
    KVDropPendingIdentReaper reaper(nullptr);
    auto ident = std::make_shared<Ident>("ident");
    reaper.addDropPendingIdent(Timestamp(10, 0), ident);
    ASSERT_FALSE(reaper.hasExpiredIdents(Timestamp(10, 0)));
    ASSERT_FALSE(reaper.hasExpiredIdents(Timestamp(11, 0)));
    ident.reset();
    ASSERT_FALSE(reaper.hasExpiredIdents(Timestamp(10, 0)));
    ASSERT_TRUE(reaper.hasExpiredIdents(Timestamp(11, 0)));
}

TEST_F(KVDropPendingIdentReaperTest, HasExpiredReturnsTrueEvenIfOldestDropIsLive) {
    KVDropPendingIdentReaper reaper(nullptr);
    auto ident = std::make_shared<Ident>("ident");
    reaper.addDropPendingIdent(Timestamp(1, 0), ident);
    reaper.addDropPendingIdent(Timestamp(2, 0), std::make_shared<Ident>("ident2"));
    ASSERT_FALSE(reaper.hasExpiredIdents(Timestamp(2, 0)));
    ASSERT_TRUE(reaper.hasExpiredIdents(Timestamp(3, 0)));
}

TEST_F(KVDropPendingIdentReaperTest, HasExpiredChecksForCheckpointIteration) {
    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);
    reaper.addDropPendingIdent(engine->checkpointIteration, std::make_shared<Ident>("ident"));
    ASSERT_FALSE(reaper.hasExpiredIdents(Timestamp::min()));
    ++engine->checkpointIteration;
    ASSERT_TRUE(reaper.hasExpiredIdents(Timestamp::min()));
}

}  // namespace
}  // namespace mongo
