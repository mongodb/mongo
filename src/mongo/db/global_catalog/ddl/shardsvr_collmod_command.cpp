/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/checked_cast.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/global_catalog/ddl/collmod_coordinator.h"
#include "mongo/db/global_catalog/ddl/collmod_coordinator_document_gen.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_service.h"
#include "mongo/db/local_catalog/ddl/coll_mod_gen.h"
#include "mongo/db/local_catalog/ddl/coll_mod_reply_validation.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/string_map.h"

#include <memory>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class ShardsvrCollModCommand final : public BasicCommandWithRequestParser<ShardsvrCollModCommand> {
public:
    using Request = ShardsvrCollMod;
    using Response = CollModReply;

    ShardsvrCollModCommand() : BasicCommandWithRequestParser() {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    bool collectsResourceConsumptionMetrics() const override {
        return true;
    }

    std::string help() const override {
        return "Internal command, which is exported by the primary sharding server. Do not call "
               "directly. Modifies collection.";
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override {
        auto* client = opCtx->getClient();
        const NamespaceString nss(parseNs(dbName, cmdObj));
        return auth::checkAuthForCollMod(client->getOperationContext(),
                                         AuthorizationSession::get(client),
                                         nss,
                                         cmdObj,
                                         false,
                                         SerializationContext::stateCommandRequest());
    }

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    bool runWithRequestParser(OperationContext* opCtx,
                              const DatabaseName& dbName,
                              const BSONObj& cmdObj,
                              const RequestParser& requestParser,
                              BSONObjBuilder& result) override {
        auto const shardingState = ShardingState::get(opCtx);
        shardingState->assertCanAcceptShardedCommands();

        CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                      opCtx->getWriteConcern());

        opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

        // Since this operation is not directly writing locally we need to force its db
        // profile level increase in order to be logged in "<db>.system.profile"
        const auto& cmd = requestParser.request();
        CurOp::get(opCtx)->raiseDbProfileLevel(
            DatabaseProfileSettings::get(opCtx->getServiceContext())
                .getDatabaseProfileLevel(cmd.getNamespace().dbName()));

        auto coordinatorDoc = CollModCoordinatorDocument();
        coordinatorDoc.setCollModRequest(cmd.getCollModRequest());
        coordinatorDoc.setShardingDDLCoordinatorMetadata(
            {{cmd.getNamespace(), DDLCoordinatorTypeEnum::kCollMod}});
        auto service = ShardingDDLCoordinatorService::getService(opCtx);
        auto collModCoordinator = checked_pointer_cast<CollModCoordinator>(
            service->getOrCreateInstance(opCtx, coordinatorDoc.toBSON(), FixedFCVRegion{opCtx}));

        result.appendElements(collModCoordinator->getResult(opCtx));
        return true;
    }

    void validateResult(const BSONObj& resultObj) final {
        StringDataSet ignorableFields({"raw", "ok", "errmsg"});
        auto reply = Response::parse(resultObj.removeFields(ignorableFields),
                                     IDLParserContext("CollModReply"));
        coll_mod_reply_validation::validateReply(reply);
    }
};
MONGO_REGISTER_COMMAND(ShardsvrCollModCommand).forShard();

}  // namespace
}  // namespace mongo
