/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/commands.h"
#include "mongo/db/commands/rwc_defaults_commands_gen.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

void assertNotStandaloneOrShardServer(OperationContext* opCtx, StringData cmdName) {
    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    uassert(51300,
            str::stream() << "'" << cmdName << "' is not supported on standalone nodes.",
            replCoord->isReplEnabled());

    uassert(51301,
            str::stream() << "'" << cmdName << "' is not supported on shard nodes.",
            serverGlobalParams.clusterRole != ClusterRole::ShardServer);
}

class SetDefaultRWConcernCommand : public TypedCommand<SetDefaultRWConcernCommand> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        // TODO SERVER-43126: Once CWRWC persistence and propagation have been implemented, this
        // should change to AllowedOnSecondary::kNever to only allow setting the default on
        // primaries.
        return AllowedOnSecondary::kAlways;
    }
    bool adminOnly() const override {
        return true;
    }
    std::string help() const override {
        return "set the current read/write concern defaults (cluster-wide)";
    }

public:
    using Request = SetDefaultRWConcern;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        auto typedRun(OperationContext* opCtx) {
            assertNotStandaloneOrShardServer(opCtx, SetDefaultRWConcern::kCommandName);

            auto& rwcDefaults = ReadWriteConcernDefaults::get(opCtx->getServiceContext());
            auto newDefaults = rwcDefaults.setConcerns(
                opCtx, request().getDefaultReadConcern(), request().getDefaultWriteConcern());
            log() << "successfully set RWC defaults to " << newDefaults.toBSON();
            return newDefaults;
        }

    private:
        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext*) const override {
            // TODO SERVER-45038: add and use privilege action
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName(), "");
        }
    };
} setDefaultRWConcernCommand;

class GetDefaultRWConcernCommand : public TypedCommand<GetDefaultRWConcernCommand> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    bool adminOnly() const override {
        return true;
    }
    std::string help() const override {
        return "get the current read/write concern defaults being applied by this node";
    }

public:
    using Request = GetDefaultRWConcern;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        auto typedRun(OperationContext* opCtx) {
            assertNotStandaloneOrShardServer(opCtx, GetDefaultRWConcern::kCommandName);

            // TODO SERVER-43720 Implement inMemory option.

            auto& rwcDefaults = ReadWriteConcernDefaults::get(opCtx->getServiceContext());
            return rwcDefaults.getDefault(opCtx);
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext*) const override {
            // TODO SERVER-45038: add and use privilege action
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName(), "");
        }
    };
} getDefaultRWConcernCommand;
}  // namespace
}  // namespace mongo
