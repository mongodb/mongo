/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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


#include "mongo/db/global_catalog/ddl/refine_collection_shard_key_coordinator.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/ddl/shard_key_util.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/global_catalog/ddl/sharding_util.h"
#include "mongo/db/global_catalog/router_role_api/router_role.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_runtime.h"
#include "mongo/db/local_catalog/shard_role_catalog/participant_block_gen.h"
#include "mongo/db/local_catalog/shard_role_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/sharding_environment/sharding_logging.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/db/vector_clock/vector_clock_mutable.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/logv2/log.h"

#include <string>
#include <tuple>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

namespace {

void notifyChangeStreamsOnRefineCollectionShardKeyComplete(OperationContext* opCtx,
                                                           const NamespaceString& collNss,
                                                           const KeyPattern& shardKey,
                                                           const KeyPattern& oldShardKey,
                                                           const UUID& collUUID) {

    const auto collNssStr =
        NamespaceStringUtil::serialize(collNss, SerializationContext::stateDefault());
    const std::string oMessage = str::stream()
        << "Refine shard key for collection " << collNssStr << " with " << shardKey.toString();

    BSONObjBuilder cmdBuilder;
    cmdBuilder.append("refineCollectionShardKey", collNssStr);
    cmdBuilder.append("shardKey", shardKey.toBSON());
    cmdBuilder.append("oldShardKey", oldShardKey.toBSON());

    auto const serviceContext = opCtx->getClient()->getServiceContext();

    writeConflictRetry(opCtx, "RefineCollectionShardKey", NamespaceString::kRsOplogNamespace, [&] {
        AutoGetOplogFastPath oplogWrite(opCtx, OplogAccessMode::kWrite);
        WriteUnitOfWork uow(opCtx);
        serviceContext->getOpObserver()->onInternalOpMessage(opCtx,
                                                             collNss,
                                                             collUUID,
                                                             BSON("msg" << oMessage),
                                                             cmdBuilder.obj(),
                                                             boost::none,
                                                             boost::none,
                                                             boost::none,
                                                             boost::none);
        uow.commit();
    });
}

void logRefineCollectionShardKey(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 const std::string& eventStr,
                                 const BSONObj& obj) {
    ShardingLogging::get(opCtx)->logChange(
        opCtx, str::stream() << "refineCollectionShardKey." << eventStr, nss, obj);
}

std::vector<ShardId> getShardsWithDataForCollection(OperationContext* opCtx,
                                                    const NamespaceString& nss) {
    // Do a refresh to get the latest routing information.
    const auto cm = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getCollectionPlacementInfoWithRefresh(opCtx, nss));
    AutoGetCollection col(opCtx, nss, MODE_IS);
    std::set<ShardId> vecsSet;
    cm.getAllShardIds(&vecsSet);
    return std::vector<ShardId>(vecsSet.begin(), vecsSet.end());
}
}  // namespace

RefineCollectionShardKeyCoordinator::RefineCollectionShardKeyCoordinator(
    ShardingDDLCoordinatorService* service, const BSONObj& initialState)
    : RecoverableShardingDDLCoordinator(
          service, "RefineCollectionShardKeyCoordinator", initialState),
      _request(_doc.getRefineCollectionShardKeyRequest()),
      _critSecReason(BSON("command" << "refineCollectionShardKey"
                                    << "ns"
                                    << NamespaceStringUtil::serialize(
                                           nss(), SerializationContext::stateDefault()))) {}

void RefineCollectionShardKeyCoordinator::checkIfOptionsConflict(const BSONObj& doc) const {
    // If we have two refine collections on the same namespace, then the arguments must be the same.
    const auto otherDoc = RefineCollectionShardKeyCoordinatorDocument::parse(
        doc, IDLParserContext("RefineCollectionShardKeyCoordinatorDocument"));

    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Another refine collection with different arguments is already running for the same "
            "namespace",
            SimpleBSONObjComparator::kInstance.evaluate(
                _request.toBSON() == otherDoc.getRefineCollectionShardKeyRequest().toBSON()));
}

void RefineCollectionShardKeyCoordinator::appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const {
    cmdInfoBuilder->appendElements(_request.toBSON());
}

void RefineCollectionShardKeyCoordinator::_performNoopWriteOnDataShardsAndConfigServer(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const OperationSessionInfo& osi,
    const std::shared_ptr<executor::TaskExecutor>& executor) {
    auto shards = getShardsWithDataForCollection(opCtx, nss);
    shards.push_back(Grid::get(opCtx)->shardRegistry()->getConfigShard()->getId());
    sharding_ddl_util::performNoopRetryableWriteOnShards(opCtx, shards, osi, executor);
}

ExecutorFuture<void> RefineCollectionShardKeyCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, anchor = shared_from_this()] {
            // Run local checks.
            if (_doc.getPhase() < Phase::kRemoteIndexValidation) {
                auto opCtxHolder = makeOperationContext();
                auto* opCtx = opCtxHolder.get();

                // Make sure the latest placement version is recovered as of the time of the
                // invocation of the command.
                uassertStatusOK(
                    FilteringMetadataCache::get(opCtx)->onCollectionPlacementVersionMismatch(
                        opCtx, nss(), boost::none));
                {
                    AutoGetCollection coll{
                        opCtx,
                        nss(),
                        MODE_IS,
                        AutoGetCollection::Options{}
                            .viewMode(auto_get_collection::ViewMode::kViewsPermitted)
                            .expectedUUID(_request.getCollectionUUID())};

                    uassert(ErrorCodes::NamespaceNotFound,
                            str::stream() << "RefineCollectionShardKey: collection "
                                          << nss().toStringForErrorMsg() << " does not exists",
                            coll);

                    _doc.setUuid(coll->uuid());

                    const auto scopedCsr =
                        CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(opCtx,
                                                                                          nss());
                    auto metadata = scopedCsr->getCurrentMetadataIfKnown();
                    uassert(ErrorCodes::NamespaceNotSharded,
                            str::stream()
                                << "Can't execute refineCollectionShardKey on unsharded collection "
                                << nss().toStringForErrorMsg(),
                            metadata && metadata->isSharded());
                    _doc.setOldKey(
                        metadata->getChunkManager()->getShardKeyPattern().getKeyPattern());

                    // No need to keep going if the shard key is already refined.
                    if (SimpleBSONObjComparator::kInstance.evaluate(
                            _doc.getOldKey()->toBSON() == _doc.getNewShardKey().toBSON())) {
                        uasserted(ErrorCodes::RequestAlreadyFulfilled,
                                  str::stream() << "Collection " << nss().toStringForErrorMsg()
                                                << " already refined");
                    }
                    _doc.setOldEpoch(metadata->getChunkManager()->getVersion().epoch());
                    _doc.setOldTimestamp(metadata->getChunkManager()->getVersion().getTimestamp());
                }

                // Validate the given shard key extends the current shard key.
                uassert(ErrorCodes::InvalidOptions,
                        str::stream() << "refineCollectionShardKey shard key "
                                      << _doc.getNewShardKey().toString()
                                      << " does not extend the current shard key "
                                      << _doc.getOldKey()->toString(),
                        ShardKeyPattern(_doc.getOldKey()->toBSON())
                            .isExtendedBy(ShardKeyPattern(_doc.getNewShardKey().toBSON())));
            }
        })
        .then(_buildPhaseHandler(
            Phase::kRemoteIndexValidation,
            [this, token, anchor = shared_from_this(), executor](auto* opCtx) {
                if (!_firstExecution) {
                    const auto session = getNewSession(opCtx);
                    _performNoopWriteOnDataShardsAndConfigServer(opCtx, nss(), session, **executor);
                }

                // Stop migrations before checking indexes considering any concurrent index
                // creation/drop with migrations could leave the cluster with inconsistent indexes,
                // PM-2077 should address that.
                {
                    const auto session = getNewSession(opCtx);
                    sharding_ddl_util::stopMigrations(
                        opCtx, nss(), _request.getCollectionUUID(), session);
                }

                const auto& ns = nss();
                auto const shardsWithData = getShardsWithDataForCollection(opCtx, ns);

                // fetch the collection metadata and install it on each shard
                if (feature_flags::gShardAuthoritativeCollMetadata.isEnabled(
                        VersionContext::getDecoration(opCtx),
                        serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                    LOGV2_INFO(
                        10303102,
                        "Fetching and installing collection and chunks metadata on all shards",
                        "ns"_attr = ns);

                    const auto session = getNewSession(opCtx);
                    sharding_ddl_util::sendFetchCollMetadataToShards(
                        opCtx, ns, shardsWithData, session, executor, token);
                }

                sharding::router::CollectionRouter router(opCtx->getServiceContext(), ns);
                router.route(opCtx,
                             "validating indexes for refineCollectionShardKey"_sd,
                             [&](OperationContext* opCtx, const CollectionRoutingInfo& cri) {
                                 ShardsvrValidateShardKeyCandidate validateRequest(ns);
                                 validateRequest.setKey(_doc.getNewShardKey());
                                 validateRequest.setEnforceUniquenessCheck(
                                     _request.getEnforceUniquenessCheck());
                                 validateRequest.setDbName(DatabaseName::kAdmin);

                                 sharding_util::sendCommandToShardsWithVersion(
                                     opCtx,
                                     ns.dbName(),
                                     validateRequest.toBSON(),
                                     shardsWithData,
                                     **executor,
                                     cri,
                                     true /* throwOnError */);
                             });
            }))
        .then(_buildPhaseHandler(
            Phase::kBlockCrud,
            [this, token, anchor = shared_from_this(), executor](auto* opCtx) {
                if (!_firstExecution) {
                    const auto session = getNewSession(opCtx);
                    _performNoopWriteOnDataShardsAndConfigServer(opCtx, nss(), session, **executor);
                }

                ShardsvrParticipantBlock blockCRUDOperationsRequest(nss());
                blockCRUDOperationsRequest.setBlockType(
                    CriticalSectionBlockTypeEnum::kReadsAndWrites);
                blockCRUDOperationsRequest.setReason(_critSecReason);
                generic_argument_util::setMajorityWriteConcern(blockCRUDOperationsRequest);
                generic_argument_util::setOperationSessionInfo(blockCRUDOperationsRequest,
                                                               getNewSession(opCtx));
                auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrParticipantBlock>>(
                    **executor, token, blockCRUDOperationsRequest);
                sharding_ddl_util::sendAuthenticatedCommandToShards(
                    opCtx, opts, getShardsWithDataForCollection(opCtx, nss()));

                // Once there are no writes in the cluster, select an epoch and a timestamp.
                if (!_doc.getNewEpoch()) {
                    _doc.setNewEpoch(OID::gen());
                }
                if (!_doc.getNewTimestamp()) {
                    _doc.setNewTimestamp([opCtx] {
                        VectorClock::VectorTime vt = VectorClock::get(opCtx)->getTime();
                        return vt.clusterTime().asTimestamp();
                    }());
                }
                logRefineCollectionShardKey(opCtx,
                                            nss(),
                                            "start",
                                            BSON("oldKey" << _doc.getOldKey()->toBSON() << "newKey"
                                                          << _doc.getNewShardKey().toBSON()
                                                          << "oldEpoch" << *_doc.getOldEpoch()
                                                          << "newEpoch" << *_doc.getNewEpoch()));
            }))
        .then(_buildPhaseHandler(
            Phase::kCommit,
            [this, token, anchor = shared_from_this(), executor](auto* opCtx) {
                if (!_firstExecution) {
                    const auto session = getNewSession(opCtx);
                    _performNoopWriteOnDataShardsAndConfigServer(opCtx, nss(), session, **executor);
                }

                ConfigsvrCommitRefineCollectionShardKey commitRequest(nss());

                CommitRefineCollectionShardKeyRequest cRCSreq(_doc.getNewShardKey(),
                                                              *_doc.getNewEpoch(),
                                                              *_doc.getNewTimestamp(),
                                                              *_doc.getOldTimestamp());
                commitRequest.setDbName(DatabaseName::kAdmin);
                commitRequest.setCommitRefineCollectionShardKeyRequest(cRCSreq);
                generic_argument_util::setMajorityWriteConcern(commitRequest);

                auto commitResponse =
                    Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommand(
                        opCtx,
                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                        DatabaseName::kAdmin,
                        commitRequest.toBSON(),
                        Shard::RetryPolicy::kIdempotent);

                uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(commitResponse));

                // Checkpoint the configTime to ensure that, in the case of a stepdown, the new
                // primary will start-up from a configTime that is inclusive of the metadata
                // removable that was committed during the critical section.
                VectorClockMutable::get(opCtx)->waitForDurableConfigTime().get(opCtx);
            }))
        .then(_buildPhaseHandler(
            Phase::kReleaseCritSec,
            [this, token, anchor = shared_from_this(), executor](auto* opCtx) {
                if (!_firstExecution) {
                    const auto session = getNewSession(opCtx);
                    _performNoopWriteOnDataShardsAndConfigServer(opCtx, nss(), session, **executor);
                }

                ShardsvrParticipantBlock unblockCRUDOperationsRequest(nss());
                unblockCRUDOperationsRequest.setBlockType(CriticalSectionBlockTypeEnum::kUnblock);
                unblockCRUDOperationsRequest.setReason(_critSecReason);
                unblockCRUDOperationsRequest.setClearFilteringMetadata(true);

                generic_argument_util::setMajorityWriteConcern(unblockCRUDOperationsRequest);
                generic_argument_util::setOperationSessionInfo(unblockCRUDOperationsRequest,
                                                               getNewSession(opCtx));
                auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrParticipantBlock>>(
                    **executor, token, unblockCRUDOperationsRequest);
                sharding_ddl_util::sendAuthenticatedCommandToShards(
                    opCtx, opts, getShardsWithDataForCollection(opCtx, nss()));
            }))
        .then(_buildPhaseHandler(
            Phase::kResumeMigrations,
            [this, token, anchor = shared_from_this(), executor](auto* opCtx) {
                notifyChangeStreamsOnRefineCollectionShardKeyComplete(
                    opCtx, nss(), _doc.getNewShardKey(), _doc.getOldKey().get(), *_doc.getUuid());
                {
                    const auto session = getNewSession(opCtx);
                    sharding_ddl_util::resumeMigrations(opCtx, nss(), boost::none, session);
                }

                logRefineCollectionShardKey(opCtx, nss(), "end", BSONObj());
            }))
        .then([this, anchor = shared_from_this(), executor] {
            auto opCtxHolder = makeOperationContext();
            auto* opCtx = opCtxHolder.get();

            // Refresh all shards so cache is warmed up for queries.
            sharding_util::tellShardsToRefreshCollection(
                opCtx, getShardsWithDataForCollection(opCtx, nss()), nss(), **executor);
        })
        .onCompletion([this, anchor = shared_from_this()](const Status& status) {
            auto opCtxHolder = makeOperationContext();
            auto* opCtx = opCtxHolder.get();

            Status finalStatus =
                status == ErrorCodes::RequestAlreadyFulfilled ? Status::OK() : status;

            if (status == ErrorCodes::RequestAlreadyFulfilled) {
                notifyChangeStreamsOnRefineCollectionShardKeyComplete(
                    opCtx, nss(), _doc.getNewShardKey(), _doc.getOldKey().get(), *_doc.getUuid());
            }

            // If a non retriable error during index validation occurs we will end the coordinator,
            // so we need to resume migrations just in case we managed to stop them in the first
            // phase.
            if (!finalStatus.isOK() && _doc.getPhase() >= Phase::kRemoteIndexValidation &&
                !_mustAlwaysMakeProgress() && !_isRetriableErrorForDDLCoordinator(finalStatus)) {
                const auto session = getNewSession(opCtx);
                sharding_ddl_util::resumeMigrations(opCtx, nss(), boost::none, session);
            }

            return finalStatus;
        });
}

}  // namespace mongo
