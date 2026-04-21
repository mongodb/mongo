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
