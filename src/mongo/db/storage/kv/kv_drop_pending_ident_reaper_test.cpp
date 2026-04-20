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
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/rss/stub_persistence_provider.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/storage/devnull/devnull_kv_engine.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <functional>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

using ::testing::_;

class ReaperTestPersistenceProvider : public rss::StubPersistenceProvider {
public:
    MOCK_METHOD(std::string, name, (), (const, override));
    MOCK_METHOD(bool, usesSchemaEpochs, (), (const, override));
    MOCK_METHOD(uint64_t, getSchemaEpochForTimestamp, (Timestamp ts), (const, override));
};

class ReplicatedIdentDropOpObserverMock : public OpObserverNoop {
public:
    MOCK_METHOD(void,
                onReplicatedIdentDrop,
                (OperationContext * opCtx, const std::string& ident, repl::OpTime& opTime),
                (override));
};

/**
 * Test-only implementation of KVEngine that tracks idents that have been dropped.
 */
class KVEngineMock : public DevNullKVEngine {
public:
    struct DroppedIdent {
        std::string identName;
        // Set for replicated ident drops. boost::none for non-replicated drops.
        boost::optional<uint64_t> schemaEpoch;
    };

    Status dropIdent(RecoveryUnit& ru,
                     StringData ident,
                     bool identHasSizeInfo,
                     const StorageEngine::DropIdentCallback& onDrop,
                     boost::optional<uint64_t> schemaEpoch) override {
        auto status = dropIdentFn(ru, ident);
        if (status.isOK()) {
            if (onDrop) {
                onDrop();
            }
            droppedIdents.emplace_back(std::string{ident}, schemaEpoch);
        }
        return status;
    }

    bool hasDataBeenCheckpointed(StorageEngine::CheckpointIteration iteration) const override {
        return iteration < checkpointIteration;
    }

    StorageEngine::CheckpointIteration getCheckpointIteration() const override {
        return checkpointIteration;
    }

    std::vector<std::string> getDroppedIdentNames() const {
        std::vector<std::string> names;
        for (const auto& droppedIdent : droppedIdents) {
            names.push_back(droppedIdent.identName);
        }
        return names;
    }

    // List of idents removed using dropIdent().
    std::vector<DroppedIdent> droppedIdents;

    // Override to modify dropIdent() behavior.
    using DropIdentFn = std::function<Status(RecoveryUnit&, StringData)>;
    DropIdentFn dropIdentFn = [](RecoveryUnit&, StringData) {
        return Status::OK();
    };

    StorageEngine::CheckpointIteration checkpointIteration{0};
};

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

    void setUsesSchemaEpochs(bool usesSchemaEpochs);

    void expectSchemaEpochForTimestamp(Timestamp ts, uint64_t epoch);

    void setPrimary(bool isPrimary);

    ReplicatedIdentDropOpObserverMock* _opObserverMock = nullptr;

private:
    std::unique_ptr<KVEngineMock> _engineMock;
    ReaperTestPersistenceProvider* _persistenceProviderMock = nullptr;
};

using Timestamps = StorageEngine::TimestampMonitor::Timestamps;
Timestamps makeTimestamps(Timestamp ts) {
    return {ts, ts, ts};
}
Timestamps makeTimestamps(int64_t seconds) {
    return makeTimestamps(Timestamp(Seconds(seconds), 0));
}

Timestamps makeTimestampWithNextInc(const Timestamp& ts) {
    auto inc = Timestamp{ts.getSecs(), ts.getInc() + 1};
    return {inc, inc, inc};
}

void dropIdentAtOldest(KVDropPendingIdentReaper& reaper,
                       Timestamp ts,
                       const std::string& identName) {
    reaper.addDropPendingIdent(StorageEngine::OldestTimestamp{ts},
                               std::make_shared<Ident>(identName));
}

void dropIdentAtStable(KVDropPendingIdentReaper& reaper,
                       Timestamp ts,
                       const std::string& identName) {
    reaper.addDropPendingIdent(StorageEngine::StableTimestamp{ts},
                               std::make_shared<Ident>(identName));
}

void KVDropPendingIdentReaperTest::setUp() {
    ServiceContextTest::setUp();
    repl::ReplicationCoordinator::set(
        getServiceContext(),
        std::make_unique<repl::ReplicationCoordinatorMock>(getServiceContext()));
    auto persistenceProvider = std::make_unique<testing::NiceMock<ReaperTestPersistenceProvider>>();
    _persistenceProviderMock = persistenceProvider.get();
    ON_CALL(*_persistenceProviderMock, name())
        .WillByDefault(testing::Return("ReaperTestPersistenceProvider"));
    rss::ReplicatedStorageService::get(getServiceContext())
        .setPersistenceProvider(std::move(persistenceProvider));

    auto opObserver = std::make_unique<ReplicatedIdentDropOpObserverMock>();
    _opObserverMock = opObserver.get();
    getServiceContext()->setOpObserver(std::move(opObserver));
    setUsesSchemaEpochs(false);
    setPrimary(true);
    _engineMock = std::make_unique<KVEngineMock>();
}
void KVDropPendingIdentReaperTest::tearDown() {
    _engineMock = {};
    _persistenceProviderMock = nullptr;
    _opObserverMock = nullptr;
    ServiceContextTest::tearDown();
}

KVEngineMock* KVDropPendingIdentReaperTest::getEngine() const {
    ASSERT(_engineMock);
    return _engineMock.get();
}

ServiceContext::UniqueOperationContext makeOpCtx() {
    return cc().makeOperationContext();
}

void KVDropPendingIdentReaperTest::setUsesSchemaEpochs(bool usesSchemaEpochs) {
    ASSERT(_persistenceProviderMock);
    ON_CALL(*_persistenceProviderMock, usesSchemaEpochs())
        .WillByDefault(testing::Return(usesSchemaEpochs));
}

void KVDropPendingIdentReaperTest::expectSchemaEpochForTimestamp(Timestamp ts, uint64_t epoch) {
    ASSERT(_persistenceProviderMock);
    EXPECT_CALL(*_persistenceProviderMock, getSchemaEpochForTimestamp(ts))
        .WillOnce(testing::Return(epoch));
}

void KVDropPendingIdentReaperTest::setPrimary(bool isPrimary) {
    auto replCoord = repl::ReplicationCoordinator::get(getServiceContext());
    ASSERT_OK(replCoord->setFollowerMode(isPrimary ? repl::MemberState::RS_PRIMARY
                                                   : repl::MemberState::RS_SECONDARY));
}

using KVDropPendingIdentReaperTestDeathTest = KVDropPendingIdentReaperTest;
DEATH_TEST_F(KVDropPendingIdentReaperTestDeathTest, DoubleDropIdentFails, "invariant") {
    const std::string identName = "ident";
    KVDropPendingIdentReaper reaper(nullptr);
    dropIdentAtOldest(reaper, Timestamp(1, 0), identName);
    dropIdentAtOldest(reaper, Timestamp(1, 0), identName);
}

DEATH_TEST_F(KVDropPendingIdentReaperTestDeathTest,
             OldestTimestampedDropAfterUnknownDrop,
             "invariant") {
    const std::string identName = "ident";
    KVDropPendingIdentReaper reaper(nullptr);
    reaper.dropUnknownIdent(Timestamp(1, 0), identName);
    dropIdentAtOldest(reaper, Timestamp(1, 0), identName);
}

DEATH_TEST_F(KVDropPendingIdentReaperTestDeathTest,
             StableTimestampedDropAfterUnknownDrop,
             "invariant") {
    const std::string identName = "ident";
    KVDropPendingIdentReaper reaper(nullptr);
    reaper.dropUnknownIdent(Timestamp(1, 0), identName);
    dropIdentAtStable(reaper, Timestamp(1, 0), identName);
}

TEST_F(KVDropPendingIdentReaperTest, DropUnknownIdentOnDropPendingIdent) {
    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);
    dropIdentAtOldest(reaper, Timestamp(10, 0), "oldest-10");
    dropIdentAtStable(reaper, Timestamp(20, 0), "stable-20");
    dropIdentAtOldest(reaper, Timestamp(30, 0), "oldest-30");
    dropIdentAtStable(reaper, Timestamp(40, 0), "stable-40");

    // Simulate RTS and catalog reconciliation at timestamp 20. Drops with timestamps after that
    // are converted to immediate, and drops before are left unchanged.
    reaper.dropUnknownIdent(Timestamp(25, 0), "oldest-10");
    reaper.dropUnknownIdent(Timestamp(25, 0), "stable-20");
    reaper.dropUnknownIdent(Timestamp(25, 0), "oldest-30");
    reaper.dropUnknownIdent(Timestamp(25, 0), "stable-40");

    auto opCtx = makeOpCtx();

    // Drop immediate only
    auto timestamps = makeTimestamps(1);
    reaper.dropIdentsOlderThan(opCtx.get(), timestamps);
    ASSERT_EQUALS(engine->getDroppedIdentNames(),
                  (std::vector<std::string>{"oldest-30", "stable-40"}));
    engine->droppedIdents.clear();

    // Other two idents should still be properly registered as stable and oldest drops and only be
    // dropped when the correct timestamp is set
    timestamps = makeTimestamps(21);
    timestamps.oldest = Timestamp(1, 0);
    reaper.dropIdentsOlderThan(opCtx.get(), timestamps);
    ASSERT_EQUALS(engine->getDroppedIdentNames(), std::vector<std::string>{"stable-20"});
    engine->droppedIdents.clear();

    timestamps = makeTimestamps(11);
    timestamps.stable = Timestamp(1, 0);
    reaper.dropIdentsOlderThan(opCtx.get(), timestamps);
    ASSERT_EQUALS(engine->getDroppedIdentNames(), std::vector<std::string>{"oldest-10"});
}

TEST_F(KVDropPendingIdentReaperTest, DropUnknownIdentWithMultipleDropsAtTheSameTimestamp) {
    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);
    dropIdentAtOldest(reaper, Timestamp(10, 0), "oldest-10-1");
    dropIdentAtOldest(reaper, Timestamp(10, 0), "oldest-10-2");
    dropIdentAtStable(reaper, Timestamp(10, 0), "stable-10");

    // Simulate a catalog reconciliation at timestamp 9 where only oldest-10-2 is missing from the
    // catalog. This doesn't actually happen in practice (it would mean that tables created at
    // different timestamps were dropped at the same timestamp), but the reaper should stay
    // internally consistent.
    reaper.dropUnknownIdent(Timestamp(9, 0), "oldest-10-2");

    auto opCtx = makeOpCtx();

    // Drop immediate only
    reaper.dropIdentsOlderThan(opCtx.get(), makeTimestamps(10));
    ASSERT_EQUALS(engine->getDroppedIdentNames(), (std::vector<std::string>{"oldest-10-2"}));
    engine->droppedIdents.clear();

    // Drop the other two
    reaper.dropIdentsOlderThan(opCtx.get(), makeTimestamps(11));
    ASSERT_EQUALS(engine->getDroppedIdentNames(),
                  (std::vector<std::string>{"oldest-10-1", "stable-10"}));
}

DEATH_TEST_F(KVDropPendingIdentReaperTestDeathTest,
             AddDropPendingIdentRejectsNullDropTimestamp,
             "invariant") {
    Timestamp nullDropTimestamp;
    KVDropPendingIdentReaper reaper(getEngine());
    dropIdentAtOldest(reaper, nullDropTimestamp, "ident");
}

TEST_F(KVDropPendingIdentReaperTest,
       AddDropPendingIdentWithDuplicateDropTimestampButDifferentIdent) {
    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);

    Timestamp dropTimestamp{Seconds(100), 0};
    std::string identName1 = "ident1";
    std::string identName2 = "ident2";
    std::string identName3 = "ident3";

    dropIdentAtOldest(reaper, dropTimestamp, identName1);
    dropIdentAtOldest(reaper, dropTimestamp, identName2);
    dropIdentAtStable(reaper, dropTimestamp, identName3);

    // getAllIdentNames() returns a set of drop-pending idents known to the reaper.
    auto dropPendingIdents = reaper.getAllIdentNames();
    ASSERT_EQUALS(dropPendingIdents, (std::set<std::string>{identName1, identName2, identName3}));

    // Check earliest drop timestamp.
    ASSERT_EQUALS(dropTimestamp, *reaper.getEarliestDropTimestamp());

    // This should have no effect.
    auto opCtx = makeOpCtx();
    reaper.dropIdentsOlderThan(opCtx.get(), makeTimestamps(dropTimestamp));
    ASSERT_EQUALS(0U, engine->droppedIdents.size());

    // Drop all idents managed by reaper and confirm number of drops.
    reaper.dropIdentsOlderThan(opCtx.get(), makeTimestampWithNextInc(dropTimestamp));
    ASSERT_EQUALS(3U, engine->droppedIdents.size());
    ASSERT_EQUALS(identName1, engine->droppedIdents[0].identName);
    ASSERT_EQUALS(identName2, engine->droppedIdents[1].identName);
    ASSERT_EQUALS(identName3, engine->droppedIdents[2].identName);
}

TEST_F(KVDropPendingIdentReaperTest, DropIdentsOlderThanUsesCorrectTimestamps) {
    auto opCtx = makeOpCtx();

    // Populate the reaper with both oldest and stable timestamped drops, call dropIdentsOlderThan()
    // with the given timestamps, and return the dropped idents.
    auto reapAtTimestamps = [&](int64_t checkpoint, int64_t oldest, int64_t stable) {
        auto engine = getEngine();
        KVDropPendingIdentReaper reaper(engine);
        dropIdentAtOldest(reaper, Timestamp(10, 0), "oldest-10");
        dropIdentAtStable(reaper, Timestamp(10, 0), "stable-10");
        dropIdentAtOldest(reaper, Timestamp(20, 0), "oldest-20");
        dropIdentAtStable(reaper, Timestamp(20, 0), "stable-20");
        dropIdentAtOldest(reaper, Timestamp(30, 0), "oldest-30");
        dropIdentAtStable(reaper, Timestamp(30, 0), "stable-30");
        reaper.dropIdentsOlderThan(
            opCtx.get(), {Timestamp(checkpoint, 0), Timestamp(oldest, 0), Timestamp(stable, 0)});
        auto droppedIdents = engine->getDroppedIdentNames();
        engine->droppedIdents.clear();
        return droppedIdents;
    };

    // All zero reaps nothing since we only have timestamped drops
    ASSERT_EQUALS(reapAtTimestamps(0, 0, 0).size(), 0U);

    // All tables are expired based on the stable and oldest timestamps, but reaping is limited by
    // checkpoint progress
    ASSERT_EQUALS(
        reapAtTimestamps(0, 50, 50),
        (std::vector<std::string>{
            "oldest-10", "oldest-20", "oldest-30", "stable-10", "stable-20", "stable-30"}));
    ASSERT_EQUALS(reapAtTimestamps(10, 50, 50), (std::vector<std::string>{}));
    ASSERT_EQUALS(reapAtTimestamps(10, 50, 50).size(), 0U);
    ASSERT_EQUALS(reapAtTimestamps(11, 50, 50),
                  (std::vector<std::string>{"oldest-10", "stable-10"}));
    ASSERT_EQUALS(reapAtTimestamps(21, 50, 50),
                  (std::vector<std::string>{"oldest-10", "oldest-20", "stable-10", "stable-20"}));
    ASSERT_EQUALS(
        reapAtTimestamps(31, 50, 50),
        (std::vector<std::string>{
            "oldest-10", "oldest-20", "oldest-30", "stable-10", "stable-20", "stable-30"}));

    // As above, but checking oldest and stable separately to verify the correct timestamp is being
    // used
    ASSERT_EQUALS(reapAtTimestamps(0, 0, 50),
                  (std::vector<std::string>{"stable-10", "stable-20", "stable-30"}));
    ASSERT_EQUALS(reapAtTimestamps(10, 0, 50), (std::vector<std::string>{}));
    ASSERT_EQUALS(reapAtTimestamps(10, 0, 50).size(), 0U);
    ASSERT_EQUALS(reapAtTimestamps(11, 0, 50), (std::vector<std::string>{"stable-10"}));
    ASSERT_EQUALS(reapAtTimestamps(21, 0, 50),
                  (std::vector<std::string>{"stable-10", "stable-20"}));
    ASSERT_EQUALS(reapAtTimestamps(31, 0, 50),
                  (std::vector<std::string>{"stable-10", "stable-20", "stable-30"}));

    ASSERT_EQUALS(reapAtTimestamps(0, 50, 0),
                  (std::vector<std::string>{"oldest-10", "oldest-20", "oldest-30"}));
    ASSERT_EQUALS(reapAtTimestamps(10, 50, 0), (std::vector<std::string>{}));
    ASSERT_EQUALS(reapAtTimestamps(10, 50, 0).size(), 0U);
    ASSERT_EQUALS(reapAtTimestamps(11, 50, 0), (std::vector<std::string>{"oldest-10"}));
    ASSERT_EQUALS(reapAtTimestamps(21, 50, 0),
                  (std::vector<std::string>{"oldest-10", "oldest-20"}));
    ASSERT_EQUALS(reapAtTimestamps(31, 50, 0),
                  (std::vector<std::string>{"oldest-10", "oldest-20", "oldest-30"}));

    // Check reaping based on stable timestamp when checkpoint isn't the limiting factor
    ASSERT_EQUALS(reapAtTimestamps(31, 0, 10), (std::vector<std::string>{}));
    ASSERT_EQUALS(reapAtTimestamps(31, 0, 11), (std::vector<std::string>{"stable-10"}));
    ASSERT_EQUALS(reapAtTimestamps(31, 0, 21),
                  (std::vector<std::string>{"stable-10", "stable-20"}));
    ASSERT_EQUALS(reapAtTimestamps(31, 0, 31),
                  (std::vector<std::string>{"stable-10", "stable-20", "stable-30"}));

    // Check reaping based on oldest timestamp when checkpoint isn't the limiting factor
    ASSERT_EQUALS(reapAtTimestamps(31, 10, 0), (std::vector<std::string>{}));
    ASSERT_EQUALS(reapAtTimestamps(31, 11, 0), (std::vector<std::string>{"oldest-10"}));
    ASSERT_EQUALS(reapAtTimestamps(31, 21, 0),
                  (std::vector<std::string>{"oldest-10", "oldest-20"}));
    ASSERT_EQUALS(reapAtTimestamps(31, 31, 0),
                  (std::vector<std::string>{"oldest-10", "oldest-20", "oldest-30"}));

    // Both stable and oldest with different values
    ASSERT_EQUALS(reapAtTimestamps(31, 10, 10), (std::vector<std::string>{}));
    ASSERT_EQUALS(reapAtTimestamps(31, 21, 11),
                  (std::vector<std::string>{"oldest-10", "oldest-20", "stable-10"}));
    ASSERT_EQUALS(reapAtTimestamps(31, 11, 21),
                  (std::vector<std::string>{"oldest-10", "stable-10", "stable-20"}));
}

TEST_F(KVDropPendingIdentReaperTest, DropIdentsOlderThanSkipsIdentsStillReferencedElsewhere) {
    auto opCtx = makeOpCtx();
    auto engine = getEngine();
    const Timestamp dropTimestamp{Seconds(100), 0};
    const auto laterThanDropTimestamp = makeTimestamps(200);
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
            reaper.addDropPendingIdent(StorageEngine::OldestTimestamp{dropTimestamp}, ident0);
            reaper.addDropPendingIdent(StorageEngine::OldestTimestamp{dropTimestamp}, ident1);
            reaper.addDropPendingIdent(StorageEngine::OldestTimestamp{dropTimestamp}, ident2);
            reaper.addDropPendingIdent(StorageEngine::OldestTimestamp{dropTimestamp}, ident3);
        }

        // All the idents have dropTimestamps old enough to drop, but only ident2 and ident3
        // should be dropped because ident0 and ident1 are still referenced.
        reaper.dropIdentsOlderThan(opCtx.get(), laterThanDropTimestamp);
        ASSERT_EQUALS(2U, engine->droppedIdents.size());
        ASSERT_EQUALS(identNames[2], engine->droppedIdents[0].identName);
        ASSERT_EQUALS(identNames[3], engine->droppedIdents[1].identName);
        ASSERT_EQUALS(dropTimestamp, reaper.getEarliestDropTimestamp());
    }

    // Now the ident0 and ident1 references have been released and only the reaper retains
    // references to them and should be able to drop them..
    reaper.dropIdentsOlderThan(opCtx.get(), laterThanDropTimestamp);
    ASSERT_EQUALS(4U, engine->droppedIdents.size());
    ASSERT_EQUALS(identNames[0], engine->droppedIdents[2].identName);
    ASSERT_EQUALS(identNames[1], engine->droppedIdents[3].identName);
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
    reaper.addDropPendingIdent(StorageEngine::OldestTimestamp{dropTimestamp}, ident);

    ASSERT_EQUALS(dropTimestamp, *reaper.getEarliestDropTimestamp());

    // Marking an unexpired ident as in-use will return a shared_ptr to that ident.
    std::shared_ptr<Ident> newIdent = reaper.markIdentInUse(identName);
    ASSERT_EQ(ident, newIdent);
    ASSERT_EQ(2, ident.use_count());

    auto opCtx = makeOpCtx();
    reaper.dropIdentsOlderThan(opCtx.get(), makeTimestamps(Timestamp::max()));
    ASSERT_EQUALS(0U, engine->droppedIdents.size());

    // Remove the references to the ident so that the reaper can drop it the next time.
    ident.reset();
    newIdent.reset();

    reaper.dropIdentsOlderThan(opCtx.get(), makeTimestamps(Timestamp::max()));
    ASSERT_EQUALS(1U, engine->droppedIdents.size());
    ASSERT_EQUALS(identName, engine->droppedIdents.front().identName);
}

TEST_F(KVDropPendingIdentReaperTest, MarkExpiredIdentInUse) {
    const std::string identName = "ident";
    const Timestamp dropTimestamp = Timestamp(50, 50);
    auto engine = getEngine();

    KVDropPendingIdentReaper reaper(engine);
    {
        // The reaper will have an expired weak_ptr to the ident.
        std::shared_ptr<Ident> ident = std::make_shared<Ident>(identName);
        reaper.addDropPendingIdent(StorageEngine::OldestTimestamp{dropTimestamp}, ident);
    }

    ASSERT_EQUALS(dropTimestamp, *reaper.getEarliestDropTimestamp());

    // Mark the ident as in use to prevent the reaper from dropping it.
    std::shared_ptr<Ident> ident = reaper.markIdentInUse(identName);
    ASSERT_EQ(1, ident.use_count());

    // The reaper should continue to return pointers to the same ident after creating a new one
    ASSERT_EQUALS(ident, reaper.markIdentInUse(identName));

    auto opCtx = makeOpCtx();
    reaper.dropIdentsOlderThan(opCtx.get(), makeTimestamps(Timestamp::max()));
    ASSERT_EQUALS(0U, engine->droppedIdents.size());

    // Remove the reference to the ident so that the reaper can drop it the next time.
    ident.reset();

    reaper.dropIdentsOlderThan(opCtx.get(), makeTimestamps(Timestamp::max()));
    ASSERT_EQUALS(1U, engine->droppedIdents.size());
    ASSERT_EQUALS(identName, engine->droppedIdents.front().identName);
}

DEATH_TEST_F(KVDropPendingIdentReaperTestDeathTest,
             DropIdentsOlderThanTerminatesIfKVEngineFailsToDropIdent,
             "Failed to remove drop-pending ident") {
    Timestamp dropTimestamp{Seconds{1}, 0};
    std::string identName = "myident";

    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);
    {
        // The reaper must have the only reference to the ident before it will drop it.
        std::shared_ptr<Ident> ident = std::make_shared<Ident>(identName);
        reaper.addDropPendingIdent(StorageEngine::OldestTimestamp{dropTimestamp}, ident);
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
    reaper.addDropPendingIdent(StorageEngine::Immediate{}, std::make_shared<Ident>(identName));

    auto opCtx = makeOpCtx();
    ASSERT_OK(reaper.immediatelyCompletePendingDrop(opCtx.get(), identName));
    ASSERT_EQUALS(engine->getDroppedIdentNames(), std::vector{identName});
    ASSERT_TRUE(reaper.getAllIdentNames().empty());
}

TEST_F(KVDropPendingIdentReaperTest, ImmediatelyDropOldestTimestampedDrop) {
    const std::string identName = "ident";
    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);
    dropIdentAtOldest(reaper, Timestamp(50, 50), identName);

    ASSERT_EQUALS(reaper.immediatelyCompletePendingDrop(makeOpCtx().get(), identName),
                  ErrorCodes::ObjectIsBusy);
    ASSERT_TRUE(engine->droppedIdents.empty());
    ASSERT_EQUALS(reaper.getAllIdentNames(), std::set{identName});
}

TEST_F(KVDropPendingIdentReaperTest, ImmediatelyDropStableTimestampedDrop) {
    const std::string identName = "ident";
    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);
    dropIdentAtStable(reaper, Timestamp(50, 50), identName);

    ASSERT_EQUALS(reaper.immediatelyCompletePendingDrop(makeOpCtx().get(), identName),
                  ErrorCodes::ObjectIsBusy);
    ASSERT_TRUE(engine->droppedIdents.empty());
    ASSERT_EQUALS(reaper.getAllIdentNames(), std::set{identName});
}

TEST_F(KVDropPendingIdentReaperTest, ImmediatelyDropTimestampedDropAtTimestampPassesTimestamp) {
    const std::string identName = "ident";
    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);
    Timestamp pendingDropTimestamp(50, 0);
    Timestamp replicatedIdentDropTimestamp(60, 0);
    dropIdentAtOldest(reaper, pendingDropTimestamp, identName);

    const uint64_t expectedSchemaEpoch = 42;
    expectSchemaEpochForTimestamp(replicatedIdentDropTimestamp, expectedSchemaEpoch);

    auto opCtx = makeOpCtx();
    ASSERT_OK(reaper.immediatelyCompletePendingDropAtTimestamp(
        opCtx.get(), identName, replicatedIdentDropTimestamp));

    // Assert ident was dropped with the expected schema epoch.
    ASSERT_EQUALS(engine->getDroppedIdentNames(), std::vector{identName});
    ASSERT_EQUALS(engine->droppedIdents.size(), 1U);
    ASSERT_EQUALS(engine->droppedIdents.front().identName, identName);
    ASSERT_EQUALS(engine->droppedIdents.front().schemaEpoch, expectedSchemaEpoch);

    // Assert the ident is no longer tracked as pending by the reaper.
    ASSERT_TRUE(reaper.getAllIdentNames().empty());
}

TEST_F(KVDropPendingIdentReaperTest, ImmediatelyDropAtTimestampUnknownIdentReturnsOK) {
    const std::string identName = "nonexistent ident";
    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);

    auto opCtx = makeOpCtx();
    ASSERT_OK(
        reaper.immediatelyCompletePendingDropAtTimestamp(opCtx.get(), identName, Timestamp(1, 0)));
    ASSERT_TRUE(engine->droppedIdents.empty());
}

TEST_F(KVDropPendingIdentReaperTest, ImmediatelyDropAtTimestampReportsDropErrors) {
    const std::string identName = "ident";
    Timestamp dropTimestamp(50, 0);
    Timestamp replicatedIdentDropTimestamp(60, 0);
    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);
    dropIdentAtOldest(reaper, dropTimestamp, identName);

    auto opCtx = makeOpCtx();
    engine->dropIdentFn = [](RecoveryUnit&, StringData) {
        return Status(ErrorCodes::OperationFailed, "Mock KV engine dropIndent() failed.");
    };
    ASSERT_EQUALS(reaper.immediatelyCompletePendingDropAtTimestamp(
                      opCtx.get(), identName, replicatedIdentDropTimestamp),
                  ErrorCodes::OperationFailed);
    ASSERT_TRUE(engine->droppedIdents.empty());
    ASSERT_EQUALS(reaper.getAllIdentNames(), std::set{identName});
}

TEST_F(KVDropPendingIdentReaperTest, ImmediatelyDropAtTimestampTooEarlyReturnsObjectIsBusy) {
    const std::string identName = "ident";
    Timestamp dropTimestamp(50, 50);

    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);
    dropIdentAtOldest(reaper, dropTimestamp, identName);

    // When identDropTs < collDropTs => Error.
    auto opCtx = makeOpCtx();
    ASSERT_EQUALS(
        reaper.immediatelyCompletePendingDropAtTimestamp(opCtx.get(), identName, Timestamp(50, 49)),
        ErrorCodes::ObjectIsBusy);
    ASSERT_TRUE(engine->droppedIdents.empty());
    ASSERT_EQUALS(reaper.getAllIdentNames(), std::set{identName});

    // When identDropTs == collDropTs => Error.
    ASSERT_EQUALS(
        reaper.immediatelyCompletePendingDropAtTimestamp(opCtx.get(), identName, dropTimestamp),
        ErrorCodes::ObjectIsBusy);
    ASSERT_TRUE(engine->droppedIdents.empty());
    ASSERT_EQUALS(reaper.getAllIdentNames(), std::set{identName});
}

TEST_F(KVDropPendingIdentReaperTest, ImmediatelyDropAtTimestampOnlyCompletesTimestampedDrops) {
    const std::string identName = "ident";
    Timestamp dropTimestamp(50, 50);

    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);
    reaper.addDropPendingIdent(StorageEngine::Immediate{}, std::make_shared<Ident>("immediate"));
    reaper.addDropPendingIdent(StorageEngine::CheckpointIteration{0},
                               std::make_shared<Ident>("checkpoint"));

    // When identDropTs < collDropTs => Error.
    auto opCtx = makeOpCtx();
    ASSERT_EQUALS(reaper.immediatelyCompletePendingDropAtTimestamp(
                      opCtx.get(), "immediate", Timestamp(50, 49)),
                  ErrorCodes::BadValue);
    ASSERT_EQUALS(reaper.immediatelyCompletePendingDropAtTimestamp(
                      opCtx.get(), "checkpoint", Timestamp(50, 49)),
                  ErrorCodes::BadValue);
    ASSERT_TRUE(engine->droppedIdents.empty());
}

TEST_F(KVDropPendingIdentReaperTest, ImmediatelyDropImpreciseTimestamp) {
    const std::string identName = "ident";
    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);
    reaper.dropUnknownIdent(Timestamp(50, 50), identName);

    auto opCtx = makeOpCtx();
    ASSERT_OK(reaper.immediatelyCompletePendingDrop(opCtx.get(), identName));
    ASSERT_EQUALS(engine->getDroppedIdentNames(), std::vector{identName});
    ASSERT_TRUE(reaper.getAllIdentNames().empty());
}

TEST_F(KVDropPendingIdentReaperTest, ImmediatelyDropIdentNotInCatalog) {
    const std::string identName = "ident";
    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);
    reaper.dropUnknownIdent(Timestamp(50, 50), identName);

    auto opCtx = makeOpCtx();
    ASSERT_OK(reaper.immediatelyCompletePendingDrop(opCtx.get(), identName));
    ASSERT_EQUALS(engine->getDroppedIdentNames(), std::vector{identName});
    ASSERT_TRUE(reaper.getAllIdentNames().empty());
}

TEST_F(KVDropPendingIdentReaperTest, ImmediatelyDropOnlyDropsTheRequestedIdent) {
    const std::string identName = "ident";
    const std::string otherIdentName = "ident2";
    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);
    reaper.addDropPendingIdent(StorageEngine::Immediate{}, std::make_shared<Ident>(identName));
    reaper.addDropPendingIdent(StorageEngine::Immediate{}, std::make_shared<Ident>(otherIdentName));

    auto opCtx = makeOpCtx();
    ASSERT_OK(reaper.immediatelyCompletePendingDrop(opCtx.get(), identName));
    ASSERT_EQUALS(engine->getDroppedIdentNames(), std::vector{identName});
    ASSERT_EQUALS(reaper.getAllIdentNames(), std::set{otherIdentName});
}

TEST_F(KVDropPendingIdentReaperTest, ImmediatelyDropCallsOnDropCallback) {
    const std::string identName = "ident";
    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);
    bool onDropCalled = false;
    reaper.addDropPendingIdent(StorageEngine::Immediate{}, std::make_shared<Ident>(identName), [&] {
        onDropCalled = true;
    });

    auto opCtx = makeOpCtx();
    ASSERT_OK(reaper.immediatelyCompletePendingDrop(opCtx.get(), identName));
    ASSERT_EQUALS(engine->getDroppedIdentNames(), std::vector{identName});
    ASSERT(onDropCalled);
}

TEST_F(KVDropPendingIdentReaperTest, ImmediatelyDropReportsDropErrors) {
    const std::string identName = "ident";
    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);
    reaper.addDropPendingIdent(StorageEngine::Immediate{}, std::make_shared<Ident>(identName));

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

DEATH_TEST_F(KVDropPendingIdentReaperTestDeathTest, ImmediatelyDropIdentInUse, "invariant") {
    auto ident = std::make_shared<Ident>("ident");
    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);
    reaper.addDropPendingIdent(StorageEngine::Immediate{}, ident);

    auto opCtx = makeOpCtx();
    reaper.immediatelyCompletePendingDrop(opCtx.get(), ident->getIdent()).ignore();
}

TEST_F(KVDropPendingIdentReaperTest, RollbackDropsAfterStableTimestamp) {
    KVDropPendingIdentReaper reaper(nullptr);
    reaper.addDropPendingIdent(StorageEngine::CheckpointIteration(1),
                               std::make_shared<Ident>("checkpoint"));
    reaper.addDropPendingIdent(StorageEngine::Immediate{},
                               std::make_shared<Ident>("StorageEngine::Immediate{}"));
    reaper.addDropPendingIdent(StorageEngine::OldestTimestamp{Timestamp(1, 0)},
                               std::make_shared<Ident>("OldestTimestamp(1, 0)"));
    reaper.addDropPendingIdent(StorageEngine::OldestTimestamp{Timestamp(2, 0)},
                               std::make_shared<Ident>("OldestTimestamp(2, 0)"));
    reaper.addDropPendingIdent(StorageEngine::StableTimestamp{Timestamp(1, 0)},
                               std::make_shared<Ident>("StableTimestamp(1, 0)"));
    reaper.addDropPendingIdent(StorageEngine::StableTimestamp{Timestamp(2, 0)},
                               std::make_shared<Ident>("StableTimestamp(2, 0)"));

    auto opCtx = makeOpCtx();
    reaper.rollbackDropsAfterStableTimestamp(Timestamp(3, 0));
    ASSERT_EQ(reaper.getNumIdents(), 6);  // did not remove any
    reaper.rollbackDropsAfterStableTimestamp(Timestamp(1, 0));
    ASSERT_EQ(reaper.getAllIdentNames(),
              (std::set<std::string>{
                  "StorageEngine::Immediate{}",
                  "checkpoint",
                  "OldestTimestamp(1, 0)",
                  "StableTimestamp(1, 0)",
              }));
    reaper.rollbackDropsAfterStableTimestamp(Timestamp::min());
    ASSERT_EQ(reaper.getAllIdentNames(),
              (std::set<std::string>{"StorageEngine::Immediate{}", "checkpoint"}));
}

TEST_F(KVDropPendingIdentReaperTest, DropIdentsChecksForInterruptsBeforeDropping) {
    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);

    Timestamp dropTimestamp{Seconds(10), 0};
    std::string identName = "ident";

    dropIdentAtOldest(reaper, dropTimestamp, identName);
    {
        auto opCtx = makeOpCtx();
        opCtx->markKilled();
        ASSERT_THROWS_CODE(
            reaper.dropIdentsOlderThan(opCtx.get(), makeTimestampWithNextInc(dropTimestamp)),
            DBException,
            ErrorCodes::Interrupted);
        ASSERT_EQUALS(0U, engine->droppedIdents.size());
    }

    {
        auto opCtx = makeOpCtx();
        reaper.dropIdentsOlderThan(opCtx.get(), makeTimestampWithNextInc(dropTimestamp));
        ASSERT_EQUALS(1U, engine->droppedIdents.size());
    }
}

TEST_F(KVDropPendingIdentReaperTest, DropIdentsOlderThan_ASCPrimaryAndSecondaryDropIndependently) {
    setUsesSchemaEpochs(false);
    auto engine = getEngine();

    // Expect the replicated ident drop observer NOT to be called.
    EXPECT_CALL(*_opObserverMock, onReplicatedIdentDrop(_, _, _)).Times(0);

    auto opCtx = makeOpCtx();
    auto runDropCase = [&](bool isPrimary, const std::string& identName) {
        setPrimary(isPrimary);
        KVDropPendingIdentReaper reaper(engine);
        dropIdentAtOldest(reaper, Timestamp(10, 0), identName);
        reaper.dropIdentsOlderThan(opCtx.get(), makeTimestamps(11));
    };

    runDropCase(true, "ident-1");
    runDropCase(false, "ident-2");

    ASSERT_EQUALS((std::vector<std::string>{"ident-1", "ident-2"}), engine->getDroppedIdentNames());
    ASSERT_EQUALS(2U, engine->droppedIdents.size());
    ASSERT_FALSE(engine->droppedIdents[0].schemaEpoch);
    ASSERT_FALSE(engine->droppedIdents[1].schemaEpoch);
}

TEST_F(KVDropPendingIdentReaperTest, DropIdentsOlderThan_DSCPrimaryReplicatesIdentDrop) {
    setUsesSchemaEpochs(true);
    setPrimary(true);

    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);
    const std::string identName("my-ident");
    const Timestamp replicatedIdentDropOpTime(100, 0);
    const uint64_t expectedSchemaEpoch = 42;
    dropIdentAtOldest(reaper, Timestamp(10, 0), identName);

    EXPECT_CALL(*_opObserverMock, onReplicatedIdentDrop(_, identName, _))
        .WillOnce([&](OperationContext*, const std::string&, repl::OpTime& opTime) {
            opTime = repl::OpTime(replicatedIdentDropOpTime, repl::OpTime::kUninitializedTerm);
        });
    expectSchemaEpochForTimestamp(replicatedIdentDropOpTime, expectedSchemaEpoch);

    auto opCtx = makeOpCtx();
    reaper.dropIdentsOlderThan(opCtx.get(), makeTimestamps(11));

    ASSERT_EQUALS(1U, engine->droppedIdents.size());
    ASSERT_EQUALS(identName, engine->droppedIdents.front().identName);
    ASSERT_EQUALS(expectedSchemaEpoch, engine->droppedIdents.front().schemaEpoch.value());
}

TEST_F(KVDropPendingIdentReaperTest, DropIdentsOlderThan_DSCPrimaryOnlyReplicatesTimestampedDrops) {
    setUsesSchemaEpochs(true);
    setPrimary(true);

    auto engine = getEngine();
    auto opCtx = makeOpCtx();
    {
        const std::string identName("i-timestamped-drop");
        const Timestamp replicatedIdentDropOpTime(200, 0);
        KVDropPendingIdentReaper reaper(engine);
        EXPECT_CALL(*_opObserverMock, onReplicatedIdentDrop(_, identName, _))
            .WillOnce([&](OperationContext*, const std::string&, repl::OpTime& opTime) {
                opTime = repl::OpTime(replicatedIdentDropOpTime, repl::OpTime::kUninitializedTerm);
            });
        dropIdentAtOldest(reaper, Timestamp(10, 0), identName);
        reaper.dropIdentsOlderThan(opCtx.get(), makeTimestamps(11));
        ASSERT_EQ(1U, engine->droppedIdents.size());
        engine->droppedIdents.clear();
    }

    {
        KVDropPendingIdentReaper reaper(engine);
        const std::string identName("i-drop-on-CheckpointIteration");
        engine->checkpointIteration = StorageEngine::CheckpointIteration{5};
        reaper.addDropPendingIdent(engine->checkpointIteration, std::make_shared<Ident>(identName));
        EXPECT_CALL(*_opObserverMock, onReplicatedIdentDrop(_, _, _)).Times(0);
        reaper.dropIdentsOlderThan(opCtx.get(), makeTimestamps(1000000));
        ASSERT_TRUE(engine->droppedIdents.empty());
        engine->checkpointIteration = StorageEngine::CheckpointIteration{6};
        reaper.dropIdentsOlderThan(opCtx.get(), makeTimestamps(1000000));
        ASSERT_EQUALS((std::vector<std::string>{identName}), engine->getDroppedIdentNames());
        ASSERT_FALSE(engine->droppedIdents.front().schemaEpoch);
        engine->droppedIdents.clear();
    }

    {
        KVDropPendingIdentReaper reaper(engine);
        const std::string identName("i-drop-immediately");
        reaper.addDropPendingIdent(StorageEngine::Immediate{}, std::make_shared<Ident>(identName));
        EXPECT_CALL(*_opObserverMock, onReplicatedIdentDrop(_, _, _)).Times(0);
        reaper.dropIdentsOlderThan(opCtx.get(), makeTimestamps(11));
        ASSERT_EQUALS((std::vector<std::string>{identName}), engine->getDroppedIdentNames());
        ASSERT_FALSE(engine->droppedIdents.front().schemaEpoch);
    }
}

TEST_F(KVDropPendingIdentReaperTest,
       DropIdentsOlderThan_DSCSecondaryTimestampedDropReapedAfterBecomingPrimary) {
    setUsesSchemaEpochs(true);
    setPrimary(false);

    auto engine = getEngine();
    auto opCtx = makeOpCtx();

    {
        const std::string identName("dscSecondary");
        KVDropPendingIdentReaper reaper(engine);
        dropIdentAtOldest(reaper, Timestamp(10, 0), identName);
        EXPECT_CALL(*_opObserverMock, onReplicatedIdentDrop(_, _, _)).Times(0);
        reaper.dropIdentsOlderThan(opCtx.get(), makeTimestamps(11));

        ASSERT_TRUE(engine->droppedIdents.empty());
        ASSERT_EQUALS((std::set<std::string>{identName}), reaper.getAllIdentNames());
        testing::Mock::VerifyAndClearExpectations(_opObserverMock);

        // Once the same node becomes primary, the pending ident should be reaped.
        setPrimary(true);
        const Timestamp primaryDropOpTime(300, 0);
        const uint64_t expectedSchemaEpoch = 42;
        EXPECT_CALL(*_opObserverMock, onReplicatedIdentDrop(_, identName, _))
            .WillOnce([&](OperationContext*, const std::string&, repl::OpTime& opTime) {
                opTime = repl::OpTime(primaryDropOpTime, repl::OpTime::kUninitializedTerm);
            });
        expectSchemaEpochForTimestamp(primaryDropOpTime, expectedSchemaEpoch);
        reaper.dropIdentsOlderThan(opCtx.get(), makeTimestamps(11));
        ASSERT_EQUALS((std::vector{identName}), engine->getDroppedIdentNames());
        ASSERT(engine->droppedIdents.front().schemaEpoch);
        ASSERT_EQUALS(expectedSchemaEpoch, engine->droppedIdents.front().schemaEpoch.value());
        engine->droppedIdents.clear();
    }
}

TEST_F(KVDropPendingIdentReaperTest, DropIdentsOlderThan_DSCSecondaryDoesOnlyUnreplicatedDrops) {
    setUsesSchemaEpochs(true);
    setPrimary(false);

    auto engine = getEngine();
    auto opCtx = makeOpCtx();
    EXPECT_CALL(*_opObserverMock, onReplicatedIdentDrop(_, _, _)).Times(0);

    {
        KVDropPendingIdentReaper reaper(engine);
        const std::string identName("timestampedSecondary");
        dropIdentAtOldest(reaper, Timestamp(10, 0), identName);
        reaper.dropIdentsOlderThan(opCtx.get(), makeTimestamps(11));
        ASSERT_TRUE(engine->droppedIdents.empty());
        ASSERT_EQUALS((std::set<std::string>{identName}), reaper.getAllIdentNames());
    }

    {
        KVDropPendingIdentReaper reaper(engine);
        const std::string identName("secondaryCheckpointStyle");
        engine->checkpointIteration = StorageEngine::CheckpointIteration{5};
        reaper.addDropPendingIdent(engine->checkpointIteration, std::make_shared<Ident>(identName));
        reaper.dropIdentsOlderThan(opCtx.get(), makeTimestamps(1000000));
        ASSERT_TRUE(engine->droppedIdents.empty());
        engine->checkpointIteration = StorageEngine::CheckpointIteration{6};
        reaper.dropIdentsOlderThan(opCtx.get(), makeTimestamps(1000000));
        ASSERT_EQUALS((std::vector<std::string>{identName}), engine->getDroppedIdentNames());
        ASSERT_FALSE(engine->droppedIdents.front().schemaEpoch);
        engine->droppedIdents.clear();
    }

    {
        KVDropPendingIdentReaper reaper(engine);
        const std::string identName("secondaryImmediate");
        reaper.addDropPendingIdent(StorageEngine::Immediate{}, std::make_shared<Ident>(identName));
        reaper.dropIdentsOlderThan(opCtx.get(), makeTimestamps(11));
        ASSERT_EQUALS((std::vector<std::string>{identName}), engine->getDroppedIdentNames());
        ASSERT_FALSE(engine->droppedIdents.front().schemaEpoch);
    }
}

TEST_F(KVDropPendingIdentReaperTest, ImmediatelyCompletePendingDropWorksAfterInterruptedDrop) {
    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);

    std::string identName = "ident";

    reaper.addDropPendingIdent(StorageEngine::Immediate{}, std::make_shared<Ident>(identName));

    {
        auto opCtx = makeOpCtx();
        opCtx->markKilled();
        ASSERT_THROWS_CODE(
            reaper.dropIdentsOlderThan(opCtx.get(), makeTimestamps(Timestamp::min())),
            DBException,
            ErrorCodes::Interrupted);
        ASSERT_EQUALS(0U, engine->droppedIdents.size());
    }

    {
        auto opCtx = makeOpCtx();
        ASSERT_OK(reaper.immediatelyCompletePendingDrop(opCtx.get(), identName));
        ASSERT_EQUALS(1U, engine->droppedIdents.size());
    }
}

}  // namespace
}  // namespace mongo

