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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/s/migration_destination_manager.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/s/request_types/clone_collection_options_from_primary_shard_gen.h"
#include "mongo/s/shard_id.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

class CloneCollectionOptionsFromPrimaryShardCommand
    : public TypedCommand<CloneCollectionOptionsFromPrimaryShardCommand> {
public:
    using Request = CloneCollectionOptionsFromPrimaryShard;
    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            auto primaryShardId = ShardId(request().getPrimaryShard().toString());
            MigrationDestinationManager::cloneCollectionIndexesAndOptions(
                opCtx, ns(), primaryShardId);

            // At the time this command is invoked, the config server primary has already written
            // the collection's routing metadata, so sync from the config server
            forceShardFilteringMetadataRefresh(opCtx, ns());
        }

    private:
        bool supportsWriteConcern() const override {
            return true;
        }

        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }
    };

    std::string help() const override {
        return "Internal command, do not call directly. Creates a collection on a shard with UUID"
               " existing on primary.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return false;
    }
} CloneCollectionOptionsFromPrimaryShardCmd;

}  // namespace
}  // namespace mongo
