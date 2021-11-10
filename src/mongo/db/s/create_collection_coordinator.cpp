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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands/create_gen.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/create_collection_coordinator.h"
#include "mongo/db/s/recoverable_critical_section_service.h"
#include "mongo/db/s/remove_chunks_gen.h"
#include "mongo/db/s/shard_key_util.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/timeseries/catalog_helper.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/cluster_write.h"
#include "mongo/s/grid.h"
#include "mongo/s/long_collection_names_gen.h"

namespace mongo {
namespace {

struct OptionsAndIndexes {
    BSONObj options;
    std::vector<BSONObj> indexSpecs;
    BSONObj idIndexSpec;
};

OptionsAndIndexes getCollectionOptionsAndIndexes(OperationContext* opCtx,
                                                 const NamespaceStringOrUUID& nssOrUUID) {
    DBDirectClient localClient(opCtx);
    BSONObj idIndex;
    BSONObjBuilder optionsBob;

    auto all =
        localClient.getCollectionInfos(nssOrUUID.dbname(), BSON("info.uuid" << *nssOrUUID.uuid()));

    // There must be a collection at this time.
    invariant(!all.empty());
    auto& entry = all.front();

    if (entry["options"].isABSONObj()) {
        optionsBob.appendElements(entry["options"].Obj());
    }
    optionsBob.append(entry["info"]["uuid"]);
    if (entry["idIndex"]) {
        idIndex = entry["idIndex"].Obj().getOwned();
    }

    auto indexSpecsList = localClient.getIndexSpecs(nssOrUUID, false, 0);

    return {optionsBob.obj(),
            std::vector<BSONObj>(std::begin(indexSpecsList), std::end(indexSpecsList)),
            idIndex};
}

/**
 * Constructs the BSON specification document for the create collections command using the given
 * namespace, collation, and timeseries options.
 */
BSONObj makeCreateCommand(const NamespaceString& nss,
                          const boost::optional<Collation>& collation,
                          const TimeseriesOptions& tsOpts) {
    CreateCommand create(nss);
    create.setTimeseries(tsOpts);
    if (collation) {
        create.setCollation(*collation);
    }
    BSONObj commandPassthroughFields;
    return create.toBSON(commandPassthroughFields);
}

/**
 * Compares the proposed shard key with the shard key of the collection's existing zones
 * to ensure they are a legal combination.
 */
void validateShardKeyAgainstExistingZones(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const BSONObj& proposedKey,
                                          const std::vector<TagsType>& tags) {
    const AutoGetCollection coll(opCtx, nss, MODE_IS);
    for (const auto& tag : tags) {
        BSONObjIterator tagMinFields(tag.getMinKey());
        BSONObjIterator tagMaxFields(tag.getMaxKey());
        BSONObjIterator proposedFields(proposedKey);

        while (tagMinFields.more() && proposedFields.more()) {
            BSONElement tagMinKeyElement = tagMinFields.next();
            BSONElement tagMaxKeyElement = tagMaxFields.next();
            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << "the min and max of the existing zone " << tag.getMinKey()
                                  << " -->> " << tag.getMaxKey() << " have non-matching keys",
                    tagMinKeyElement.fieldNameStringData() ==
                        tagMaxKeyElement.fieldNameStringData());

            BSONElement proposedKeyElement = proposedFields.next();
            bool match = ((tagMinKeyElement.fieldNameStringData() ==
                           proposedKeyElement.fieldNameStringData()) &&
                          ((tagMinFields.more() && proposedFields.more()) ||
                           (!tagMinFields.more() && !proposedFields.more())));
            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << "the proposed shard key " << proposedKey.toString()
                                  << " does not match with the shard key of the existing zone "
                                  << tag.getMinKey() << " -->> " << tag.getMaxKey(),
                    match);

            // If the field is hashed, make sure that the min and max values are of supported type.
            uassert(
                ErrorCodes::InvalidOptions,
                str::stream() << "cannot do hash sharding with the proposed key "
                              << proposedKey.toString() << " because there exists a zone "
                              << tag.getMinKey() << " -->> " << tag.getMaxKey()
                              << " whose boundaries are not of type NumberLong, MinKey or MaxKey",
                !ShardKeyPattern::isHashedPatternEl(proposedKeyElement) ||
                    (ShardKeyPattern::isValidHashedValue(tagMinKeyElement) &&
                     ShardKeyPattern::isValidHashedValue(tagMaxKeyElement)));

            if (coll && coll->getTimeseriesOptions()) {
                const std::string controlTimeField =
                    timeseries::kControlMinFieldNamePrefix.toString() +
                    coll->getTimeseriesOptions()->getTimeField();
                if (tagMinKeyElement.fieldNameStringData() == controlTimeField) {
                    uassert(ErrorCodes::InvalidOptions,
                            str::stream() << "time field cannot be specified in the zone range for "
                                             "time-series collections",
                            tagMinKeyElement.type() == MinKey);
                }
                if (tagMaxKeyElement.fieldNameStringData() == controlTimeField) {
                    uassert(ErrorCodes::InvalidOptions,
                            str::stream() << "time field cannot be specified in the zone range for "
                                             "time-series collections",
                            tagMaxKeyElement.type() == MinKey);
                }
            }
        }
    }
}

std::vector<TagsType> getTagsAndValidate(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         const BSONObj& proposedKey) {
    // Read zone info
    const auto catalogClient = Grid::get(opCtx)->catalogClient();
    auto tags = uassertStatusOK(catalogClient->getTagsForCollection(opCtx, nss));

    if (!tags.empty()) {
        validateShardKeyAgainstExistingZones(opCtx, nss, proposedKey, tags);
    }

    return tags;
}

bool checkIfCollectionIsEmpty(OperationContext* opCtx, const NamespaceString& nss) {
    // Use find with predicate instead of count in order to ensure that the count
    // command doesn't just consult the cached metadata, which may not always be
    // correct
    DBDirectClient localClient(opCtx);
    return localClient.findOne(nss.ns(), BSONObj{}).isEmpty();
}

int getNumShards(OperationContext* opCtx) {
    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
    shardRegistry->reload(opCtx);

    return shardRegistry->getNumShards(opCtx);
}

std::pair<boost::optional<Collation>, BSONObj> getCollation(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const boost::optional<BSONObj>& collation) {
    // Ensure the collation is valid. Currently we only allow the simple collation.
    std::unique_ptr<CollatorInterface> requestedCollator = nullptr;
    if (collation) {
        requestedCollator =
            uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                ->makeFromBSON(collation.value()));
        uassert(ErrorCodes::BadValue,
                str::stream() << "The collation for shardCollection must be {locale: 'simple'}, "
                              << "but found: " << collation.value(),
                !requestedCollator);
    }

    AutoGetCollection autoColl(opCtx, nss, MODE_IS, AutoGetCollectionViewMode::kViewsForbidden);

    const auto actualCollator = [&]() -> const CollatorInterface* {
        const auto& coll = autoColl.getCollection();
        if (coll) {
            uassert(
                ErrorCodes::InvalidOptions, "can't shard a capped collection", !coll->isCapped());
            return coll->getDefaultCollator();
        }

        return nullptr;
    }();

    if (!requestedCollator && !actualCollator)
        return {boost::none, BSONObj()};

    auto actualCollation = actualCollator->getSpec();
    auto actualCollatorBSON = actualCollation.toBSON();

    if (!collation) {
        auto actualCollatorFilter =
            uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                ->makeFromBSON(actualCollatorBSON));
        uassert(ErrorCodes::BadValue,
                str::stream() << "If no collation was specified, the collection collation must be "
                                 "{locale: 'simple'}, "
                              << "but found: " << actualCollatorBSON,
                !actualCollatorFilter);
    }

    return {actualCollation, actualCollatorBSON};
}

void cleanupPartialChunksFromPreviousAttempt(OperationContext* opCtx,
                                             const UUID& uuid,
                                             const OperationSessionInfo& osi) {
    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    // Remove the chunks matching uuid
    ConfigsvrRemoveChunks configsvrRemoveChunksCmd(uuid);
    configsvrRemoveChunksCmd.setDbName(NamespaceString::kAdminDb);

    const auto swRemoveChunksResult = configShard->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        NamespaceString::kAdminDb.toString(),
        CommandHelpers::appendMajorityWriteConcern(configsvrRemoveChunksCmd.toBSON(osi.toBSON())),
        Shard::RetryPolicy::kIdempotent);

    uassertStatusOKWithContext(
        Shard::CommandResponse::getEffectiveStatus(std::move(swRemoveChunksResult)),
        str::stream() << "Error removing chunks matching uuid " << uuid);
}

void insertChunks(OperationContext* opCtx,
                  std::vector<ChunkType>& chunks,
                  const OperationSessionInfo& osi) {
    BatchedCommandRequest insertRequest([&]() {
        write_ops::InsertCommandRequest insertOp(ChunkType::ConfigNS);
        std::vector<BSONObj> entries;
        entries.reserve(chunks.size());
        for (const auto& chunk : chunks) {
            entries.push_back(chunk.toConfigBSON());
        }
        insertOp.setDocuments(entries);
        return insertOp;
    }());

    insertRequest.setWriteConcern(ShardingCatalogClient::kMajorityWriteConcern.toBSON());
    {
        auto newClient =
            opCtx->getServiceContext()->makeClient("CreateCollectionCoordinator::insertChunks");
        {
            stdx::lock_guard<Client> lk(*newClient.get());
            newClient->setSystemOperationKillableByStepdown(lk);
        }

        AlternativeClientRegion acr(newClient);
        auto executor =
            Grid::get(opCtx->getServiceContext())->getExecutorPool()->getFixedExecutor();
        auto newOpCtx = CancelableOperationContext(
            cc().makeOperationContext(), opCtx->getCancellationToken(), executor);
        newOpCtx->setLogicalSessionId(*osi.getSessionId());
        newOpCtx->setTxnNumber(*osi.getTxnNumber());

        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        cluster::write(newOpCtx.get(), insertRequest, &stats, &response);
        uassertStatusOK(response.toStatus());
    }
}

void updateCatalogEntry(OperationContext* opCtx,
                        const NamespaceString& nss,
                        CollectionType& coll,
                        const OperationSessionInfo& osi) {
    const auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    BatchedCommandRequest updateRequest([&]() {
        write_ops::UpdateCommandRequest updateOp(CollectionType::ConfigNS);
        updateOp.setUpdates({[&] {
            write_ops::UpdateOpEntry entry;
            entry.setQ(BSON(CollectionType::kNssFieldName << nss.ns()));
            entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(coll.toBSON()));
            entry.setUpsert(true);
            entry.setMulti(false);
            return entry;
        }()});
        return updateOp;
    }());

    updateRequest.setWriteConcern(ShardingCatalogClient::kMajorityWriteConcern.toBSON());
    const BSONObj cmdObj = updateRequest.toBSON().addFields(osi.toBSON());

    try {
        BatchedCommandResponse batchResponse;
        const auto response =
            configShard->runCommand(opCtx,
                                    ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                    CollectionType::ConfigNS.db().toString(),
                                    cmdObj,
                                    Shard::kDefaultConfigCommandTimeout,
                                    Shard::RetryPolicy::kIdempotent);

        const auto writeStatus =
            Shard::CommandResponse::processBatchWriteResponse(response, &batchResponse);

        uassertStatusOK(batchResponse.toStatus());
        uassertStatusOK(writeStatus);
    } catch (const DBException&) {
        // If an error happens when contacting the config server, we don't know if the update
        // succeeded or not, which might cause the local shard version to differ from the config
        // server, so we clear the metadata to allow another operation to refresh it.
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        AutoGetCollection autoColl(opCtx, nss, MODE_IX);
        CollectionShardingRuntime::get(opCtx, nss)->clearFilteringMetadata(opCtx);
        throw;
    }
}

void broadcastDropCollection(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const std::shared_ptr<executor::TaskExecutor>& executor,
                             const OperationSessionInfo& osi) {
    const auto primaryShardId = ShardingState::get(opCtx)->shardId();
    const ShardsvrDropCollectionParticipant dropCollectionParticipant(nss);

    auto participants = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
    // Remove primary shard from participants
    participants.erase(std::remove(participants.begin(), participants.end(), primaryShardId),
                       participants.end());

    sharding_ddl_util::sendDropCollectionParticipantCommandToShards(
        opCtx, nss, participants, executor, osi);
}

}  // namespace

CreateCollectionCoordinator::CreateCollectionCoordinator(ShardingDDLCoordinatorService* service,
                                                         const BSONObj& initialState)
    : ShardingDDLCoordinator(service, initialState),
      _doc(CreateCollectionCoordinatorDocument::parse(
          IDLParserErrorContext("CreateCollectionCoordinatorDocument"), initialState)),
      _critSecReason(BSON("command"
                          << "createCollection"
                          << "ns" << nss().toString() << "request"
                          << _doc.getCreateCollectionRequest().toBSON())) {}

boost::optional<BSONObj> CreateCollectionCoordinator::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept {
    BSONObjBuilder cmdBob;
    if (const auto& optComment = getForwardableOpMetadata().getComment()) {
        cmdBob.append(optComment.get().firstElement());
    }
    cmdBob.appendElements(_doc.getCreateCollectionRequest().toBSON());

    BSONObjBuilder bob;
    bob.append("type", "op");
    bob.append("desc", "CreateCollectionCoordinator");
    bob.append("op", "command");
    bob.append("ns", nss().toString());
    bob.append("command", cmdBob.obj());
    bob.append("currentPhase", _doc.getPhase());
    bob.append("active", true);
    return bob.obj();
}

void CreateCollectionCoordinator::checkIfOptionsConflict(const BSONObj& doc) const {
    // If we have two shard collections on the same namespace, then the arguments must be the same.
    const auto otherDoc = CreateCollectionCoordinatorDocument::parse(
        IDLParserErrorContext("CreateCollectionCoordinatorDocument"), doc);

    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Another create collection with different arguments is already running for the same "
            "namespace",
            SimpleBSONObjComparator::kInstance.evaluate(
                _doc.getCreateCollectionRequest().toBSON() ==
                otherDoc.getCreateCollectionRequest().toBSON()));
}

ExecutorFuture<void> CreateCollectionCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, anchor = shared_from_this()] {
            _shardKeyPattern = ShardKeyPattern(*_doc.getShardKey());
            if (_doc.getPhase() < Phase::kCommit) {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                _checkCommandArguments(opCtx);
            }
        })
        .then(_executePhase(
            Phase::kCommit,
            [this, executor = executor, token, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                if (!_firstExecution) {
                    // Perform a noop write on the participants in order to advance the txnNumber
                    // for this coordinator's lsid so that requests with older txnNumbers can no
                    // longer execute.
                    _performNoopRetryableWriteOnParticipants(opCtx, **executor);
                }

                if (_recoveredFromDisk) {
                    // If a stedown happened it could've ocurred while waiting for majority when
                    // writing config.collections. If the refresh happens before this write is
                    // majority committed, we will only see the data on config.chunks but not on
                    // config.collections, so we need to serialize the refresh with the collection
                    // creation.
                    sharding_ddl_util::linearizeCSRSReads(opCtx);
                }
                // Log the start of the event only if we're not recovering.
                _logStartCreateCollection(opCtx);

                // Quick check (without critical section) to see if another create collection
                // already succeeded.
                if (auto createCollectionResponseOpt =
                        sharding_ddl_util::checkIfCollectionAlreadySharded(
                            opCtx,
                            nss(),
                            _shardKeyPattern->getKeyPattern().toBSON(),
                            getCollation(opCtx, nss(), _doc.getCollation()).second,
                            _doc.getUnique().value_or(false))) {
                    _result = createCollectionResponseOpt;
                    // The collection was already created and commited but there was a
                    // stepdown after the commit.
                    RecoverableCriticalSectionService::get(opCtx)
                        ->releaseRecoverableCriticalSection(
                            opCtx,
                            nss(),
                            _critSecReason,
                            ShardingCatalogClient::kMajorityWriteConcern);
                    return;
                }

                // Entering the critical section. From this point on, the writes are blocked. Before
                // calling this method, we need the coordinator document to be persisted (and hence
                // the kCheck state), otherwise nothing will release the critical section in the
                // presence of a stepdown.
                RecoverableCriticalSectionService::get(opCtx)
                    ->acquireRecoverableCriticalSectionBlockWrites(
                        opCtx, nss(), _critSecReason, ShardingCatalogClient::kMajorityWriteConcern);

                if (_recoveredFromDisk) {
                    auto uuid = sharding_ddl_util::getCollectionUUID(opCtx, nss());
                    // If the collection can be found locally, then we clean up the config.chunks
                    // collection.
                    if (uuid) {
                        LOGV2_DEBUG(5458704,
                                    1,
                                    "Removing partial changes from previous run",
                                    "namespace"_attr = nss());

                        _doc = _updateSession(opCtx, _doc);
                        cleanupPartialChunksFromPreviousAttempt(
                            opCtx, *uuid, getCurrentSession(_doc));

                        _doc = _updateSession(opCtx, _doc);
                        broadcastDropCollection(opCtx, nss(), **executor, getCurrentSession(_doc));
                    }
                }

                _createPolicy(opCtx);
                _createCollectionAndIndexes(opCtx);

                audit::logShardCollection(opCtx->getClient(),
                                          nss().ns(),
                                          *_doc.getShardKey(),
                                          _doc.getUnique().value_or(false));

                if (_splitPolicy->isOptimized()) {
                    _createChunks(opCtx);
                    // Block reads/writes from here on if we need to create
                    // the collection on other shards, this way we prevent
                    // reads/writes that should be redirected to another
                    // shard.
                    RecoverableCriticalSectionService::get(opCtx)
                        ->promoteRecoverableCriticalSectionToBlockAlsoReads(
                            opCtx,
                            nss(),
                            _critSecReason,
                            ShardingCatalogClient::kMajorityWriteConcern);

                    _doc = _updateSession(opCtx, _doc);
                    try {
                        _createCollectionOnNonPrimaryShards(opCtx, getCurrentSession(_doc));
                    } catch (const ExceptionFor<ErrorCodes::NotARetryableWriteCommand>&) {
                        // Older 5.0 binaries don't support running the
                        // _shardsvrCreateCollectionParticipant command as a retryable write yet. In
                        // that case, retry without attaching session info.
                        _createCollectionOnNonPrimaryShards(opCtx, boost::none);
                    }

                    _commit(opCtx);
                }

                // End of the critical section, from now on, read and writes are permitted.
                RecoverableCriticalSectionService::get(opCtx)->releaseRecoverableCriticalSection(
                    opCtx, nss(), _critSecReason, ShardingCatalogClient::kMajorityWriteConcern);

                // Slow path. Create chunks (which might incur in an index scan) and commit must be
                // done outside of the critical section to prevent writes from stalling in unsharded
                // collections.
                if (!_splitPolicy->isOptimized()) {
                    _createChunks(opCtx);

                    _commit(opCtx);
                }

                _finalize(opCtx);
            }))
        .then([this] {
            auto opCtxHolder = cc().makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            getForwardableOpMetadata().setOn(opCtx);

            _logEndCreateCollection(opCtx);
        })
        .onError([this, anchor = shared_from_this()](const Status& status) {
            if (!status.isA<ErrorCategory::NotPrimaryError>() &&
                !status.isA<ErrorCategory::ShutdownError>()) {
                LOGV2_ERROR(5458702,
                            "Error running create collection",
                            "namespace"_attr = nss(),
                            "error"_attr = redact(status));

                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();

                RecoverableCriticalSectionService::get(opCtx)->releaseRecoverableCriticalSection(
                    opCtx, nss(), _critSecReason, ShardingCatalogClient::kMajorityWriteConcern);
            }
            return status;
        });
}

void CreateCollectionCoordinator::_checkCommandArguments(OperationContext* opCtx) {
    LOGV2_DEBUG(5277902, 2, "Create collection _checkCommandArguments", "namespace"_attr = nss());

    const auto dbEnabledForSharding = [&, this] {
        // The modification of the 'sharded' flag for the db does not imply a database version
        // change so we can't use the DatabaseShardingState to look it up. Instead we will do a
        // first attempt through the catalog cache and if it is unset we will attempt another time
        // after a forced catalog cache refresh.
        auto catalogCache = Grid::get(opCtx)->catalogCache();

        auto dbInfo = uassertStatusOK(catalogCache->getDatabase(opCtx, nss().db()));
        if (!dbInfo.shardingEnabled()) {
            sharding_ddl_util::linearizeCSRSReads(opCtx);
            dbInfo = uassertStatusOK(catalogCache->getDatabaseWithRefresh(opCtx, nss().db()));
        }

        return dbInfo.shardingEnabled();
    }();

    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "sharding not enabled for db " << nss().db(),
            dbEnabledForSharding);

    if (nss().db() == NamespaceString::kConfigDb) {
        // Only allowlisted collections in config may be sharded (unless we are in test mode)
        uassert(ErrorCodes::IllegalOperation,
                "only special collections in the config db may be sharded",
                nss() == NamespaceString::kLogicalSessionsNamespace);
    }

    // Ensure that hashed and unique are not both set.
    uassert(ErrorCodes::InvalidOptions,
            "Hashed shard keys cannot be declared unique. It's possible to ensure uniqueness on "
            "the hashed field by declaring an additional (non-hashed) unique index on the field.",
            !_shardKeyPattern->isHashedPattern() || !_doc.getUnique().value_or(false));

    // Ensure that a time-series collection cannot be sharded unless the feature flag is enabled.
    if (nss().isTimeseriesBucketsCollection()) {
        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "can't shard time-series collection " << nss(),
                feature_flags::gFeatureFlagShardedTimeSeries.isEnabled(
                    serverGlobalParams.featureCompatibility) ||
                    !timeseries::getTimeseriesOptions(opCtx, nss(), false));
    }

    // Ensure the namespace is valid.
    uassert(ErrorCodes::IllegalOperation,
            "can't shard system namespaces",
            !nss().isSystem() || nss() == NamespaceString::kLogicalSessionsNamespace ||
                nss().isTemporaryReshardingCollection() || nss().isTimeseriesBucketsCollection());

    if (_doc.getNumInitialChunks()) {
        // Ensure numInitialChunks is within valid bounds.
        // Cannot have more than 8192 initial chunks per shard. Setting a maximum of 1,000,000
        // chunks in total to limit the amount of memory this command consumes so there is less
        // danger of an OOM error.

        const int maxNumInitialChunksForShards =
            Grid::get(opCtx)->shardRegistry()->getNumShardsNoReload() * 8192;
        const int maxNumInitialChunksTotal = 1000 * 1000;  // Arbitrary limit to memory consumption
        int numChunks = _doc.getNumInitialChunks().value();
        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "numInitialChunks cannot be more than either: "
                              << maxNumInitialChunksForShards << ", 8192 * number of shards; or "
                              << maxNumInitialChunksTotal,
                numChunks >= 0 && numChunks <= maxNumInitialChunksForShards &&
                    numChunks <= maxNumInitialChunksTotal);
    }

    if (nss().db() == NamespaceString::kConfigDb) {
        auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

        auto findReponse = uassertStatusOK(
            configShard->exhaustiveFindOnConfig(opCtx,
                                                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                                repl::ReadConcernLevel::kMajorityReadConcern,
                                                nss(),
                                                BSONObj(),
                                                BSONObj(),
                                                1));

        auto numDocs = findReponse.docs.size();

        // If this is a collection on the config db, it must be empty to be sharded.
        uassert(ErrorCodes::IllegalOperation,
                "collections in the config db must be empty to be sharded",
                numDocs == 0);
    }
}

void CreateCollectionCoordinator::_createCollectionAndIndexes(OperationContext* opCtx) {
    LOGV2_DEBUG(
        5277903, 2, "Create collection _createCollectionAndIndexes", "namespace"_attr = nss());

    boost::optional<Collation> collation;
    std::tie(collation, _collationBSON) = getCollation(opCtx, nss(), _doc.getCollation());

    // We need to implicitly create a timeseries view and underlying bucket collection.
    if (_collectionEmpty && _doc.getTimeseries()) {
        const auto viewName = nss().getTimeseriesViewNamespace();
        auto createCmd = makeCreateCommand(viewName, collation, _doc.getTimeseries().get());

        BSONObj createRes;
        DBDirectClient localClient(opCtx);
        localClient.runCommand(nss().db().toString(), createCmd, createRes);
        auto createStatus = getStatusFromCommandResult(createRes);

        if (!createStatus.isOK() && createStatus.code() == ErrorCodes::NamespaceExists) {
            LOGV2_DEBUG(5909400,
                        3,
                        "Timeseries namespace already exists",
                        "namespace"_attr = viewName.toString());
        } else {
            uassertStatusOK(createStatus);
        }
    }

    const auto indexCreated = shardkeyutil::validateShardKeyIndexExistsOrCreateIfPossible(
        opCtx,
        nss(),
        *_shardKeyPattern,
        _collationBSON,
        _doc.getUnique().value_or(false),
        shardkeyutil::ValidationBehaviorsShardCollection(opCtx));

    auto replClientInfo = repl::ReplClientInfo::forClient(opCtx->getClient());

    if (!indexCreated) {
        replClientInfo.setLastOpToSystemLastOpTime(opCtx);
    }
    // Wait until the index is majority written, to prevent having the collection commited to the
    // config server, but the index creation rolled backed on stepdowns.
    WriteConcernResult ignoreResult;
    uassertStatusOK(waitForWriteConcern(opCtx,
                                        replClientInfo.getLastOp(),
                                        ShardingCatalogClient::kMajorityWriteConcern,
                                        &ignoreResult));

    _collectionUUID = *sharding_ddl_util::getCollectionUUID(opCtx, nss());
}

void CreateCollectionCoordinator::_createPolicy(OperationContext* opCtx) {
    LOGV2_DEBUG(6042001, 2, "Create collection _createPolicy", "namespace"_attr = nss());

    _collectionEmpty = checkIfCollectionIsEmpty(opCtx, nss());

    _splitPolicy = InitialSplitPolicy::calculateOptimizationStrategy(
        opCtx,
        *_shardKeyPattern,
        _doc.getNumInitialChunks() ? *_doc.getNumInitialChunks() : 0,
        _doc.getPresplitHashedZones() ? *_doc.getPresplitHashedZones() : false,
        _doc.getInitialSplitPoints(),
        getTagsAndValidate(opCtx, nss(), _shardKeyPattern->toBSON()),
        getNumShards(opCtx),
        *_collectionEmpty);
}

void CreateCollectionCoordinator::_createChunks(OperationContext* opCtx) {
    LOGV2_DEBUG(5277904, 2, "Create collection _createChunks", "namespace"_attr = nss());

    _initialChunks = _splitPolicy->createFirstChunks(
        opCtx, *_shardKeyPattern, {*_collectionUUID, ShardingState::get(opCtx)->shardId()});

    // There must be at least one chunk.
    invariant(!_initialChunks.chunks.empty());

    _numChunks = _initialChunks.chunks.size();
}

void CreateCollectionCoordinator::_createCollectionOnNonPrimaryShards(
    OperationContext* opCtx, const boost::optional<OperationSessionInfo>& osi) {
    LOGV2_DEBUG(5277905,
                2,
                "Create collection _createCollectionOnNonPrimaryShards",
                "namespace"_attr = nss());

    std::vector<AsyncRequestsSender::Request> requests;
    std::set<ShardId> initializedShards;
    auto dbPrimaryShardId = ShardingState::get(opCtx)->shardId();

    NamespaceStringOrUUID nssOrUUID{nss().db().toString(), *_collectionUUID};
    auto [collOptions, indexes, idIndex] = getCollectionOptionsAndIndexes(opCtx, nssOrUUID);

    for (const auto& chunk : _initialChunks.chunks) {
        const auto& chunkShardId = chunk.getShard();
        if (chunkShardId == dbPrimaryShardId ||
            initializedShards.find(chunkShardId) != initializedShards.end()) {
            continue;
        }

        ShardsvrCreateCollectionParticipant createCollectionParticipantRequest(nss());
        createCollectionParticipantRequest.setCollectionUUID(*_collectionUUID);

        createCollectionParticipantRequest.setOptions(collOptions);
        createCollectionParticipantRequest.setIdIndex(idIndex);
        createCollectionParticipantRequest.setIndexes(indexes);

        requests.emplace_back(
            chunkShardId,
            CommandHelpers::appendMajorityWriteConcern(
                createCollectionParticipantRequest.toBSON(osi ? osi->toBSON() : BSONObj())));

        initializedShards.emplace(chunkShardId);
    }

    if (!requests.empty()) {
        auto responses = gatherResponses(opCtx,
                                         nss().db(),
                                         ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                         Shard::RetryPolicy::kIdempotent,
                                         requests);

        // If any shards fail to create the collection, fail the entire shardCollection command
        // (potentially leaving incomplely created sharded collection)
        for (const auto& response : responses) {
            auto shardResponse = uassertStatusOKWithContext(
                std::move(response.swResponse),
                str::stream() << "Unable to create collection " << nss().ns() << " on "
                              << response.shardId);
            auto status = getStatusFromCommandResult(shardResponse.data);
            uassertStatusOK(status.withContext(str::stream()
                                               << "Unable to create collection " << nss().ns()
                                               << " on " << response.shardId));

            auto wcStatus = getWriteConcernStatusFromCommandResult(shardResponse.data);
            uassertStatusOK(wcStatus.withContext(str::stream()
                                                 << "Unable to create collection " << nss().ns()
                                                 << " on " << response.shardId));
        }
    }
}

void CreateCollectionCoordinator::_commit(OperationContext* opCtx) {
    LOGV2_DEBUG(5277906, 2, "Create collection _commit", "namespace"_attr = nss());

    // Upsert Chunks.
    _doc = _updateSession(opCtx, _doc);
    insertChunks(opCtx, _initialChunks.chunks, getCurrentSession(_doc));

    CollectionType coll(nss(),
                        _initialChunks.collVersion().epoch(),
                        _initialChunks.collVersion().getTimestamp(),
                        Date_t::now(),
                        *_collectionUUID);

    coll.setKeyPattern(_shardKeyPattern->getKeyPattern());

    // Prevent the FCV from changing before committing the new collection to the config server.
    // This ensures that the 'supportingLongName' field is properly set (and committed) based on
    // the current shard's FCV.
    //
    // TODO: Remove once FCV 6.0 becomes last-lts
    std::shared_ptr<FixedFCVRegion> currentFCV;

    // TODO: Remove condition once FCV 6.0 becomes last-lts
    if (feature_flags::gFeatureFlagLongCollectionNames.isEnabledAndIgnoreFCV()) {
        currentFCV = std::make_shared<FixedFCVRegion>(opCtx);
        if ((*currentFCV)
                ->isGreaterThanOrEqualTo(
                    multiversion::FeatureCompatibilityVersion::kUpgradingFrom_5_0_To_5_1)) {
            coll.setSupportingLongName(SupportingLongNameStatusEnum::kImplicitlyEnabled);
        }
    }

    if (_doc.getCreateCollectionRequest().getTimeseries()) {
        TypeCollectionTimeseriesFields timeseriesFields;
        timeseriesFields.setTimeseriesOptions(*_doc.getCreateCollectionRequest().getTimeseries());
        coll.setTimeseriesFields(std::move(timeseriesFields));
    }

    if (_collationBSON) {
        coll.setDefaultCollation(_collationBSON.value());
    }

    if (_doc.getUnique()) {
        coll.setUnique(*_doc.getUnique());
    }

    _doc = _updateSession(opCtx, _doc);
    updateCatalogEntry(opCtx, nss(), coll, getCurrentSession(_doc));
}

void CreateCollectionCoordinator::_finalize(OperationContext* opCtx) {
    LOGV2_DEBUG(5277907, 2, "Create collection _finalize", "namespace"_attr = nss());

    try {
        forceShardFilteringMetadataRefresh(opCtx, nss());
    } catch (const DBException&) {
        // If the refresh fails, then set the shard version to UNKNOWN and let a future operation to
        // refresh the metadata.
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        AutoGetCollection autoColl(opCtx, nss(), MODE_IX);
        CollectionShardingRuntime::get(opCtx, nss())->clearFilteringMetadata(opCtx);
    }

    // Best effort refresh to warm up cache of all involved shards so we can have a cluster ready to
    // receive operations.
    auto shardRegistry = Grid::get(opCtx)->shardRegistry();
    auto dbPrimaryShardId = ShardingState::get(opCtx)->shardId();

    std::set<ShardId> shardsRefreshed;
    for (const auto& chunk : _initialChunks.chunks) {
        const auto& chunkShardId = chunk.getShard();
        if (chunkShardId == dbPrimaryShardId ||
            shardsRefreshed.find(chunkShardId) != shardsRefreshed.end()) {
            continue;
        }

        auto shard = uassertStatusOK(shardRegistry->getShard(opCtx, chunkShardId));
        shard->runFireAndForgetCommand(opCtx,
                                       ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                       NamespaceString::kAdminDb.toString(),
                                       BSON("_flushRoutingTableCacheUpdates" << nss().ns()));

        shardsRefreshed.emplace(chunkShardId);
    }

    LOGV2(5277901,
          "Created initial chunk(s)",
          "namespace"_attr = nss(),
          "numInitialChunks"_attr = _initialChunks.chunks.size(),
          "initialCollectionVersion"_attr = _initialChunks.collVersion());

    auto result = CreateCollectionResponse(
        _initialChunks.chunks[_initialChunks.chunks.size() - 1].getVersion());
    result.setCollectionUUID(_collectionUUID);
    _result = std::move(result);

    LOGV2(5458701,
          "Collection created",
          "namespace"_attr = nss(),
          "UUID"_attr = _result->getCollectionUUID(),
          "version"_attr = _result->getCollectionVersion());
}

void CreateCollectionCoordinator::_logStartCreateCollection(OperationContext* opCtx) {
    BSONObjBuilder collectionDetail;
    collectionDetail.append("shardKey", *_doc.getCreateCollectionRequest().getShardKey());
    collectionDetail.append("collection", nss().ns());
    collectionDetail.append("primary", ShardingState::get(opCtx)->shardId().toString());
    ShardingLogging::get(opCtx)->logChange(
        opCtx, "shardCollection.start", nss().ns(), collectionDetail.obj());
}

void CreateCollectionCoordinator::_logEndCreateCollection(OperationContext* opCtx) {
    BSONObjBuilder collectionDetail;
    _result->getCollectionUUID()->appendToBuilder(&collectionDetail, "uuid");
    collectionDetail.append("version", _result->getCollectionVersion().toString());
    if (_collectionEmpty)
        collectionDetail.append("empty", *_collectionEmpty);
    if (_numChunks)
        collectionDetail.appendNumber("numChunks", static_cast<long long>(*_numChunks));
    ShardingLogging::get(opCtx)->logChange(
        opCtx, "shardCollection.end", nss().ns(), collectionDetail.obj());
}

void CreateCollectionCoordinator::_performNoopRetryableWriteOnParticipants(
    OperationContext* opCtx, const std::shared_ptr<executor::TaskExecutor>& executor) {
    auto shardsAndConfigsvr = [&] {
        const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
        auto participants = shardRegistry->getAllShardIds(opCtx);
        participants.emplace_back(shardRegistry->getConfigShard()->getId());
        return participants;
    }();

    _doc = _updateSession(opCtx, _doc);
    sharding_ddl_util::performNoopRetryableWriteOnShards(
        opCtx, shardsAndConfigsvr, getCurrentSession(_doc), executor);
}

// Phase change API.

void CreateCollectionCoordinator::_enterPhase(Phase newPhase) {
    CoordDoc newDoc(_doc);
    newDoc.setPhase(newPhase);

    LOGV2_DEBUG(5565600,
                2,
                "Create collection coordinator phase transition",
                "namespace"_attr = nss(),
                "newPhase"_attr = CreateCollectionCoordinatorPhase_serializer(newDoc.getPhase()),
                "oldPhase"_attr = CreateCollectionCoordinatorPhase_serializer(_doc.getPhase()));

    if (_doc.getPhase() == Phase::kUnset) {
        _doc = _insertStateDocument(std::move(newDoc));
        return;
    }
    _doc = _updateStateDocument(cc().makeOperationContext().get(), std::move(newDoc));
}

}  // namespace mongo
