/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
