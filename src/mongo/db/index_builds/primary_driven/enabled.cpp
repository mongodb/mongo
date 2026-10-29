// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index_builds/primary_driven/enabled.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/version_context.h"

namespace mongo::index_builds::primary_driven {

bool enabled(OperationContext* opCtx, const ServerGlobalParams::FCVSnapshot& fcvSnapshot) {
    return enabled(opCtx, VersionContext::getDecoration(opCtx), fcvSnapshot);
}

bool enabled(OperationContext* opCtx,
             const VersionContext& vCtx,
             const ServerGlobalParams::FCVSnapshot& fcvSnapshot) {
    if (rss::ReplicatedStorageService::get(opCtx)
            .getPersistenceProvider()
            .mustUsePrimaryDrivenIndexBuilds()) {
        return true;
    }
    return feature_flags::gFeatureFlagPrimaryDrivenIndexBuilds
        .isEnabledUseLastLTSFCVWhenUninitialized(vCtx, fcvSnapshot);
}

}  // namespace mongo::index_builds::primary_driven

namespace mongo {
namespace {

class IndexBuildOplogGroupingPolicy : public OplogGroupingPolicy {
public:
    bool shouldGroupOplogEntries(OperationContext* opCtx) const override {
        return index_builds::primary_driven::enabled(
            opCtx, serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
    }
};

ServiceContext::ConstructorActionRegisterer registerOplogGroupingPolicy{
    "RegisterOplogGroupingPolicy", [](ServiceContext* svc) {
        OplogGroupingPolicy::set(svc, std::make_unique<IndexBuildOplogGroupingPolicy>());
    }};

}  // namespace
}  // namespace mongo
