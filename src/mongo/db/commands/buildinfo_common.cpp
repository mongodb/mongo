// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands/buildinfo_common.h"

#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/metrics_policy_manager.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/buildinfo.h"

#include <string_view>

namespace mongo {
namespace {
Atomic<BuildInfoAuthModeEnum> gBuildInfoAuthMode{BuildInfoAuthModeEnum::kRequiresAuth};
}  // namespace

void BuildInfoAuthModeServerParameter::append(OperationContext*,
                                              BSONObjBuilder* builder,
                                              std::string_view fieldName,
                                              const boost::optional<TenantId>&) {
    builder->append(fieldName, idl::serialize(gBuildInfoAuthMode.load()));
}

Status BuildInfoAuthModeServerParameter::setFromString(std::string_view strMode,
                                                       const boost::optional<TenantId>&) try {
    gBuildInfoAuthMode.store(
        idl::deserialize<BuildInfoAuthModeEnum>(strMode, IDLParserContext{"buildInfoAuthMode"}));
    return Status::OK();
} catch (const DBException& ex) {
    return ex.toStatus().withContext("Invalid value for Server Parameter: 'buildInfoAuthMode'");
}

bool CmdBuildInfoCommon::requiresAuth() const {
    return gBuildInfoAuthMode.load() == BuildInfoAuthModeEnum::kRequiresAuth;
}

BuildInfo CmdBuildInfoCommon::generateBuildInfo(OperationContext*) const {
    return getBuildInfo();
}

BuildInfo CmdBuildInfoCommon::Invocation::typedRun(OperationContext* opCtx) {
    const auto mode = gBuildInfoAuthMode.load();
    auto isAuthenticated = [&] {
        if (!AuthorizationManager::get(opCtx->getService())->isAuthEnabled()) {
            // Authentication not enabled in this configuration.
            return true;
        }

        auto* as = AuthorizationSession::get(opCtx->getClient());
        if (as->isUsingLocalhostBypass()) {
            // Special-case localhost auth bypass.
            return true;
        }

        return as->isAuthenticated();
    };

    if (mode == BuildInfoAuthModeEnum::kVersionOnlyIfPreAuth && !isAuthenticated()) {
        // Limited response required for certain legacy drivers.
        return getBuildInfoVersionOnly();
    }

    invariant(mode == BuildInfoAuthModeEnum::kRequiresAuth ||
              mode == BuildInfoAuthModeEnum::kAllowedPreAuth ||
              mode == BuildInfoAuthModeEnum::kVersionOnlyIfPreAuth);
    auto info = checked_cast<const CmdBuildInfoCommon*>(definition())->generateBuildInfo(opCtx);
    if (auto&& mgr = MetricsPolicyManager::get(opCtx);
        mgr.requiresFiltering(opCtx, MetricsCategoryEnum::kBuildInfo, /*forceFiltered=*/false)) {
        BSONObjBuilder bob;
        metrics_filtering_util::appendPaths(
            bob, info.toBSON(), mgr.getAllowlistMatcher(MetricsCategoryEnum::kBuildInfo));
        info = BuildInfo::parseOwned(bob.obj());
    }
    return info;
}

}  // namespace mongo
