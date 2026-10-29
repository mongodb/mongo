// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/storage_repair_observer.h"

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

static const std::string kRepairIncompleteFileName = "_repair_incomplete";

using boost::filesystem::path;

class StorageRepairObserverTest : public ServiceContextMongoDTest {
public:
    void assertRepairIncompleteOnTearDown() {
        _assertRepairIncompleteOnTearDown = true;
    }

    void createMockReplConfig() {
        _mockReplConfigValid = true;
    }

    void assertReplConfigValid(bool valid) {
        ASSERT(hasReplConfig() && _mockReplConfigValid == valid);
    }

    bool hasReplConfig() {
        return _mockReplConfigValid.has_value();
    }

    void invalidateReplConfig() {
        if (hasReplConfig()) {
            _mockReplConfigValid = false;
        }
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

    void setUp() override {
        ServiceContextMongoDTest::setUp();
        storageGlobalParams.repair = true;
    }

    void tearDown() override {
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
    boost::optional<bool> _mockReplConfigValid;
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
    createMockReplConfig();

    repairObserver->onRepairDone(opCtx.get(), [this]() { invalidateReplConfig(); });
    ASSERT(!repairObserver->isIncomplete());
    ASSERT(!boost::filesystem::exists(repairFile));

    ASSERT(repairObserver->isDone());
    ASSERT(!repairObserver->isDataInvalidated());

    assertReplConfigValid(true);
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
    createMockReplConfig();

    repairObserver->onRepairDone(opCtx.get(), [this]() { invalidateReplConfig(); });
    ASSERT(!repairObserver->isIncomplete());
    ASSERT(!boost::filesystem::exists(repairFile));

    ASSERT(repairObserver->isDone());
    ASSERT(repairObserver->isDataInvalidated());
    EXPECT_EQ(1U, repairObserver->getModifications().size());

    assertReplConfigValid(false);
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
    createMockReplConfig();

    repairObserver->onRepairDone(opCtx.get(), [this]() { invalidateReplConfig(); });
    ASSERT(!repairObserver->isIncomplete());
    ASSERT(!boost::filesystem::exists(repairFile));

    ASSERT(repairObserver->isDone());
    ASSERT(!repairObserver->isDataInvalidated());
    EXPECT_EQ(1U, repairObserver->getModifications().size());

    assertReplConfigValid(true);
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

    repairObserver->onRepairDone(opCtx.get(), [this]() { invalidateReplConfig(); });
    ASSERT(!repairObserver->isIncomplete());
    ASSERT(!boost::filesystem::exists(repairFile));

    ASSERT(repairObserver->isDone());
    ASSERT(repairObserver->isDataInvalidated());
    EXPECT_EQ(1U, repairObserver->getModifications().size());
    ASSERT(!hasReplConfig());
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
    createMockReplConfig();

    repairObserver->onRepairDone(opCtx.get(), [this]() { invalidateReplConfig(); });
    ASSERT(repairObserver->isDone());
    EXPECT_EQ(1U, repairObserver->getModifications().size());

    repairObserver = reset();
    ASSERT(!repairObserver->isIncomplete());
    // Done is reserved for completed operations.
    ASSERT(!repairObserver->isDone());
    assertReplConfigValid(false);
}

using StorageRepairObserverTestDeathTest = StorageRepairObserverTest;
DEATH_TEST_F(StorageRepairObserverTestDeathTest, FailsWhenDoneCalledFirst, "Invariant failure") {
    auto repairObserver = getRepairObserver();
    ASSERT(!repairObserver->isIncomplete());

    auto opCtx = cc().makeOperationContext();
    createMockReplConfig();
    repairObserver->onRepairDone(opCtx.get(), [this]() { invalidateReplConfig(); });
}

DEATH_TEST_F(StorageRepairObserverTestDeathTest,
             FailsWhenStartedCalledAfterDone,
             "Invariant failure") {
    auto repairObserver = getRepairObserver();
    ASSERT(!repairObserver->isIncomplete());
    repairObserver->onRepairStarted();
    ASSERT(repairObserver->isIncomplete());

    auto opCtx = cc().makeOperationContext();
    createMockReplConfig();
    repairObserver->onRepairDone(opCtx.get(), [this]() { invalidateReplConfig(); });
    ASSERT(repairObserver->isDone());
    assertReplConfigValid(true);

    repairObserver->onRepairStarted();
}
}  // namespace
}  // namespace mongo
