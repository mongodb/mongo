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
#include "mongo/db/commands/buildinfo_common_gen.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/version.h"

namespace mongo {
namespace {
Atomic<BuildInfoAuthModeEnum> gBuildInfoAuthMode{BuildInfoAuthModeEnum::kRequiresAuth};
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
        BuildInfoAuthMode_parse(IDLParserContext{"buildInfoAuthMode"}, strMode));
    return Status::OK();
} catch (const DBException& ex) {
    return ex.toStatus().withContext("Invalid value for Server Parameter: 'buildInfoAuthMode'");
}

bool CmdBuildInfoBase::requiresAuth() const {
    return gBuildInfoAuthMode.load() == BuildInfoAuthModeEnum::kRequiresAuth;
}

bool CmdBuildInfoBase::run(OperationContext* opCtx,
                           const DatabaseName&,
                           const BSONObj&,
                           BSONObjBuilder& result) {
    const auto mode = gBuildInfoAuthMode.load();
    if (mode == BuildInfoAuthModeEnum::kAllowedPreAuth) {
        // Full buildinfo always allowed pre or post-auth.
        generateBuildInfo(opCtx, result);
        return true;
    }

    const bool isAuthenticated = [&] {
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
    }();

    if (isAuthenticated) {
        generateBuildInfo(opCtx, result);
        return true;
    }

    // In practice we should never actually trigger this uassert,
    // since requiresAuth() would have returned true, and run() would not have executed.
    uassert(ErrorCodes::Unauthorized,
            "buildInfo command requires authorization",
            mode != BuildInfoAuthModeEnum::kRequiresAuth);

    // kAllowedPreAuth handled at top of function, and kRequiresAuth ruled out above.
    invariant(mode == BuildInfoAuthModeEnum::kVersionOnlyIfPreAuth);

    // Limited response required for certain legacy drivers.
    VersionInfoInterface::instance().appendVersionInfoOnly(&result);
    return true;
}

}  // namespace mongo
