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


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/rwc_defaults_commands_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/sharding_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/read_write_concern_defaults_gen.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

// Hang during the execution of SetDefaultRWConcernCommand.
MONGO_FAIL_POINT_DEFINE(hangWhileSettingDefaultRWC);

/**
 * Replaces the persisted default read/write concern document with a new one representing the given
 * defaults. Waits for the write concern on the given operation context to be satisfied before
 * returning.
 */
void updatePersistedDefaultRWConcernDocument(OperationContext* opCtx, const RWConcernDefault& rw) {
    DBDirectClient client(opCtx);
    const auto commandResponse = client.runCommand([&] {
        write_ops::UpdateCommandRequest updateOp(NamespaceString::kConfigSettingsNamespace);
        updateOp.setUpdates({[&] {
            write_ops::UpdateOpEntry entry;
            entry.setQ(BSON("_id" << ReadWriteConcernDefaults::kPersistedDocumentId));
            // Note the _id is propagated from the query into the upserted document.
            entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(rw.toBSON()));
            entry.setUpsert(true);
            return entry;
        }()});
        updateOp.setWriteConcern(opCtx->getWriteConcern());
        return updateOp.serialize();
    }());
    uassertStatusOK(getStatusFromWriteCommandReply(commandResponse->getCommandReply()));
}

void assertNotStandaloneOrShardServer(OperationContext* opCtx, StringData cmdName) {
    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    uassert(51300,
            str::stream() << "'" << cmdName << "' is not supported on standalone nodes.",
            replCoord->getSettings().isReplSet());

    uassert(51301,
            str::stream() << "'" << cmdName << "' is not supported on shard nodes.",
            serverGlobalParams.clusterRole.has(ClusterRole::None) ||
                serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));
}

auto makeResponse(const ReadWriteConcernDefaults::RWConcernDefaultAndTime& rwcDefault,
                  bool inMemory) {
    GetDefaultRWConcernResponse response;
    response.setRWConcernDefault(rwcDefault);
    response.setLocalUpdateWallClockTime(rwcDefault.localUpdateWallClockTime());
    if (inMemory)
        response.setInMemory(true);

    return response;
}

class SetDefaultRWConcernCommand : public TypedCommand<SetDefaultRWConcernCommand> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
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

            auto replCoord = repl::ReplicationCoordinator::get(opCtx);
            auto wcChanges = replCoord->getWriteConcernTagChanges();

            // Synchronize this change with potential changes to the write concern tags.
            uassert(ErrorCodes::ConfigurationInProgress,
                    "Replica set reconfig in progress. Please retry the command later.",
                    wcChanges->reserveDefaultWriteConcernChange());
            ON_BLOCK_EXIT([&]() { wcChanges->releaseDefaultWriteConcernChange(); });

            hangWhileSettingDefaultRWC.pauseWhileSet();

            auto& rwcDefaults = ReadWriteConcernDefaults::get(opCtx);
            auto newDefaults = rwcDefaults.generateNewCWRWCToBeSavedOnDisk(
                opCtx, request().getDefaultReadConcern(), request().getDefaultWriteConcern());

            if (auto optWC = newDefaults.getDefaultWriteConcern()) {
                if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
                    // When trying to set a custom write concern, validate that it exists on all
                    // shards and the config server. This is a best-effort check as users may
                    // directly connect and manipulate write concerns while the check is running.
                    auto shardsWithMissingWCDefinition =
                        getShardsMissingWriteConcernDefinition(opCtx, *optWC);
                    if (!replCoord->validateWriteConcern(*optWC).isOK()) {
                        const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
                        const auto configShard = shardRegistry->getConfigShard();
                        shardsWithMissingWCDefinition.insert(configShard->getId());
                    }
                    uassert(
                        ErrorCodes::UnknownReplWriteConcern,
                        fmt::format(
                            "The value of the defaultWriteConcern must be specified on the config "
                            "server and all shards. The definition of the provided value is "
                            "missing on: {}.",
                            [&]() {
                                std::ostringstream stream;
                                StringData sep;
                                for (auto&& shard : shardsWithMissingWCDefinition) {
                                    stream << sep << "'" << shard.toString() << "'";
                                    sep = ", ";
                                }
                                return stream.str();
                            }()),
                        shardsWithMissingWCDefinition.empty());
                } else {
                    uassertStatusOK(replCoord->validateWriteConcern(*optWC));
                }
            }

            updatePersistedDefaultRWConcernDocument(opCtx, newDefaults);
            LOGV2(20498, "Successfully set RWC defaults", "value"_attr = newDefaults);

            // Refresh to populate the cache with the latest defaults.
            rwcDefaults.refreshIfNecessary(opCtx);
            return makeResponse(rwcDefaults.getDefault(opCtx), false);
        }

    private:
        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForPrivilege(Privilege{
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::setDefaultRWConcern}));
        }

        stdx::unordered_set<ShardId> getShardsMissingWriteConcernDefinition(
            OperationContext* opCtx, const WriteConcernOptions& writeConcern) {
            auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
            BSONObj replSetGetConfigCmd = BSON("replSetGetConfig" << 1);
            auto responses = sharding_util::sendCommandToShards(
                opCtx,
                DatabaseName::kAdmin,
                replSetGetConfigCmd,
                Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx),
                executor);
            stdx::unordered_set<ShardId> shardsMissingWCDef;
            for (auto&& response : responses) {
                uassertStatusOK(AsyncRequestsSender::Response::getEffectiveStatus(response));
                auto configBSON = response.swResponse.getValue().data;
                uassert(ErrorCodes::NoSuchKey,
                        "Missing 'config' field in replSetGetConfig response",
                        configBSON.hasField("config"));
                repl::ReplSetConfig shardReplSetConfig(
                    repl::ReplSetConfig::parse(configBSON["config"].Obj()));
                if (!shardReplSetConfig.validateWriteConcern(writeConcern).isOK()) {
                    shardsMissingWCDef.insert(response.shardId);
                }
            }
            return shardsMissingWCDef;
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }
    };
};
MONGO_REGISTER_COMMAND(SetDefaultRWConcernCommand).forShard();

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

            auto& rwcDefaults = ReadWriteConcernDefaults::get(opCtx);
            const bool inMemory = request().getInMemory().value_or(false);
            if (!inMemory) {
                // If not asking for the in-memory values, force a refresh to find the most recent
                // defaults
                rwcDefaults.refreshIfNecessary(opCtx);
            }

            return makeResponse(rwcDefaults.getDefault(opCtx), inMemory);
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForPrivilege(Privilege{
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::getDefaultRWConcern}));
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }
    };
};
MONGO_REGISTER_COMMAND(GetDefaultRWConcernCommand).forShard();

}  // namespace
}  // namespace mongo
