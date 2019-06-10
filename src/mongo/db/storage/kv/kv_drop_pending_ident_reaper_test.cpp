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

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/kv/kv_drop_pending_ident_reaper.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

/**
 * Since dropIdents() acquires the global intent lock, which is not supported by the default locker,
 * we swap in a different locker implementation to make the tests pass.
 */
class ReaperTestClientObserver final : public ServiceContext::ClientObserver {
public:
    void onCreateClient(Client* client) override{};
    void onDestroyClient(Client* client) override{};
    void onCreateOperationContext(OperationContext* opCtx) override {
        opCtx->setLockState(std::make_unique<LockerImpl>());
    }
    void onDestroyOperationContext(OperationContext* opCtx) override {}
};

/**
 * Test-only implementation of KVEngine that tracks idents that have been dropped.
 */
class KVEngineMock : public KVEngine {
public:
    Status dropIdent(OperationContext* opCtx, StringData ident) override;

    // Unused KVEngine functions below.
    RecoveryUnit* newRecoveryUnit() override {
        return nullptr;
    }
    std::unique_ptr<RecordStore> getRecordStore(OperationContext* opCtx,
                                                StringData ns,
                                                StringData ident,
                                                const CollectionOptions& options) override {
        return {};
    }
    SortedDataInterface* getSortedDataInterface(OperationContext* opCtx,
                                                StringData ident,
                                                const IndexDescriptor* desc) override {
        return nullptr;
    }
    Status createRecordStore(OperationContext* opCtx,
                             StringData ns,
                             StringData ident,
                             const CollectionOptions& options) override {
        return Status::OK();
    }
    std::unique_ptr<RecordStore> makeTemporaryRecordStore(OperationContext* opCtx,
                                                          StringData ident) override {
        return {};
    }
    Status createSortedDataInterface(OperationContext* opCtx,
                                     StringData ident,
                                     const IndexDescriptor* desc) override {
        return Status::OK();
    }
    int64_t getIdentSize(OperationContext* opCtx, StringData ident) override {
        return 0;
    }
    Status repairIdent(OperationContext* opCtx, StringData ident) override {
        return Status::OK();
    }
    bool isDurable() const override {
        return false;
    }
    bool isEphemeral() const override {
        return false;
    }
    bool supportsDocLocking() const override {
        return false;
    }
    bool supportsDirectoryPerDB() const override {
        return false;
    }
    bool hasIdent(OperationContext* opCtx, StringData ident) const override {
        return false;
    }
    std::vector<std::string> getAllIdents(OperationContext* opCtx) const override {
        return {};
    }
    void cleanShutdown() override {}
    void setJournalListener(JournalListener* jl) override {}
    Timestamp getAllCommittedTimestamp() const override {
        return {};
    }
    Timestamp getOldestOpenReadTimestamp() const override {
        return {};
    }

    boost::optional<Timestamp> getOplogNeededForCrashRecovery() const override {
        return boost::none;
    }

    // List of idents removed using dropIdent().
    std::vector<std::string> droppedIdents;

    // Override to modify dropIdent() behavior.
    using DropIdentFn = std::function<Status(OperationContext*, StringData)>;
    DropIdentFn dropIdentFn = [](OperationContext*, StringData) { return Status::OK(); };
};

Status KVEngineMock::dropIdent(OperationContext* opCtx, StringData ident) {
    auto status = dropIdentFn(opCtx, ident);
    if (status.isOK()) {
        droppedIdents.push_back(ident.toString());
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
    auto service = getServiceContext();
    service->registerClientObserver(std::make_unique<ReaperTestClientObserver>());
    _engineMock = stdx::make_unique<KVEngineMock>();
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

TEST_F(KVDropPendingIdentReaperTest, GetEarliestDropTimestampReturnsBoostNoneOnEmptyIdents) {
    KVDropPendingIdentReaper reaper(nullptr);
    ASSERT_FALSE(reaper.getEarliestDropTimestamp());
}

TEST_F(KVDropPendingIdentReaperTest, AddDropPendingIdentAcceptsNullDropTimestamp) {
    Timestamp nullDropTimestamp;
    NamespaceString nss("test.foo");
    constexpr auto ident = "myident"_sd;
    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);
    reaper.addDropPendingIdent(nullDropTimestamp, nss, ident);
    ASSERT_EQUALS(nullDropTimestamp, *reaper.getEarliestDropTimestamp());

    auto opCtx = makeOpCtx();
    reaper.dropIdentsOlderThan(opCtx.get(), {Seconds(100), 0});
    ASSERT_EQUALS(1U, engine->droppedIdents.size());
    ASSERT_EQUALS(ident, engine->droppedIdents.front());
}

TEST_F(KVDropPendingIdentReaperTest,
       AddDropPendingIdentWithDuplicateDropTimestampButDifferentIdent) {
    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);

    Timestamp dropTimestamp{Seconds(100), 0};
    NamespaceString nss1("test.foo");
    constexpr auto ident1 = "ident1"_sd;
    NamespaceString nss2("test.bar");
    constexpr auto ident2 = "ident2"_sd;
    reaper.addDropPendingIdent(dropTimestamp, nss1, ident1);
    reaper.addDropPendingIdent(dropTimestamp, nss2, ident2);

    // getAllIdents() returns a set of drop-pending idents known to the reaper.
    auto dropPendingIdents = reaper.getAllIdents();
    ASSERT_EQUALS(2U, dropPendingIdents.size());
    ASSERT(dropPendingIdents.find(ident1.toString()) != dropPendingIdents.cend());
    ASSERT(dropPendingIdents.find(ident2.toString()) != dropPendingIdents.cend());

    // Check earliest drop timestamp.
    ASSERT_EQUALS(dropTimestamp, *reaper.getEarliestDropTimestamp());

    // This should have no effect.
    auto opCtx = makeOpCtx();
    reaper.dropIdentsOlderThan(opCtx.get(), dropTimestamp);
    ASSERT_EQUALS(0U, engine->droppedIdents.size());

    // Drop all idents managed by reaper and confirm number of drops.
    reaper.dropIdentsOlderThan(opCtx.get(), makeTimestampWithNextInc(dropTimestamp));
    ASSERT_EQUALS(2U, engine->droppedIdents.size());
    ASSERT_EQUALS(ident1, engine->droppedIdents.front());
    ASSERT_EQUALS(ident2, engine->droppedIdents.back());
}

DEATH_TEST_F(KVDropPendingIdentReaperTest,
             AddDropPendingIdentTerminatesOnDuplicateDropTimestampAndIdent,
             "Failed to add drop-pending ident") {
    Timestamp dropTimestamp{Seconds(100), 0};
    NamespaceString nss("test.foo");
    constexpr auto ident = "myident"_sd;
    KVDropPendingIdentReaper reaper(getEngine());
    reaper.addDropPendingIdent(dropTimestamp, nss, ident);
    reaper.addDropPendingIdent(dropTimestamp, nss, ident);
}

TEST_F(KVDropPendingIdentReaperTest,
       DropIdentsOlderThanDropsIdentsWithDropTimestampsBeforeOldestTimestamp) {
    auto opCtx = makeOpCtx();

    // Generate timestamps with secs: 10, 20, ..., 50.
    const int n = 5U;
    Timestamp ts[n];
    NamespaceString nss[n];
    std::string ident[n];
    for (int i = 0; i < n; ++i) {
        ts[i] = {Seconds((i + 1) * 10), 0};
        nss[i] = NamespaceString("test", str::stream() << "coll" << i);
        ident[i] = str::stream() << "ident" << i;
    }

    // Add drop-pending ident with drop timestamp out of order and check that
    // getEarliestDropOpTime() returns earliest timestamp.
    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);
    ASSERT_FALSE(reaper.getEarliestDropTimestamp());
    reaper.addDropPendingIdent(ts[1], nss[1], ident[1]);
    reaper.addDropPendingIdent(ts[0], nss[0], ident[0]);
    reaper.addDropPendingIdent(ts[2], nss[2], ident[2]);
    reaper.addDropPendingIdent(ts[3], nss[3], ident[3]);
    reaper.addDropPendingIdent(ts[4], nss[4], ident[4]);
    ASSERT_EQUALS(ts[0], *reaper.getEarliestDropTimestamp());

    // Committed optime before first drop optime has no effect.
    reaper.dropIdentsOlderThan(opCtx.get(), {Seconds(5), 0});
    ASSERT_EQUALS(ts[0], *reaper.getEarliestDropTimestamp());

    // Committed optime matching second drop optime will result in the first two drop-pending
    // collections being removed.
    reaper.dropIdentsOlderThan(opCtx.get(), makeTimestampWithNextInc(ts[1]));
    ASSERT_EQUALS(ts[2], *reaper.getEarliestDropTimestamp());
    ASSERT_EQUALS(2U, engine->droppedIdents.size());
    ASSERT_EQUALS(ident[0], engine->droppedIdents[0]);
    ASSERT_EQUALS(ident[1], engine->droppedIdents[1]);

    // Committed optime between third and fourth optimes will result in the third collection being
    // removed.
    reaper.dropIdentsOlderThan(opCtx.get(), {Seconds(35), 0});
    ASSERT_EQUALS(ts[3], *reaper.getEarliestDropTimestamp());
    ASSERT_EQUALS(3U, engine->droppedIdents.size());
    ASSERT_EQUALS(ident[2], engine->droppedIdents[2]);

    // Committed optime after last optime will result in all drop-pending collections being removed.
    reaper.dropIdentsOlderThan(opCtx.get(), {Seconds(100), 0});
    ASSERT_FALSE(reaper.getEarliestDropTimestamp());
    ASSERT_EQUALS(5U, engine->droppedIdents.size());
    ASSERT_EQUALS(ident[3], engine->droppedIdents[3]);
    ASSERT_EQUALS(ident[4], engine->droppedIdents[4]);
}

DEATH_TEST_F(KVDropPendingIdentReaperTest,
             DropIdentsOlderTerminatesIfKVEngineFailsToDropIdent,
             "Failed to remove drop-pending ident") {
    Timestamp dropTimestamp{Seconds{1}, 0};
    NamespaceString nss("test.foo");
    constexpr auto ident = "myident"_sd;

    auto engine = getEngine();
    KVDropPendingIdentReaper reaper(engine);
    reaper.addDropPendingIdent(dropTimestamp, nss, ident);
    ASSERT_EQUALS(dropTimestamp, *reaper.getEarliestDropTimestamp());

    // Make KVEngineMock::dropIndent() fail.
    engine->dropIdentFn = [ident](OperationContext* opCtx, StringData identToDrop) {
        ASSERT(opCtx);
        ASSERT_EQUALS(ident, identToDrop);
        return Status(ErrorCodes::OperationFailed, "Mock KV engine dropIndent() failed.");
    };

    auto opCtx = makeOpCtx();
    reaper.dropIdentsOlderThan(opCtx.get(), makeTimestampWithNextInc(dropTimestamp));
}

}  // namespace
}  // namespace mongo
