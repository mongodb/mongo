/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/db/s/resharding/resharding_coordinator_service.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/db/vector_clock.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/reshard_collection_gen.h"
#include "mongo/s/sharded_collections_ddl_parameters_gen.h"

namespace mongo {
namespace {

std::vector<TagsType> convertZones(const std::vector<BSONObj>& objs) {
    std::vector<TagsType> zones;

    for (const BSONObj& obj : objs) {
        zones.push_back(uassertStatusOK(TagsType::fromBSON(obj)));
    }

    return zones;
}

class ConfigsvrReshardCollectionCommand final
    : public TypedCommand<ConfigsvrReshardCollectionCommand> {
public:
    using Request = ConfigsvrReshardCollection;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {

            uassert(ErrorCodes::IllegalOperation,
                    "_configsvrReshardCollection can only be run on config servers",
                    serverGlobalParams.clusterRole == ClusterRole::ConfigServer);
            uassert(ErrorCodes::InvalidOptions,
                    "_configsvrReshardCollection must be called with majority writeConcern",
                    opCtx->getWriteConcern().wMode == WriteConcernOptions::kMajority);

            repl::ReadConcernArgs::get(opCtx) =
                repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

            const NamespaceString& nss = ns();

            uassert(ErrorCodes::BadValue,
                    "The unique field must be false",
                    !request().getUnique().get_value_or(false));

            if (request().getCollation()) {
                auto& collation = request().getCollation().get();
                auto collator =
                    uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                        ->makeFromBSON(collation));
                uassert(ErrorCodes::BadValue,
                        str::stream()
                            << "The collation for reshardCollection must be {locale: 'simple'}, "
                            << "but found: " << collation,
                        !collator);
            }

            std::vector<TagsType> newZones;
            const auto& authoritativeTags = uassertStatusOK(
                Grid::get(opCtx)->catalogClient()->getTagsForCollection(opCtx, nss));
            if (!authoritativeTags.empty()) {
                uassert(ErrorCodes::BadValue,
                        "Must specify value for zones field",
                        request().getZones());
                validateZones(request().getZones().get(), authoritativeTags);
                newZones = convertZones(request().getZones().get());
            }

            const auto cm = uassertStatusOK(
                Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithRefresh(opCtx,
                                                                                             nss));

            bool presetReshardedChunksSpecified = bool(request().get_presetReshardedChunks());
            uassert(ErrorCodes::BadValue,
                    "Test commands must be enabled when a value is provided for field: "
                    "_presetReshardedChunks",
                    !presetReshardedChunksSpecified || getTestCommandsEnabled());

            uassert(ErrorCodes::BadValue,
                    "Must specify only one of _presetReshardedChunks or numInitialChunks",
                    !(presetReshardedChunksSpecified && bool(request().getNumInitialChunks())));

            std::set<ShardId> donorShardIds;
            cm.getAllShardIds(&donorShardIds);

            int numInitialChunks;
            std::set<ShardId> recipientShardIds;
            std::vector<ChunkType> initialChunks;

            boost::optional<Timestamp> timestamp;
            if (feature_flags::gShardingFullDDLSupportTimestampedVersion.isEnabled(
                    serverGlobalParams.featureCompatibility)) {
                const auto now = VectorClock::get(opCtx)->getTime();
                timestamp = now.clusterTime().asTimestamp();
            }

            ChunkVersion version(1, 0, OID::gen(), timestamp);
            auto tempReshardingNss = constructTemporaryReshardingNss(
                nss.db(), getCollectionUUIDFromChunkManger(nss, cm));

            if (presetReshardedChunksSpecified) {
                const auto chunks = request().get_presetReshardedChunks().get();
                validateReshardedChunks(
                    chunks, opCtx, ShardKeyPattern(request().getKey()).getKeyPattern());
                numInitialChunks = chunks.size();

                // Use the provided shardIds from presetReshardedChunks to construct the
                // recipient list.
                for (const BSONObj& obj : chunks) {
                    recipientShardIds.emplace(
                        obj.getStringField(ReshardedChunk::kRecipientShardIdFieldName));

                    auto reshardedChunk =
                        ReshardedChunk::parse(IDLParserErrorContext("ReshardedChunk"), obj);
                    initialChunks.emplace_back(
                        tempReshardingNss,
                        ChunkRange{reshardedChunk.getMin(), reshardedChunk.getMax()},
                        version,
                        reshardedChunk.getRecipientShardId());
                    version.incMinor();
                }
            } else {
                numInitialChunks = request().getNumInitialChunks().get_value_or(cm.numChunks());

                // No presetReshardedChunks were provided, make the recipients list be the same as
                // the donors list by default.
                recipientShardIds = donorShardIds;

                cm.forEachChunk([&](const auto& chunk) {
                    initialChunks.emplace_back(tempReshardingNss,
                                               ChunkRange{chunk.getMin(), chunk.getMax()},
                                               version,
                                               chunk.getShardId());
                    version.incMinor();
                    return true;
                });
            }

            // Construct the lists of donor and recipient shard entries, where each ShardEntry is
            // in state kUnused.
            std::vector<DonorShardEntry> donorShards;
            std::transform(donorShardIds.begin(),
                           donorShardIds.end(),
                           std::back_inserter(donorShards),
                           [](const ShardId& shardId) -> DonorShardEntry {
                               DonorShardEntry entry{shardId};
                               entry.setState(DonorStateEnum::kUnused);
                               return entry;
                           });
            std::vector<RecipientShardEntry> recipientShards;
            std::transform(recipientShardIds.begin(),
                           recipientShardIds.end(),
                           std::back_inserter(recipientShards),
                           [](const ShardId& shardId) -> RecipientShardEntry {
                               RecipientShardEntry entry{shardId};
                               entry.setState(RecipientStateEnum::kUnused);
                               return entry;
                           });

            auto coordinatorDoc =
                ReshardingCoordinatorDocument(std::move(tempReshardingNss),
                                              std::move(CoordinatorStateEnum::kInitializing),
                                              std::move(donorShards),
                                              std::move(recipientShards));

            // Generate the resharding metadata for the ReshardingCoordinatorDocument.
            auto reshardingUUID = UUID::gen();
            auto existingUUID = getCollectionUUIDFromChunkManger(ns(), cm);
            auto commonMetadata = CommonReshardingMetadata(
                std::move(reshardingUUID), ns(), std::move(existingUUID), request().getKey());
            coordinatorDoc.setCommonReshardingMetadata(std::move(commonMetadata));

            auto registry = repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext());
            auto service = registry->lookupServiceByName(kReshardingCoordinatorServiceName);
            auto instance = ReshardingCoordinatorService::ReshardingCoordinator::getOrCreate(
                opCtx, service, coordinatorDoc.toBSON());

            instance->setInitialChunksAndZones(std::move(initialChunks), std::move(newZones));
            instance->getCompletionFuture().get(opCtx);
        }

    private:
        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        bool supportsWriteConcern() const override {
            return true;
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
        return "Internal command, which is exported by the sharding config server. Do not call "
               "directly. Reshards a collection on a new shard key.";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
} configsvrReshardCollectionCmd;

}  // namespace
}  // namespace mongo
