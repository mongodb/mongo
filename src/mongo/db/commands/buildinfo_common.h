// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/buildinfo_common_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/request_execution_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>

namespace mongo {

/**
 * Common implementation shared between shared and router roles.
 *
 * By default, the buildInfo command requires authentication in order to be run.
 * Using the buildInfoAuthMode server parameter however, this command may be
 * invoked in a pre-auth state to return either the entire reply (allowedPreAuth),
 * or a redacted reply of just version information (versionOnlyIfPreAuth).
 *
 * The default implementation available here provide every field except 'storageEngines'
 * which is only provided by the CmdBuildInfoShard specialization.
 */
class [[MONGO_MOD_OPEN]] CmdBuildInfoCommon : public TypedCommand<CmdBuildInfoCommon> {
public:
    using Request = BuildInfoCommand;
    using Reply = typename BuildInfoCommand::Reply;

    CmdBuildInfoCommon() : TypedCommand(Request::kCommandName, Request::kCommandAlias) {}

    AllowedOnSecondary secondaryAllowed(ServiceContext* svcCtx) const override {
        return AllowedOnSecondary::kAlways;
    }

    // See class comment.
    bool requiresAuth() const override;

    bool adminOnly() const override {
        return false;
    }

    bool allowedWithSecurityToken() const override {
        return true;
    }

    bool requiresAuthzChecks() const override {
        return false;
    }
    class Invocation : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext*) const override {
            // See class comment.
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }

        Reply typedRun(OperationContext*);
    };

    /**
     * Generates complete buildInfo response for authenticated user (or when configured for
     * preAuth).
     */
    virtual Reply generateBuildInfo(OperationContext*) const;
};

}  // namespace mongo
