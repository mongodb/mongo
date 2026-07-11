// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands/set_feature_compatibility_version_steps/fcv_step.h"

#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/version/releases.h"


namespace mongo {
namespace {


template <class ActualStep>
class TestStep : public FCVStep {
public:
    using FCV = FCVStep::FCV;
    int numCallsPrepareToUpgradeActionsBeforeGlobalLock{0};
    int numCallsUserCollectionsUassertsForUpgrade{0};
    int numCallsUserCollectionsWorkForUpgrade{0};
    int numCallsUpgradeServerMetadata{0};
    int numCallsFinalizeUpgrade{0};
    int numCallsBeforeStartWithFCVLock{0};
    int numCallsBeforeStartWithoutFCVLock{0};
    int numCallsPrepareToDowngradeActions{0};
    int numCallsDrainingOnDowngrade{0};
    int numCallsUserCollectionsUassertsForDowngrade{0};
    int numCallsInternalServerCleanupForDowngrade{0};
    int numCallsFinalizeDowngrade{0};

protected:
    void prepareToUpgradeActionsBeforeGlobalLock(OperationContext* opCtx,
                                                 FCV originalVersion,
                                                 FCV requestedVersion) override {
        numCallsPrepareToUpgradeActionsBeforeGlobalLock++;
    }

    void userCollectionsUassertsForUpgrade(OperationContext* opCtx,
                                           FCV originalVersion,
                                           FCV requestedVersion) override {
        numCallsUserCollectionsUassertsForUpgrade++;
    }

    void userCollectionsWorkForUpgrade(OperationContext* opCtx,
                                       FCV originalVersion,
                                       FCV requestedVersion) override {
        numCallsUserCollectionsWorkForUpgrade++;
    }

    void upgradeServerMetadata(OperationContext* opCtx,
                               FCV originalVersion,
                               FCV requestedVersion) override {
        numCallsUpgradeServerMetadata++;
    }

    void finalizeUpgrade(OperationContext* optCtx, FCV requestedVersion) override {
        numCallsFinalizeUpgrade++;
    }

    void beforeStartWithoutFCVLock(OperationContext* opCtx,
                                   FCV originalVersion,
                                   FCV requestedVersion) override {
        numCallsBeforeStartWithoutFCVLock++;
    }

    void beforeStartWithFCVLock(OperationContext* opCtx,
                                FCV originalVersion,
                                FCV requestedVersion) override {
        numCallsBeforeStartWithFCVLock++;
    }

    void drainingOnDowngrade(OperationContext* opCtx,
                             FCV originalVersion,
                             FCV requestedVersion) override {
        numCallsDrainingOnDowngrade++;
    }

    void prepareToDowngradeActions(OperationContext* opCtx,
                                   FCV originalVersion,
                                   FCV requestedVersion) override {
        numCallsPrepareToDowngradeActions++;
    }

    void userCollectionsUassertsForDowngrade(OperationContext* opCtx,
                                             FCV originalVersion,
                                             FCV requestedVersion) override {
        numCallsUserCollectionsUassertsForDowngrade++;
    }

    void internalServerCleanupForDowngrade(OperationContext* opCtx,
                                           FCV originalVersion,
                                           FCV requestedVersion) override {
        numCallsInternalServerCleanupForDowngrade++;
    }

    void finalizeDowngrade(OperationContext* opCtx, FCV requestedVersion) override {
        numCallsFinalizeDowngrade++;
    }
};

/**
 * An FCV Step which is never executed
 */

class FCVStepNeverExecutes : public TestStep<FCVStepNeverExecutes> {
public:
    static FCVStepNeverExecutes* get(ServiceContext* serviceContext);
    bool shouldRegisterFCVStep() const final {
        return false;
    }

private:
    std::string getStepName() const final {
        return "FCVStepNeverExecutes";
    }
};


const auto getFCVStepNeverExecutes = ServiceContext::declareDecoration<FCVStepNeverExecutes>();

FCVStepRegistry::Registerer<FCVStepNeverExecutes> fcvStepNeverExecutesRegisterer(
    "FCVStepNeverExecutes");

FCVStepNeverExecutes* FCVStepNeverExecutes::get(ServiceContext* serviceContext) {
    return &getFCVStepNeverExecutes(serviceContext);
}

/**
 * An FCV Step which is always executed
 */

class FCVStepAlwaysExecutes : public TestStep<FCVStepAlwaysExecutes> {
public:
    static FCVStepAlwaysExecutes* get(ServiceContext* serviceContext);
    bool shouldRegisterFCVStep() const final {
        return true;
    }

private:
    std::string getStepName() const final {
        return "FCVStepAlwaysExecutes";
    }
};


const auto getFCVStepAlwaysExecutes = ServiceContext::declareDecoration<FCVStepAlwaysExecutes>();

FCVStepRegistry::Registerer<FCVStepAlwaysExecutes> fcvStepAlwaysExecutesRegister(
    "FCVStepAlwaysExecutes");

FCVStepAlwaysExecutes* FCVStepAlwaysExecutes::get(ServiceContext* serviceContext) {
    return &getFCVStepAlwaysExecutes(serviceContext);
}


class FCVStepRegistryTest : public ServiceContextTest {};

TEST_F(FCVStepRegistryTest, FCVStepRegistrySimple) {
    auto sc = getGlobalServiceContext();
    auto opCtxHolder = makeOperationContext();
    auto opCtx = opCtxHolder.get();

    auto a = FCVStepNeverExecutes::get(sc);
    auto b = FCVStepAlwaysExecutes::get(sc);

    auto originalVersion = FCVStep::FCV::kVersion_8_0;
    auto requestedVersion = FCVStep::FCV::kVersion_9_0;

    ASSERT_EQ(0, a->numCallsBeforeStartWithoutFCVLock);
    ASSERT_EQ(0, a->numCallsBeforeStartWithFCVLock);
    ASSERT_EQ(0, a->numCallsPrepareToUpgradeActionsBeforeGlobalLock);
    ASSERT_EQ(0, a->numCallsUserCollectionsUassertsForDowngrade);
    ASSERT_EQ(0, a->numCallsUserCollectionsWorkForUpgrade);
    ASSERT_EQ(0, a->numCallsUpgradeServerMetadata);
    ASSERT_EQ(0, a->numCallsFinalizeUpgrade);
    ASSERT_EQ(0, a->numCallsPrepareToDowngradeActions);
    ASSERT_EQ(0, a->numCallsDrainingOnDowngrade);
    ASSERT_EQ(0, a->numCallsUserCollectionsUassertsForDowngrade);
    ASSERT_EQ(0, a->numCallsInternalServerCleanupForDowngrade);
    ASSERT_EQ(0, a->numCallsFinalizeDowngrade);

    ASSERT_EQ(0, b->numCallsBeforeStartWithoutFCVLock);
    ASSERT_EQ(0, b->numCallsBeforeStartWithFCVLock);
    ASSERT_EQ(0, b->numCallsPrepareToUpgradeActionsBeforeGlobalLock);
    ASSERT_EQ(0, b->numCallsUserCollectionsUassertsForDowngrade);
    ASSERT_EQ(0, b->numCallsUserCollectionsWorkForUpgrade);
    ASSERT_EQ(0, b->numCallsUpgradeServerMetadata);
    ASSERT_EQ(0, b->numCallsFinalizeUpgrade);
    ASSERT_EQ(0, b->numCallsPrepareToDowngradeActions);
    ASSERT_EQ(0, b->numCallsDrainingOnDowngrade);
    ASSERT_EQ(0, b->numCallsUserCollectionsUassertsForDowngrade);
    ASSERT_EQ(0, b->numCallsInternalServerCleanupForDowngrade);
    ASSERT_EQ(0, b->numCallsFinalizeDowngrade);

    FCVStepRegistry::get(sc).beforeStartWithoutFCVLock(opCtx, originalVersion, requestedVersion);
    FCVStepRegistry::get(sc).beforeStartWithFCVLock(opCtx, originalVersion, requestedVersion);
    FCVStepRegistry::get(sc).prepareToUpgradeActionsBeforeGlobalLock(
        opCtx, originalVersion, requestedVersion);
    FCVStepRegistry::get(sc).userCollectionsUassertsForUpgrade(
        opCtx, originalVersion, requestedVersion);
    FCVStepRegistry::get(sc).userCollectionsWorkForUpgrade(
        opCtx, originalVersion, requestedVersion);
    FCVStepRegistry::get(sc).upgradeServerMetadata(opCtx, originalVersion, requestedVersion);
    FCVStepRegistry::get(sc).finalizeUpgrade(opCtx, requestedVersion);
    FCVStepRegistry::get(sc).prepareToDowngradeActions(opCtx, originalVersion, requestedVersion);
    FCVStepRegistry::get(sc).drainingOnDowngrade(opCtx, originalVersion, requestedVersion);
    FCVStepRegistry::get(sc).userCollectionsUassertsForDowngrade(
        opCtx, originalVersion, requestedVersion);
    FCVStepRegistry::get(sc).internalServerCleanupForDowngrade(
        opCtx, originalVersion, requestedVersion);
    FCVStepRegistry::get(sc).finalizeDowngrade(opCtx, requestedVersion);


    ASSERT_EQ(0, a->numCallsBeforeStartWithoutFCVLock);
    ASSERT_EQ(0, a->numCallsBeforeStartWithFCVLock);
    ASSERT_EQ(0, a->numCallsPrepareToUpgradeActionsBeforeGlobalLock);
    ASSERT_EQ(0, a->numCallsUserCollectionsUassertsForDowngrade);
    ASSERT_EQ(0, a->numCallsUserCollectionsWorkForUpgrade);
    ASSERT_EQ(0, a->numCallsUpgradeServerMetadata);
    ASSERT_EQ(0, a->numCallsFinalizeUpgrade);
    ASSERT_EQ(0, a->numCallsPrepareToDowngradeActions);
    ASSERT_EQ(0, a->numCallsDrainingOnDowngrade);
    ASSERT_EQ(0, a->numCallsUserCollectionsUassertsForDowngrade);
    ASSERT_EQ(0, a->numCallsInternalServerCleanupForDowngrade);
    ASSERT_EQ(0, a->numCallsFinalizeDowngrade);

    ASSERT_EQ(1, b->numCallsBeforeStartWithoutFCVLock);
    ASSERT_EQ(1, b->numCallsBeforeStartWithFCVLock);
    ASSERT_EQ(1, b->numCallsPrepareToUpgradeActionsBeforeGlobalLock);
    ASSERT_EQ(1, b->numCallsUserCollectionsUassertsForDowngrade);
    ASSERT_EQ(1, b->numCallsUserCollectionsWorkForUpgrade);
    ASSERT_EQ(1, b->numCallsUpgradeServerMetadata);
    ASSERT_EQ(1, b->numCallsFinalizeUpgrade);
    ASSERT_EQ(1, b->numCallsPrepareToDowngradeActions);
    ASSERT_EQ(1, b->numCallsDrainingOnDowngrade);
    ASSERT_EQ(1, b->numCallsUserCollectionsUassertsForDowngrade);
    ASSERT_EQ(1, b->numCallsInternalServerCleanupForDowngrade);
    ASSERT_EQ(1, b->numCallsFinalizeDowngrade);
}
}  // namespace
}  // namespace mongo
