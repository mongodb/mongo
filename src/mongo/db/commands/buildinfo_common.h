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
#include "mongo/executor/async_request_executor.h"

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
class CmdBuildInfoCommon : public TypedCommand<CmdBuildInfoCommon> {
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

        Future<void> runAsync(std::shared_ptr<RequestExecutionContext> rec) override;
    };

    /**
     * Generates complete buildInfo response for authenticated user (or when configured for
     * preAuth).
     */
    virtual Reply generateBuildInfo(OperationContext*) const;

    /**
     * Provide an executor on which to run the command.
     */
    virtual AsyncRequestExecutor* getAsyncRequestExecutor(ServiceContext* svcCtx) const;
};

}  // namespace mongo
