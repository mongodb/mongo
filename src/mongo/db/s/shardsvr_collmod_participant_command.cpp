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


#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/collmod_coordinator.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/sharded_collmod_gen.h"
#include "mongo/db/s/sharding_recovery_service.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/timeseries/catalog_helper.h"
#include "mongo/db/timeseries/timeseries_collmod.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/grid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

class ShardSvrCollModParticipantCommand final
    : public TypedCommand<ShardSvrCollModParticipantCommand> {
public:
    using Request = ShardsvrCollModParticipant;
    using Response = CollModReply;

    std::string help() const override {
        return "Internal command, which is exported by the shards. Do not call "
               "directly. Unblocks CRUD and processes collMod.";
    }

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    bool supportsRetryableWrite() const final {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());

            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            // If the needsUnblock flag is set, we must have blocked the CRUD operations in the
            // previous phase of collMod operation for granularity updates. Unblock it now after we
            // have updated the granularity.
            if (request().getNeedsUnblock()) {
                // This is only ever used for time-series collection as of now.
                uassert(6102802,
                        "collMod unblocking should always be on a time-series collection",
                        timeseries::getTimeseriesOptions(opCtx, ns(), true));
                auto bucketNs = ns().makeTimeseriesBucketsNamespace();
                {
                    // Clear the filtering metadata before releasing the critical section to prevent
                    // scenarios where a stepDown/stepUp will leave the node with wrong metadata.
                    // Cleanup on secondary nodes is performed by the release of the section.
                    AutoGetCollection autoColl(opCtx, bucketNs, MODE_IX);
                    CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx,
                                                                                         bucketNs)
                        ->clearFilteringMetadata(opCtx);
                }
                // Starting from 7.1 In order to guarantee replay protection
                // ShardsvrCollModParticipant will run within a retryable write. Any local
                // transaction or retryable write spawned by this command (such as the release of
                // the critical section) using the original operation context will cause a dead lock
                // since the session has been already checked-out. We prevent the issue by using a
                // new operation context with an empty session.
                // Note for 7.0: this is done for multiversion compatibility with 7.1. No OSI is
                // attached to a request for this version
                auto newClient =
                    getGlobalServiceContext()->makeClient("ShardsvrMovePrimaryExitCriticalSection");
                AlternativeClientRegion acr(newClient);
                auto newOpCtx = CancelableOperationContext(
                    cc().makeOperationContext(),
                    opCtx->getCancellationToken(),
                    Grid::get(opCtx->getServiceContext())->getExecutorPool()->getFixedExecutor());
                newOpCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
                auto service = ShardingRecoveryService::get(newOpCtx.get());
                const auto reason = BSON("command"
                                         << "ShardSvrParticipantBlockCommand"
                                         << "ns" << bucketNs.toString());
                service->releaseRecoverableCriticalSection(
                    newOpCtx.get(), bucketNs, reason, ShardingCatalogClient::kLocalWriteConcern);
            }

            BSONObjBuilder builder;
            CollMod cmd(ns());
            cmd.setCollModRequest(request().getCollModRequest());

            // This flag is set from the collMod coordinator. We do not allow view definition change
            // on non-primary shards since it's not in the view catalog.
            auto performViewChange = request().getPerformViewChange();
            uassertStatusOK(timeseries::processCollModCommandWithTimeSeriesTranslation(
                opCtx, ns(), cmd, performViewChange, &builder));
            auto response = CollModReply::parse(IDLParserContext("CollModReply"), builder.obj());

            // Since no write that generated a retryable write oplog entry with this sessionId and
            // txnNumber happened, we need to make a dummy write so that the session gets durably
            // persisted on the oplog. This must be the last operation done on this command.
            // Note for 7.0: this is done for multiversion compatibility with 7.1. No OSI is
            // attached to a request for this version
            DBDirectClient dbClient(opCtx);
            dbClient.update(NamespaceString::kServerConfigurationNamespace,
                            BSON("_id" << Request::kCommandName),
                            BSON("$inc" << BSON("count" << 1)),
                            true /* upsert */,
                            false /* multi */);
            return response;
        }

    private:
        NamespaceString ns() const override {
            return request().getNamespace();
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
} shardsvrCollModParticipantCommand;

}  // namespace
}  // namespace mongo
