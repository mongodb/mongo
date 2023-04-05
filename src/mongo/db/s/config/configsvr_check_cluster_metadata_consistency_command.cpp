/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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


#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/metadata_consistency_util.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"

namespace mongo {
namespace {

std::vector<MetadataInconsistencyItem> getHiddenCollectionsInconsistencies(
    OperationContext* opCtx) {
    static const auto rawPipelineStages = [] {
        auto rawPipelineBSON = fromjson(R"({pipeline: [
            {
                $addFields: {
                    dbName: {
                        $arrayElemAt: [{
                            $split: ['$_id', '.']
                        }, 0]
                    }
                }
            },
            {
                $match: {
                    dbName: {
                        $ne: 'config'
                    }
                }
            },
            {
                $lookup: {
                    from: 'databases',
                    localField: 'dbName',
                    foreignField: '_id',
                    as: 'db'
                }
            },
            {
                $match: {
                    db: []
                }
            }
        ]})");
        return parsePipelineFromBSON(rawPipelineBSON.firstElement());
    }();

    AggregateCommandRequest hiddenCollAggRequest{NamespaceString::kConfigsvrCollectionsNamespace,
                                                 rawPipelineStages};
    const auto catalogClient = ShardingCatalogManager::get(opCtx)->localCatalogClient();
    auto rawHiddenColls = catalogClient->runCatalogAggregation(
        opCtx, hiddenCollAggRequest, {repl::ReadConcernLevel::kSnapshotReadConcern});

    std::vector<MetadataInconsistencyItem> inconsistencies;
    inconsistencies.reserve(rawHiddenColls.size());
    for (auto&& rawHiddenColl : rawHiddenColls) {
        CollectionType coll{rawHiddenColl};
        inconsistencies.emplace_back(metadata_consistency_util::makeInconsistency(
            MetadataInconsistencyTypeEnum::kHiddenShardedCollection,
            HiddenShardedCollectionDetails{coll.getNss(), coll.toBSON()}));
    }
    return inconsistencies;
}

class ConfigsvrCheckClusterMetadataConsistencyCommand final
    : public TypedCommand<ConfigsvrCheckClusterMetadataConsistencyCommand> {
public:
    using Request = ConfigsvrCheckClusterMetadataConsistency;
    using Response = CursorInitialReply;

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command. Do not call directly.";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            std::vector<MetadataInconsistencyItem> inconsistencies;

            auto hiddenCollectionsIncon = getHiddenCollectionsInconsistencies(opCtx);
            inconsistencies.insert(inconsistencies.end(),
                                   std::make_move_iterator(hiddenCollectionsIncon.begin()),
                                   std::make_move_iterator(hiddenCollectionsIncon.end()));

            auto exec = metadata_consistency_util::makeQueuedPlanExecutor(
                opCtx, std::move(inconsistencies), ns());

            ClientCursorParams cursorParams{
                std::move(exec),
                ns(),
                AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserName(),
                APIParameters::get(opCtx),
                opCtx->getWriteConcern(),
                repl::ReadConcernArgs::get(opCtx),
                ReadPreferenceSetting::get(opCtx),
                request().toBSON({}),
                {Privilege(ResourcePattern::forClusterResource(), ActionType::internal)}};

            const auto batchSize = [&]() -> long long {
                const auto& cursorOpts = request().getCursor();
                if (cursorOpts && cursorOpts->getBatchSize()) {
                    return *cursorOpts->getBatchSize();
                } else {
                    return query_request_helper::kDefaultBatchSize;
                }
            }();

            return metadata_consistency_util::createInitialCursorReplyMongod(
                opCtx, std::move(cursorParams), batchSize);
        }

    private:
        NamespaceString ns() const override {
            return NamespaceString{request().getDbName()};
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }
    };

} configsvrCheckClusterMetadataConsistencyCommand;

}  // namespace
}  // namespace mongo
