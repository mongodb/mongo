// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands/set_feature_compatibility_version_steps/fcv_step.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"

namespace mongo {
namespace {

/*
 * Prevent starting setFCV with index builds in progress, because catalog changes will fail
 * during the FCV upgrade/downgrade if there are index builds in progress (see ticket below).
 *
 * TODO(SERVER-114573): Consider removing this step if this is the last ticket depending on it
 * TODO(SERVER-100328): Consider removing this step if this is the last ticket depending on it
 * TODO(SERVER-117265): Consider removing this step if this is the last ticket depending on it
 */
class IndexBuildFCVStep : public mongo::FCVStep {
public:
    static IndexBuildFCVStep* get(ServiceContext* serviceContext);


    inline std::string getStepName() const final {
        return "IndexBuildFCVStep";
    }

private:
    void userCollectionsUassertsForUpgrade(OperationContext* opCtx, FCV, FCV) final {
        uassert(ErrorCodes::BackgroundOperationInProgressForNamespace,
                "Cannot upgrade the cluster when there are index builds in progress.",
                IndexBuildsCoordinator::get(opCtx)->noIndexBuildInProgress());
    }

    void userCollectionsUassertsForDowngrade(OperationContext* opCtx, FCV, FCV) final {
        uassert(ErrorCodes::BackgroundOperationInProgressForNamespace,
                "Cannot downgrade the cluster when there are index builds in progress.",
                IndexBuildsCoordinator::get(opCtx)->noIndexBuildInProgress());
    }
};

const auto decoration = ServiceContext::declareDecoration<IndexBuildFCVStep>();
const FCVStepRegistry::Registerer<IndexBuildFCVStep> indexBuildFCVStepRegisterer(
    "IndexBuildFCVStep");

IndexBuildFCVStep* IndexBuildFCVStep::get(ServiceContext* serviceContext) {
    return &decoration(serviceContext);
}

}  // namespace
}  // namespace mongo
