/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/commands/buildinfo_common.h"

#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/buildinfo.h"

namespace mongo {
namespace {
Atomic<BuildInfoAuthModeEnum> gBuildInfoAuthMode{BuildInfoAuthModeEnum::kRequiresAuth};

class BuildInfoExecutor final : public AsyncRequestExecutor {
public:
    BuildInfoExecutor() : AsyncRequestExecutor("BuildInfoExecutor") {}

    Status handleRequest(std::shared_ptr<RequestExecutionContext> rec) override {
        auto result = rec->getReplyBuilder()->getBodyBuilder();
        getBuildInfo().serialize(&result);
        return Status::OK();
    }

    static BuildInfoExecutor* get(ServiceContext* svc);
};

const auto getBuildInfoExecutor = ServiceContext::declareDecoration<BuildInfoExecutor>();
BuildInfoExecutor* BuildInfoExecutor::get(ServiceContext* svc) {
    return const_cast<BuildInfoExecutor*>(&getBuildInfoExecutor(svc));
}

const auto buildInfoExecutorRegisterer = ServiceContext::ConstructorActionRegisterer{
    "BuildInfoExecutor",
    [](ServiceContext* ctx) { getBuildInfoExecutor(ctx).start(); },
    [](ServiceContext* ctx) {
        getBuildInfoExecutor(ctx).stop();
    }};
}  // namespace

void BuildInfoAuthModeServerParameter::append(OperationContext*,
                                              BSONObjBuilder* builder,
                                              StringData fieldName,
                                              const boost::optional<TenantId>&) {
    builder->append(fieldName, BuildInfoAuthMode_serializer(gBuildInfoAuthMode.load()));
}

Status BuildInfoAuthModeServerParameter::setFromString(StringData strMode,
                                                       const boost::optional<TenantId>&) try {
    gBuildInfoAuthMode.store(
        BuildInfoAuthMode_parse(strMode, IDLParserContext{"buildInfoAuthMode"}));
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
    return checked_cast<const CmdBuildInfoCommon*>(definition())->generateBuildInfo(opCtx);
}

Future<void> CmdBuildInfoCommon::Invocation::runAsync(
    std::shared_ptr<RequestExecutionContext> rec) {
    auto* svcCtx = rec->getOpCtx()->getServiceContext();
    auto* executor =
        checked_cast<const CmdBuildInfoCommon*>(definition())->getAsyncRequestExecutor(svcCtx);
    return executor->schedule(std::move(rec));
}

AsyncRequestExecutor* CmdBuildInfoCommon::getAsyncRequestExecutor(ServiceContext* svcCtx) const {
    return BuildInfoExecutor::get(svcCtx);
}

}  // namespace mongo
