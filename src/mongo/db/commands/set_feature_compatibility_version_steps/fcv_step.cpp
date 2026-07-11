// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands/set_feature_compatibility_version_steps/fcv_step.h"

#include "mongo/logv2/log.h"

#include <algorithm>
#include <vector>


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {

namespace {

auto fcvStepRegistryDecoration = ServiceContext::declareDecoration<FCVStepRegistry>();

}  // namespace


FCVStepRegistry& FCVStepRegistry::get(ServiceContext* serviceContext) {
    return fcvStepRegistryDecoration(serviceContext);
}

FCVStepRegistry::~FCVStepRegistry() {
    invariant(_steps.empty());
}

void FCVStepRegistry::prepareToUpgradeActionsBeforeGlobalLock(OperationContext* opCtx,
                                                              FCV originalVersion,
                                                              FCV requestedVersion) {

    std::for_each(_steps.begin(), _steps.end(), [&](FCVStep* step) {
        step->prepareToUpgradeActionsBeforeGlobalLock(opCtx, originalVersion, requestedVersion);
    });
}

void FCVStepRegistry::userCollectionsUassertsForUpgrade(OperationContext* opCtx,
                                                        FCV originalVersion,
                                                        FCV requestedVersion) {
    std::for_each(_steps.begin(), _steps.end(), [&](FCVStep* step) {
        step->userCollectionsUassertsForUpgrade(opCtx, originalVersion, requestedVersion);
    });
}

void FCVStepRegistry::userCollectionsWorkForUpgrade(OperationContext* opCtx,
                                                    FCV originalVersion,
                                                    FCV requestedVersion) {
    std::for_each(_steps.begin(), _steps.end(), [&](FCVStep* step) {
        step->userCollectionsWorkForUpgrade(opCtx, originalVersion, requestedVersion);
    });
}


void FCVStepRegistry::upgradeServerMetadata(OperationContext* opCtx,
                                            const FCV originalVersion,
                                            const FCV requestedVersion) {
    std::for_each(_steps.begin(), _steps.end(), [&](FCVStep* step) {
        step->upgradeServerMetadata(opCtx, originalVersion, requestedVersion);
    });
}

void FCVStepRegistry::finalizeUpgrade(OperationContext* opCtx, const FCV requestedVersion) {
    std::for_each(_steps.begin(), _steps.end(), [&](FCVStep* step) {
        step->finalizeUpgrade(opCtx, requestedVersion);
    });
}

void FCVStepRegistry::beforeStartWithoutFCVLock(OperationContext* opCtx,
                                                FCV originalVersion,
                                                FCV requestedVersion) {
    std::for_each(_steps.begin(), _steps.end(), [&](FCVStep* step) {
        step->beforeStartWithoutFCVLock(opCtx, originalVersion, requestedVersion);
    });
}

void FCVStepRegistry::beforeStartWithFCVLock(OperationContext* opCtx,
                                             FCV originalVersion,
                                             FCV requestedVersion) {
    std::for_each(_steps.begin(), _steps.end(), [&](FCVStep* step) {
        step->beforeStartWithFCVLock(opCtx, originalVersion, requestedVersion);
    });
}

void FCVStepRegistry::prepareToDowngradeActions(OperationContext* opCtx,
                                                FCV originalVersion,
                                                FCV requestedVersion) {
    std::for_each(_steps.begin(), _steps.end(), [&](FCVStep* step) {
        step->prepareToDowngradeActions(opCtx, originalVersion, requestedVersion);
    });
}

void FCVStepRegistry::drainingOnDowngrade(OperationContext* opCtx,
                                          FCV originalVersion,
                                          FCV requestedVersion) {
    std::for_each(_steps.begin(), _steps.end(), [&](FCVStep* step) {
        step->drainingOnDowngrade(opCtx, originalVersion, requestedVersion);
    });
}

void FCVStepRegistry::userCollectionsUassertsForDowngrade(OperationContext* opCtx,
                                                          FCV originalVersion,
                                                          FCV requestedVersion) {
    std::for_each(_steps.begin(), _steps.end(), [&](FCVStep* step) {
        step->userCollectionsUassertsForDowngrade(opCtx, originalVersion, requestedVersion);
    });
}

void FCVStepRegistry::internalServerCleanupForDowngrade(OperationContext* opCtx,
                                                        const FCV originalVersion,
                                                        const FCV requestedVersion) {
    std::for_each(_steps.begin(), _steps.end(), [&](FCVStep* step) {
        step->internalServerCleanupForDowngrade(opCtx, originalVersion, requestedVersion);
    });
}

void FCVStepRegistry::finalizeDowngrade(OperationContext* opCtx, const FCV requestedVersion) {
    std::for_each(_steps.begin(), _steps.end(), [&](FCVStep* step) {
        step->finalizeDowngrade(opCtx, requestedVersion);
    });
}


void FCVStepRegistry::_registerFeature(FCVStep* step) {
    _steps.push_back(step);
}

void FCVStepRegistry::_unregisterFeature(FCVStep* step) {
    auto it = std::find(_steps.begin(), _steps.end(), step);
    invariant(it != _steps.end());
    _steps.erase(it);
}


}  // namespace mongo
