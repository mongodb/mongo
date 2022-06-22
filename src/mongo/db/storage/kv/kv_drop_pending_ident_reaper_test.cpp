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
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/kv/kv_drop_pending_ident_reaper.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

/**
 * Test-only implementation of KVEngine that tracks idents that have been dropped.
 */
class KVEngineMock : public KVEngine {
public:
    Status dropIdent(RecoveryUnit* ru,
                     StringData ident,
                     StorageEngine::DropIdentCallback&& onDrop) override;

    void dropIdentForImport(OperationContext* opCtx, StringData ident) override {}

    // Unused KVEngine functions below.
    RecoveryUnit* newRecoveryUnit() override {
        return nullptr;
    }
    std::unique_ptr<RecordStore> getRecordStore(OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                StringData ident,
                                                const CollectionOptions& options) override {
        return {};
    }
    std::unique_ptr<SortedDataInterface> getSortedDataInterface(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const CollectionOptions& collOptions,
        StringData ident,
        const IndexDescriptor* desc) override {
        return nullptr;
    }

    Status createColumnStore(OperationContext* opCtx,
                             const NamespaceString& ns,
                             const CollectionOptions& collOptions,
                             StringData ident,
                             const IndexDescriptor* desc) override {
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<ColumnStore> getColumnStore(OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                const CollectionOptions& collOptions,
                                                StringData ident,
                                                const IndexDescriptor*) override {
        return nullptr;
    }

    Status createRecordStore(OperationContext* opCtx,
                             const NamespaceString& nss,
                             StringData ident,
                             const CollectionOptions& options,
                             KeyFormat keyFormat) override {
        return Status::OK();
    }
    std::unique_ptr<RecordStore> makeTemporaryRecordStore(OperationContext* opCtx,
                                                          StringData ident,
                                                          KeyFormat keyFormat) override {
        return {};
    }
    Status createSortedDataInterface(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     const CollectionOptions& collOptions,
                                     StringData ident,
                                     const IndexDescriptor* desc) override {
        return Status::OK();
    }
    Status dropSortedDataInterface(OperationContext* opCtx, StringData ident) override {
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
    Timestamp getAllDurableTimestamp() const override {
        return {};
    }

    boost::optional<Timestamp> getOplogNeededForCrashRecovery() const override {
        return boost::none;
    }

    Timestamp getOldestTimestamp() const override {
        return Timestamp();
    }

    boost::optional<Timestamp> getRecoveryTimestamp() const {
        return boost::none;
    }

    void setPinnedOplogTimestamp(const Timestamp& pinnedTimestamp) {}

    void dump() const override {}

    // List of ident names removed using dropIdent().
    std::vector<std::string> droppedIdents;

    // Override to modify dropIdent() behavior.
    using DropIdentFn = std::function<Status(RecoveryUnit*, StringData)>;
    DropIdentFn dropIdentFn = [](RecoveryUnit*, StringData) { return Status::OK(); };
};

Status KVEngineMock::dropIdent(RecoveryUnit* ru,
                               StringData ident,
                               StorageEngine::DropIdentCallback&& onDrop) {
    auto status = dropIdentFn(ru, ident);
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
    ASSERT_EQUALS(2U, dropPendingIdents.size());
    ASSERT(dropPendingIdents.find(identName1) != dropPendingIdents.cend());
    ASSERT(dropPendingIdents.find(identName2) != dropPendingIdents.cend());

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

DEATH_TEST_F(KVDropPendingIdentReaperTest,
             AddDropPendingIdentTerminatesOnDuplicateDropTimestampAndIdent,
             "Failed to add drop-pending ident") {
    Timestamp dropTimestamp{Seconds(100), 0};

    KVDropPendingIdentReaper reaper(getEngine());

    {
        std::shared_ptr<Ident> ident = std::make_shared<Ident>("myident");
        reaper.addDropPendingIdent(dropTimestamp, ident);
        reaper.addDropPendingIdent(dropTimestamp, ident);
    }
}

TEST_F(KVDropPendingIdentReaperTest,
       DropIdentsOlderThanDropsIdentsWithDropTimestampsBeforeOldestTimestamp) {
    auto opCtx = makeOpCtx();

    // Generate timestamps with secs: 10, 20, ..., 50.
    const int n = 5U;
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
    {
        // The reaper must have the only references to the idents before it will drop them.
        std::shared_ptr<Ident> ident[n];
        for (int i = 0; i < n; ++i) {
            ident[i] = std::make_shared<Ident>(identName[i]);
        }

        reaper.addDropPendingIdent(ts[1], ident[1]);
        reaper.addDropPendingIdent(ts[0], ident[0]);
        reaper.addDropPendingIdent(ts[2], ident[2]);
        reaper.addDropPendingIdent(ts[3], ident[3]);
        reaper.addDropPendingIdent(ts[4], ident[4]);
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

    // Committed optime between third and fourth optimes will result in the third collection being
    // removed.
    reaper.dropIdentsOlderThan(opCtx.get(), {Seconds(35), 0});
    ASSERT_EQUALS(ts[3], *reaper.getEarliestDropTimestamp());
    ASSERT_EQUALS(3U, engine->droppedIdents.size());
    ASSERT_EQUALS(identName[2], engine->droppedIdents[2]);

    // Committed optime after last optime will result in all drop-pending collections being removed.
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
        // after giving them to the reaper. The latter 2 should be droppable; the former 2 should
        // not be droppable.
        std::shared_ptr<Ident> ident0 = std::make_shared<Ident>(identNames[0]);
        std::shared_ptr<Ident> ident1 = std::make_shared<Ident>(identNames[1]);
        {
            std::shared_ptr<Ident> ident2 = std::make_shared<Ident>(identNames[2]);
            std::shared_ptr<Ident> ident3 = std::make_shared<Ident>(identNames[3]);
            // The reaper must have the only references to the idents before it will drop them.

            reaper.addDropPendingIdent(dropTimestamp, ident0);
            reaper.addDropPendingIdent(dropTimestamp, ident1);
            reaper.addDropPendingIdent(dropTimestamp, ident2);
            reaper.addDropPendingIdent(dropTimestamp, ident3);
        }

        // All the idents have dropTimestamps old enough to drop, but only ident2 and ident3 should
        // be dropped because ident0 and ident1 are still referenced.
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
    engine->dropIdentFn = [&identName](RecoveryUnit* ru, StringData identToDropName) {
        ASSERT(ru);
        ASSERT_EQUALS(identName, identToDropName);
        return Status(ErrorCodes::OperationFailed, "Mock KV engine dropIndent() failed.");
    };

    auto opCtx = makeOpCtx();
    reaper.dropIdentsOlderThan(opCtx.get(), makeTimestampWithNextInc(dropTimestamp));
}

}  // namespace
}  // namespace mongo
