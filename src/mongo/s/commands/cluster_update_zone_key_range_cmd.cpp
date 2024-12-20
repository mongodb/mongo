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


#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/update_zone_key_range_gen.h"
#include "mongo/util/assert_util.h"
#include <string>


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

namespace {

const ReadPreferenceSetting kPrimaryOnlyReadPreference{ReadPreference::PrimaryOnly};
const WriteConcernOptions kMajorityWriteConcern(WriteConcernOptions::kMajority,
                                                // Note: Even though we're setting UNSET here,
                                                // kMajority implies JOURNAL if journaling is
                                                // supported by mongod and
                                                // writeConcernMajorityJournalDefault is set to true
                                                // in the ReplSetConfig.
                                                WriteConcernOptions::SyncMode::UNSET,
                                                WriteConcernOptions::kWriteConcernTimeoutSharding);

class UpdateZoneKeyRangeCmd : public TypedCommand<UpdateZoneKeyRangeCmd> {
public:
    using Request = UpdateZoneKeyRange;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::InvalidNamespace,
                    "invalid namespace specified for request",
                    ns().isValid());

            BSONObjBuilder cmdBuilder;
            ConfigsvrUpdateZoneKeyRange cmd(
                ns(), request().getMin(), request().getMax(), request().getZone());
            cmd.serialize(&cmdBuilder);
            cmdBuilder.append("writeConcern", kMajorityWriteConcern.toBSON());

            auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
            auto cmdResponseStatus = uassertStatusOK(
                configShard->runCommandWithFixedRetryAttempts(opCtx,
                                                              kPrimaryOnlyReadPreference,
                                                              DatabaseName::kAdmin,
                                                              cmdBuilder.obj(),
                                                              Shard::RetryPolicy::kIdempotent));
            uassertStatusOK(cmdResponseStatus.commandStatus);
        }

    private:
        bool isRemove() const {
            if (request().getZone()) {
                return false;
            }
            return true;
        }

        ChunkRange getRange() const {
            auto range = ChunkRange(request().getMin(), request().getMax());
            uassertStatusOK(ChunkRange::validate(range));
            return range;
        }

        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            auto* as = AuthorizationSession::get(opCtx->getClient());

            uassert(
                ErrorCodes::Unauthorized,
                "Unauthorized",
                as->isAuthorizedForActionsOnResource(
                    ResourcePattern::forExactNamespace(NamespaceString::kConfigsvrShardsNamespace),
                    ActionType::find));

            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    as->isAuthorizedForActionsOnResource(
                        ResourcePattern::forExactNamespace(TagsType::ConfigNS), ActionType::find));

            uassert(
                ErrorCodes::Unauthorized,
                "Unauthorized",
                as->isAuthorizedForActionsOnResource(
                    ResourcePattern::forExactNamespace(TagsType::ConfigNS), ActionType::update));
            uassert(
                ErrorCodes::Unauthorized,
                "Unauthorized",
                as->isAuthorizedForActionsOnResource(
                    ResourcePattern::forExactNamespace(TagsType::ConfigNS), ActionType::remove));
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "assigns/remove a range of a sharded collection to a zone.";
    }
};
MONGO_REGISTER_COMMAND(UpdateZoneKeyRangeCmd).forRouter();

}  // namespace

}  // namespace mongo
