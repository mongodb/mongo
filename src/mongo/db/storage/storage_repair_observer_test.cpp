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

#include <boost/filesystem.hpp>

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/storage_repair_observer.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

static const NamespaceString kConfigNss("local.system.replset");
static const std::string kRepairIncompleteFileName = "_repair_incomplete";

using boost::filesystem::path;

class StorageRepairObserverTest : public ServiceContextMongoDTest {
public:
    StorageRepairObserverTest() {
        repl::ReplicationCoordinator::set(
            getServiceContext(),
            std::make_unique<repl::ReplicationCoordinatorMock>(getServiceContext()));
    }

    void assertRepairIncompleteOnTearDown() {
        _assertRepairIncompleteOnTearDown = true;
    }

    void createMockReplConfig(OperationContext* opCtx) {
        BSONObj replConfig;
        Lock::DBLock dbLock(opCtx, DatabaseName(boost::none, "local"), MODE_X);
        Helpers::putSingleton(
            opCtx, NamespaceString(boost::none, "local.system.replset"), replConfig);
    }

    void assertReplConfigValid(OperationContext* opCtx, bool valid) {
        BSONObj replConfig;
        ASSERT(Helpers::getSingleton(
            opCtx, NamespaceString(boost::none, "local.system.replset"), replConfig));
        if (valid) {
            ASSERT(!replConfig.hasField("repaired"));
        } else {
            ASSERT(replConfig.hasField("repaired"));
        }
    }

    bool hasReplConfig(OperationContext* opCtx) {
        BSONObj replConfig;
        Lock::DBLock dbLock(opCtx, DatabaseName(boost::none, "local"), MODE_IS);
        return Helpers::getSingleton(
            opCtx, NamespaceString(boost::none, "local.system.replset"), replConfig);
    }

    path repairFilePath() {
        return path(storageGlobalParams.dbpath) / path(kRepairIncompleteFileName);
    }

    StorageRepairObserver* reset() {
        StorageRepairObserver::set(
            getServiceContext(),
            std::make_unique<StorageRepairObserver>(storageGlobalParams.dbpath));
        return getRepairObserver();
    }

    StorageRepairObserver* getRepairObserver() {
        return StorageRepairObserver::get(getServiceContext());
    }

    void setUp() {
        ServiceContextMongoDTest::setUp();
        storageGlobalParams.repair = true;
    }

    void tearDown() {
        auto repairObserver = getRepairObserver();
        if (_assertRepairIncompleteOnTearDown) {
            ASSERT(repairObserver->isIncomplete());
        } else {
            ASSERT(!repairObserver->isIncomplete());
        }

        if (repairObserver->isDone() && repairObserver->isDataInvalidated()) {
            LOGV2(22291, "Modifications: ");
            for (const auto& mod : repairObserver->getModifications()) {
                LOGV2(22292,
                      "  {mod_getDescription}",
                      "mod_getDescription"_attr = mod.getDescription());
            }
        }
        storageGlobalParams.repair = false;
    }

private:
    bool _assertRepairIncompleteOnTearDown = false;
};

TEST_F(StorageRepairObserverTest, DataUnmodified) {
    auto repairObserver = getRepairObserver();

    auto repairFile = repairFilePath();
    ASSERT(!boost::filesystem::exists(repairFile));
    ASSERT(!repairObserver->isIncomplete());

    repairObserver->onRepairStarted();

    ASSERT(repairObserver->isIncomplete());
    ASSERT(boost::filesystem::exists(repairFile));

    auto opCtx = cc().makeOperationContext();
    createMockReplConfig(opCtx.get());

    repairObserver->onRepairDone(opCtx.get());
    ASSERT(!repairObserver->isIncomplete());
    ASSERT(!boost::filesystem::exists(repairFile));

    ASSERT(repairObserver->isDone());
    ASSERT(!repairObserver->isDataInvalidated());

    assertReplConfigValid(opCtx.get(), true);
}

TEST_F(StorageRepairObserverTest, DataModified) {
    auto repairObserver = getRepairObserver();

    auto repairFile = repairFilePath();
    ASSERT(!boost::filesystem::exists(repairFile));
    ASSERT(!repairObserver->isIncomplete());

    repairObserver->onRepairStarted();

    ASSERT(repairObserver->isIncomplete());
    ASSERT(boost::filesystem::exists(repairFile));

    std::string mod("Collection mod");
    repairObserver->invalidatingModification(mod);

    auto opCtx = cc().makeOperationContext();
    Lock::GlobalWrite lock(opCtx.get());
    createMockReplConfig(opCtx.get());

    repairObserver->onRepairDone(opCtx.get());
    ASSERT(!repairObserver->isIncomplete());
    ASSERT(!boost::filesystem::exists(repairFile));

    ASSERT(repairObserver->isDone());
    ASSERT(repairObserver->isDataInvalidated());
    ASSERT_EQ(1U, repairObserver->getModifications().size());

    assertReplConfigValid(opCtx.get(), false);
}

TEST_F(StorageRepairObserverTest, DataValidAfterBenignModification) {
    auto repairObserver = getRepairObserver();

    auto repairFile = repairFilePath();
    ASSERT(!boost::filesystem::exists(repairFile));
    ASSERT(!repairObserver->isIncomplete());

    repairObserver->onRepairStarted();

    ASSERT(repairObserver->isIncomplete());
    ASSERT(boost::filesystem::exists(repairFile));

    std::string mod("Collection mod");
    repairObserver->benignModification(mod);

    auto opCtx = cc().makeOperationContext();
    Lock::GlobalWrite lock(opCtx.get());
    createMockReplConfig(opCtx.get());

    repairObserver->onRepairDone(opCtx.get());
    ASSERT(!repairObserver->isIncomplete());
    ASSERT(!boost::filesystem::exists(repairFile));

    ASSERT(repairObserver->isDone());
    ASSERT(!repairObserver->isDataInvalidated());
    ASSERT_EQ(1U, repairObserver->getModifications().size());

    assertReplConfigValid(opCtx.get(), true);
}

TEST_F(StorageRepairObserverTest, DataModifiedDoesNotCreateReplConfigOnStandalone) {
    auto repairObserver = getRepairObserver();

    auto repairFile = repairFilePath();
    ASSERT(!boost::filesystem::exists(repairFile));
    ASSERT(!repairObserver->isIncomplete());

    repairObserver->onRepairStarted();

    ASSERT(repairObserver->isIncomplete());
    ASSERT(boost::filesystem::exists(repairFile));

    std::string mod("Collection mod");
    repairObserver->invalidatingModification(mod);

    auto opCtx = cc().makeOperationContext();
    Lock::GlobalWrite lock(opCtx.get());

    repairObserver->onRepairDone(opCtx.get());
    ASSERT(!repairObserver->isIncomplete());
    ASSERT(!boost::filesystem::exists(repairFile));

    ASSERT(repairObserver->isDone());
    ASSERT(repairObserver->isDataInvalidated());
    ASSERT_EQ(1U, repairObserver->getModifications().size());
    ASSERT(!hasReplConfig(opCtx.get()));
}

TEST_F(StorageRepairObserverTest, RepairIsIncompleteOnFailure) {
    auto repairObserver = getRepairObserver();

    auto repairFile = repairFilePath();
    ASSERT(!boost::filesystem::exists(repairFile));
    ASSERT(!repairObserver->isIncomplete());

    repairObserver->onRepairStarted();

    ASSERT(repairObserver->isIncomplete());
    ASSERT(boost::filesystem::exists(repairFile));

    // Assert that a failure to call onRepairDone does not remove the failure file.
    assertRepairIncompleteOnTearDown();
}

TEST_F(StorageRepairObserverTest, RepairIncompleteAfterRestart) {
    auto repairObserver = getRepairObserver();
    ASSERT(!repairObserver->isIncomplete());
    repairObserver->onRepairStarted();
    ASSERT(repairObserver->isIncomplete());

    repairObserver = reset();
    ASSERT(repairObserver->isIncomplete());


    // Assert that a failure to call onRepairStarted does not create the failure file.
    assertRepairIncompleteOnTearDown();
}

TEST_F(StorageRepairObserverTest, RepairCompleteAfterRestart) {
    auto repairObserver = getRepairObserver();
    ASSERT(!repairObserver->isIncomplete());
    repairObserver->onRepairStarted();
    ASSERT(repairObserver->isIncomplete());

    std::string mod("Collection mod");
    repairObserver->invalidatingModification(mod);

    auto opCtx = cc().makeOperationContext();
    Lock::GlobalWrite lock(opCtx.get());
    createMockReplConfig(opCtx.get());

    repairObserver->onRepairDone(opCtx.get());
    ASSERT(repairObserver->isDone());
    ASSERT_EQ(1U, repairObserver->getModifications().size());

    repairObserver = reset();
    ASSERT(!repairObserver->isIncomplete());
    // Done is reservered for completed operations.
    ASSERT(!repairObserver->isDone());
    assertReplConfigValid(opCtx.get(), false);
}

DEATH_TEST_F(StorageRepairObserverTest, FailsWhenDoneCalledFirst, "Invariant failure") {
    auto repairObserver = getRepairObserver();
    ASSERT(!repairObserver->isIncomplete());

    auto opCtx = cc().makeOperationContext();
    createMockReplConfig(opCtx.get());
    repairObserver->onRepairDone(opCtx.get());
}

DEATH_TEST_F(StorageRepairObserverTest, FailsWhenStartedCalledAfterDone, "Invariant failure") {
    auto repairObserver = getRepairObserver();
    ASSERT(!repairObserver->isIncomplete());
    repairObserver->onRepairStarted();
    ASSERT(repairObserver->isIncomplete());

    auto opCtx = cc().makeOperationContext();
    createMockReplConfig(opCtx.get());
    repairObserver->onRepairDone(opCtx.get());
    ASSERT(repairObserver->isDone());
    assertReplConfigValid(opCtx.get(), true);

    repairObserver->onRepairStarted();
}
}  // namespace
}  // namespace mongo
