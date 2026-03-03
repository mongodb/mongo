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

namespace mongo {


class LegacyFCVStep : public mongo::FCVStep {
public:
    static LegacyFCVStep* get(ServiceContext* serviceContext);


    inline std::string getStepName() const final {
        return "LegacyFCVStep";
    }

private:
    void prepareToUpgradeActionsBeforeGlobalLock(OperationContext* opCtx,
                                                 FCV originalVersion,
                                                 FCV requestedVersion) final {}

    void userCollectionsUassertsForUpgrade(OperationContext* opCtx,
                                           FCV originalVersion,
                                           FCV requestedVersion) final {}

    void userCollectionsWorkForUpgrade(OperationContext* opCtx,
                                       FCV originalVersion,
                                       FCV requestedVersion) final {}

    void upgradeServerMetadata(OperationContext* opCtx,
                               FCV originalVersion,
                               FCV requestedVersion) final {}

    void finalizeUpgrade(OperationContext* optCtx, FCV requestedVersion) final {}

    void prepareToDowngradeActions(OperationContext* opCtx,
                                   FCV originalVersion,
                                   FCV requestedVersion) final {}

    void userCollectionsUassertsForDowngrade(OperationContext* opCtx,
                                             FCV originalVersion,
                                             FCV requestedVersion) final {}

    void internalServerCleanupForDowngrade(OperationContext* opCtx,
                                           FCV originalVersion,
                                           FCV requestedVersion) final {}

    void finalizeDowngrade(OperationContext* opCtx, FCV requestedVersion) final {}
};

namespace {

const auto _sampleDecoration = ServiceContext::declareDecoration<LegacyFCVStep>();

const FCVStepRegistry::Registerer<LegacyFCVStep> _LegacyFCVStepRegisterer("LegacyFCVStep");

}  // namespace

LegacyFCVStep* LegacyFCVStep::get(ServiceContext* serviceContext) {
    return &_sampleDecoration(serviceContext);
}

}  // namespace mongo

