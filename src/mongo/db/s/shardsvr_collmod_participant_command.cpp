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


#include <memory>
#include <string>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/coll_mod_gen.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/sharded_collmod_gen.h"
#include "mongo/db/s/sharding_recovery_service.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/timeseries/catalog_helper.h"
#include "mongo/db/timeseries/timeseries_collmod.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/grid.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/out_of_line_executor.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

void releaseCriticalSectionInEmptySession(OperationContext* opCtx,
                                          ShardingRecoveryService* service,
                                          const NamespaceString& bucketNs,
                                          const BSONObj& reason) {
    auto txnParticipant = TransactionParticipant::get(opCtx);
    if (txnParticipant) {
        auto newClient =
            getGlobalServiceContext()->makeClient("ShardsvrMovePrimaryExitCriticalSection");
        AlternativeClientRegion acr(newClient);
        auto newOpCtx = CancelableOperationContext(
            cc().makeOperationContext(),
            opCtx->getCancellationToken(),
            Grid::get(opCtx->getServiceContext())->getExecutorPool()->getFixedExecutor());
        newOpCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
        service->releaseRecoverableCriticalSection(
            newOpCtx.get(), bucketNs, reason, ShardingCatalogClient::kLocalWriteConcern);
    } else {
        // No need to create a new operation context if no session is checked-out
        service->releaseRecoverableCriticalSection(
            opCtx, bucketNs, reason, ShardingCatalogClient::kLocalWriteConcern);
    }
}

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

                auto service = ShardingRecoveryService::get(opCtx);
                const auto reason = BSON("command"
                                         << "ShardSvrParticipantBlockCommand"
                                         << "ns"
                                         << NamespaceStringUtil::serialize(
                                                bucketNs, SerializationContext::stateDefault()));
                // In order to guarantee replay protection ShardsvrCollModParticipant will run
                // within a retryable write. Any local transaction or retryable write spawned by
                // this command (such as the release of the critical section) using the original
                // operation context will cause a dead lock since the session has been already
                // checked-out. We prevent the issue by using a new operation context with an
                // empty session.
                releaseCriticalSectionInEmptySession(opCtx, service, bucketNs, reason);
            }

            BSONObjBuilder builder;
            CollMod cmd(ns());
            cmd.setCollModRequest(request().getCollModRequest());

            // This flag is set from the collMod coordinator. We do not allow view definition change
            // on non-primary shards since it's not in the view catalog.
            auto performViewChange = request().getPerformViewChange();
            uassertStatusOK(timeseries::processCollModCommandWithTimeSeriesTranslation(
                opCtx, ns(), cmd, performViewChange, &builder));
            auto collmodReply =
                CollModReply::parse(IDLParserContext("CollModReply"), builder.obj());

            // Since no write that generated a retryable write oplog entry with this sessionId
            // and txnNumber happened, we need to make a dummy write so that the session gets
            // durably persisted on the oplog. This must be the last operation done on this
            // command.
            auto txnParticipant = TransactionParticipant::get(opCtx);
            if (txnParticipant) {
                DBDirectClient dbClient(opCtx);
                dbClient.update(NamespaceString::kServerConfigurationNamespace,
                                BSON("_id" << Request::kCommandName),
                                BSON("$inc" << BSON("count" << 1)),
                                true /* upsert */,
                                false /* multi */);
            }
            return collmodReply;
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
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::internal));
        }
    };
};
MONGO_REGISTER_COMMAND(ShardSvrCollModParticipantCommand).forShard();

}  // namespace
}  // namespace mongo
