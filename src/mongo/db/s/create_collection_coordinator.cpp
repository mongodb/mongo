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

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/create_collection_coordinator.h"
#include "mongo/db/s/shard_key_util.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/timeseries/timeseries_lookup.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/cluster_write.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/shard_collection_gen.h"
#include "mongo/util/future_util.h"

namespace mongo {
namespace {

const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());

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
    idIndex = entry["idIndex"].Obj().getOwned();

    auto indexSpecsList = localClient.getIndexSpecs(nssOrUUID, false, 0);

    return {optionsBob.obj(),
            std::vector<BSONObj>(std::begin(indexSpecsList), std::end(indexSpecsList)),
            idIndex};
}

/**
 * Compares the proposed shard key with the shard key of the collection's existing zones
 * to ensure they are a legal combination.
 */
void validateShardKeyAgainstExistingZones(OperationContext* opCtx,
                                          const BSONObj& proposedKey,
                                          const ShardKeyPattern& shardKeyPattern,
                                          const std::vector<TagsType>& tags) {
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
        }
    }
}

std::vector<TagsType> getTagsAndValidate(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         const BSONObj& proposedKey,
                                         const ShardKeyPattern& shardKeyPattern) {
    // Read zone info
    const auto catalogClient = Grid::get(opCtx)->catalogClient();
    auto tags = uassertStatusOK(catalogClient->getTagsForCollection(opCtx, nss));

    if (!tags.empty()) {
        validateShardKeyAgainstExistingZones(opCtx, proposedKey, shardKeyPattern, tags);
    }

    return tags;
}

boost::optional<UUID> getUUIDFromPrimaryShard(OperationContext* opCtx, const NamespaceString& nss) {
    // Obtain the collection's UUID from the primary shard's listCollections response.
    DBDirectClient localClient(opCtx);
    BSONObj res;
    {
        std::list<BSONObj> all =
            localClient.getCollectionInfos(nss.db().toString(), BSON("name" << nss.coll()));
        if (!all.empty()) {
            res = all.front().getOwned();
        }
    }

    uassert(ErrorCodes::InternalError,
            str::stream() << "expected to have an entry for " << nss.toString()
                          << " in listCollections response, but did not",
            !res.isEmpty());

    BSONObj collectionInfo;
    if (res["info"].type() == BSONType::Object) {
        collectionInfo = res["info"].Obj();
    }

    uassert(ErrorCodes::InternalError,
            str::stream() << "expected to return 'info' field as part of "
                             "listCollections for "
                          << nss.ns()
                          << " because the cluster is in featureCompatibilityVersion=3.6, but got "
                          << res,
            !collectionInfo.isEmpty());

    uassert(ErrorCodes::InternalError,
            str::stream() << "expected to return a UUID for collection " << nss.ns()
                          << " as part of 'info' field but got " << res,
            collectionInfo.hasField("uuid"));

    return uassertStatusOK(UUID::parse(collectionInfo["uuid"]));
}

bool checkIfCollectionIsEmpty(OperationContext* opCtx, const NamespaceString& nss) {
    // Use find with predicate instead of count in order to ensure that the count
    // command doesn't just consult the cached metadata, which may not always be
    // correct
    DBDirectClient localClient(opCtx);
    return localClient.findOne(nss.ns(), Query()).isEmpty();
}

int getNumShards(OperationContext* opCtx) {
    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
    shardRegistry->reload(opCtx);

    return shardRegistry->getNumShards(opCtx);
}

BSONObj getCollation(OperationContext* opCtx,
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
        return BSONObj();

    auto actualCollatorBSON = actualCollator->getSpec().toBSON();

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

    return actualCollatorBSON;
}

void removeChunks(OperationContext* opCtx, const UUID& uuid) {
    BatchWriteExecStats stats;
    BatchedCommandResponse response;

    BatchedCommandRequest deleteRequest([&]() {
        write_ops::DeleteCommandRequest deleteOp(ChunkType::ConfigNS);
        deleteOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        deleteOp.setDeletes(
            std::vector{write_ops::DeleteOpEntry(BSON(ChunkType::collectionUUID << uuid), false)});
        return deleteOp;
    }());

    deleteRequest.setWriteConcern(ShardingCatalogClient::kMajorityWriteConcern.toBSON());

    cluster::write(opCtx, deleteRequest, &stats, &response, boost::none);

    uassertStatusOK(response.toStatus());
}

void upsertChunks(OperationContext* opCtx, std::vector<ChunkType>& chunks) {
    BatchWriteExecStats stats;
    BatchedCommandResponse response;

    BatchedCommandRequest updateRequest([&]() {
        write_ops::UpdateCommandRequest updateOp(ChunkType::ConfigNS);
        std::vector<write_ops::UpdateOpEntry> entries;
        entries.reserve(chunks.size());
        for (const auto& chunk : chunks) {
            write_ops::UpdateOpEntry entry(
                BSON(ChunkType::collectionUUID << chunk.getCollectionUUID() << ChunkType::shard
                                               << chunk.getShard() << ChunkType::min
                                               << chunk.getMin()),
                write_ops::UpdateModification::parseFromClassicUpdate(chunk.toConfigBSON()));
            entry.setUpsert(true);
            entry.setMulti(false);
            entries.push_back(entry);
        }
        updateOp.setUpdates(entries);
        return updateOp;
    }());

    updateRequest.setWriteConcern(ShardingCatalogClient::kMajorityWriteConcern.toBSON());

    cluster::write(opCtx, updateRequest, &stats, &response, boost::none);

    uassertStatusOK(response.toStatus());
}

void updateCatalogEntry(OperationContext* opCtx, const NamespaceString& nss, CollectionType& coll) {
    BatchWriteExecStats stats;
    BatchedCommandResponse response;
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
    try {
        cluster::write(opCtx, updateRequest, &stats, &response, boost::none);
    } catch (const DBException&) {
        // If an error happens when contacting the config server, we don't know if the update
        // succeded or not, which might cause the local shard version to differ from the config
        // server, so we clear the metadata to allow another operation to refresh it.
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        AutoGetCollection autoColl(opCtx, nss, MODE_IX);
        CollectionShardingRuntime::get(opCtx, nss)->clearFilteringMetadata(opCtx);
        throw;
    }
}

void broadcastDropCollection(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const std::shared_ptr<executor::TaskExecutor>& executor) {
    const auto primaryShardId = ShardingState::get(opCtx)->shardId();
    const ShardsvrDropCollectionParticipant dropCollectionParticipant(nss);

    auto participants = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
    // Remove prymary shard from participants
    participants.erase(std::remove(participants.begin(), participants.end(), primaryShardId),
                       participants.end());

    sharding_ddl_util::sendAuthenticatedCommandToShards(
        opCtx,
        nss.db(),
        CommandHelpers::appendMajorityWriteConcern(dropCollectionParticipant.toBSON({})),
        participants,
        executor);
}

}  // namespace

CreateCollectionCoordinator::CreateCollectionCoordinator(const BSONObj& initialState)
    : ShardingDDLCoordinator(initialState),
      _doc(CreateCollectionCoordinatorDocument::parse(
          IDLParserErrorContext("CreateCollectionCoordinatorDocument"), initialState)),
      _critSecReason(BSON("command"
                          << "createCollection"
                          << "ns" << nss().toString() << "request"
                          << _doc.getCreateCollectionRequest().toBSON())) {}

boost::optional<BSONObj> CreateCollectionCoordinator::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept {
    // TODO (SERVER-55485): Add request parameters.
    BSONObjBuilder cmdBob;
    if (const auto& optComment = getForwardableOpMetadata().getComment()) {
        cmdBob.append(optComment.get().firstElement());
    }
    cmdBob.append("request", _doc.getCreateCollectionRequest().toBSON());

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
        IDLParserErrorContext("RenameCollectionCoordinatorDocument"), doc);

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
        .then(_executePhase(Phase::kCheck,
                            [this, anchor = shared_from_this()] {
                                _shardKeyPattern = ShardKeyPattern(*_doc.getShardKey());
                                auto opCtxHolder = cc().makeOperationContext();
                                auto* opCtx = opCtxHolder.get();
                                getForwardableOpMetadata().setOn(opCtx);

                                _checkCommandArguments(opCtx);
                            }))
        .then(_executePhase(
            Phase::kCommit,
            [this, executor = executor, token, anchor = shared_from_this()] {
                if (!_shardKeyPattern) {
                    _shardKeyPattern = ShardKeyPattern(*_doc.getShardKey());
                }
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                if (auto createCollectionResponseOpt =
                        sharding_ddl_util::checkIfCollectionAlreadySharded(
                            opCtx,
                            nss(),
                            _shardKeyPattern->getKeyPattern().toBSON(),
                            getCollation(opCtx, nss(), _doc.getCollation()),
                            _doc.getUnique().value_or(false))) {
                    _result = createCollectionResponseOpt;
                    // Early return before holding the critical section, the
                    // collection was already created and commited but there was a
                    // stepdown before removing the coordinator document.
                    return;
                }

                // Entering the critical section. From this point on, the writes are blocked. Before
                // calling this method, we need the coordinator document to be persisted (and hence
                // the kCheck state), otherwise nothing will release the critical section in the
                // presence of a stepdown.
                sharding_ddl_util::acquireRecoverableCriticalSectionBlockWrites(
                    opCtx, nss(), _critSecReason, ShardingCatalogClient::kMajorityWriteConcern);

                if (_recoveredFromDisk) {
                    LOGV2_DEBUG(5458704,
                                1,
                                "Removing partial changes from previous run",
                                "namespace"_attr = nss());
                    try {
                        removeChunks(opCtx, *getUUIDFromPrimaryShard(opCtx, nss()));
                        broadcastDropCollection(opCtx, nss(), **executor);
                    } catch (const DBException& ex) {
                        LOGV2_WARNING(5555101,
                                      "The partial changes operations failed, this is normal.",
                                      "error"_attr = redact(ex));
                    }
                }

                _createCollectionAndIndexes(opCtx);

                _createPolicyAndChunks(opCtx);

                if (_splitPolicy->isOptimized()) {
                    // Block reads/writes from here on if we need to create
                    // the collection on other shards, this way we prevent
                    // reads/writes that should be redirected to another
                    // shard.
                    sharding_ddl_util::acquireRecoverableCriticalSectionBlockReads(
                        opCtx, nss(), _critSecReason, ShardingCatalogClient::kMajorityWriteConcern);
                    _createCollectionOnNonPrimaryShards(opCtx);

                    _commitWithRetries(executor, token).get(opCtx);
                }

                sharding_ddl_util::releaseRecoverableCriticalSection(
                    opCtx, nss(), _critSecReason, ShardingCatalogClient::kMajorityWriteConcern);

                if (!_splitPolicy->isOptimized()) {
                    _commitWithRetries(executor, token).get(opCtx);
                }

                _finalize(opCtx);
            }))
        .onError([this, anchor = shared_from_this()](const Status& status) {
            if (!status.isA<ErrorCategory::NotPrimaryError>() &&
                !status.isA<ErrorCategory::ShutdownError>()) {
                LOGV2_ERROR(5458702,
                            "Error running create collection",
                            "namespace"_attr = nss(),
                            "error"_attr = redact(status));

                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();

                sharding_ddl_util::releaseRecoverableCriticalSection(
                    opCtx, nss(), _critSecReason, ShardingCatalogClient::kMajorityWriteConcern);
            }
            return status;
        });
}

void CreateCollectionCoordinator::_interrupt(Status status) noexcept {
    if (status.isA<ErrorCategory::NotPrimaryError>() ||
        status.isA<ErrorCategory::ShutdownError>()) {
        auto client = cc().getServiceContext()->makeClient("CreateCollectionCleanupClient");
        AlternativeClientRegion acr(client);
        auto opCtxHolder = cc().makeOperationContext();
        auto* opCtx = opCtxHolder.get();
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());

        auto* const csr = CollectionShardingRuntime::get_UNSAFE(opCtx->getServiceContext(), nss());
        auto csrLock = CollectionShardingRuntime::CSRLock::lockExclusive(opCtx, csr);
        csr->exitCriticalSection(csrLock);
        csr->clearFilteringMetadata(opCtx);
    }
}

void CreateCollectionCoordinator::_checkCommandArguments(OperationContext* opCtx) {
    LOGV2_DEBUG(5277902, 2, "Create collection _checkCommandArguments", "namespace"_attr = nss());

    const auto dbInfo = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getDatabaseWithRefresh(opCtx, nss().db()));

    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "sharding not enabled for db " << nss().db(),
            dbInfo.shardingEnabled());

    if (nss().db() == NamespaceString::kConfigDb) {
        // Only whitelisted collections in config may be sharded (unless we are in test mode)
        uassert(ErrorCodes::IllegalOperation,
                "only special collections in the config db may be sharded",
                nss() == NamespaceString::kLogicalSessionsNamespace);
    }

    // Ensure that hashed and unique are not both set.
    uassert(ErrorCodes::InvalidOptions,
            "Hashed shard keys cannot be declared unique. It's possible to ensure uniqueness on "
            "the hashed field by declaring an additional (non-hashed) unique index on the field.",
            !_shardKeyPattern->isHashedPattern() || !_doc.getUnique().value_or(false));

    // Ensure that a time-series collection cannot be sharded
    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "can't shard time-series collection " << nss(),
            !timeseries::getTimeseriesOptions(opCtx, nss()));

    // Ensure the namespace is valid.
    uassert(ErrorCodes::IllegalOperation,
            "can't shard system namespaces",
            !nss().isSystem() || nss() == NamespaceString::kLogicalSessionsNamespace ||
                nss().isTemporaryReshardingCollection());

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

    auto unique = _doc.getUnique().value_or(false);
    _collation = getCollation(opCtx, nss(), _doc.getCollation());

    if (auto createCollectionResponseOpt =
            mongo::sharding_ddl_util::checkIfCollectionAlreadySharded(
                opCtx, nss(), _shardKeyPattern->getKeyPattern().toBSON(), *_collation, unique)) {
        _result = createCollectionResponseOpt;
        return;
    }

    shardkeyutil::validateShardKeyIndexExistsOrCreateIfPossible(
        opCtx,
        nss(),
        *_shardKeyPattern,
        _collation,
        _doc.getUnique().value_or(false),
        shardkeyutil::ValidationBehaviorsShardCollection(opCtx));

    // Wait until the index is majority written, to prevent having the collection commited to the
    // config server, but the index creation rolled backed on stepdowns.
    WriteConcernResult ignoreResult;
    auto latestOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    uassertStatusOK(waitForWriteConcern(
        opCtx, latestOpTime, ShardingCatalogClient::kMajorityWriteConcern, &ignoreResult));

    _collectionUUID = *getUUIDFromPrimaryShard(opCtx, nss());
}

void CreateCollectionCoordinator::_createPolicyAndChunks(OperationContext* opCtx) {
    LOGV2_DEBUG(5277904, 2, "Create collection _createChunks", "namespace"_attr = nss());

    _splitPolicy = InitialSplitPolicy::calculateOptimizationStrategy(
        opCtx,
        *_shardKeyPattern,
        _doc.getNumInitialChunks() ? *_doc.getNumInitialChunks() : 0,
        _doc.getPresplitHashedZones() ? *_doc.getPresplitHashedZones() : false,
        _doc.getInitialSplitPoints(),
        getTagsAndValidate(opCtx, nss(), _shardKeyPattern->toBSON(), *_shardKeyPattern),
        getNumShards(opCtx),
        checkIfCollectionIsEmpty(opCtx, nss()));

    _initialChunks = _splitPolicy->createFirstChunks(
        opCtx,
        *_shardKeyPattern,
        {nss(),
         *_collectionUUID,
         ShardingState::get(opCtx)->shardId(),
         ChunkEntryFormat::getForVersionCallerGuaranteesFCVStability(
             ServerGlobalParams::FeatureCompatibility::Version::kVersion50)});

    // There must be at least one chunk.
    invariant(!_initialChunks.chunks.empty());
}

void CreateCollectionCoordinator::_createCollectionOnNonPrimaryShards(OperationContext* opCtx) {
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
            createCollectionParticipantRequest.toBSON(
                BSON("writeConcern" << ShardingCatalogClient::kMajorityWriteConcern.toBSON())));

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
    upsertChunks(opCtx, _initialChunks.chunks);

    CollectionType coll(nss(),
                        _initialChunks.collVersion().epoch(),
                        _initialChunks.collVersion().getTimestamp(),
                        Date_t::now(),
                        *_collectionUUID);

    coll.setKeyPattern(_shardKeyPattern->getKeyPattern());

    if (_collation) {
        coll.setDefaultCollation(_collation.value());
    }

    if (_doc.getUnique()) {
        coll.setUnique(*_doc.getUnique());
    }

    updateCatalogEntry(opCtx, nss(), coll);
}

ExecutorFuture<void> CreateCollectionCoordinator::_commitWithRetries(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& token) {
    return AsyncTry([this] {
               auto opCtxHolder = cc().makeOperationContext();
               auto* opCtx = opCtxHolder.get();
               getForwardableOpMetadata().setOn(opCtx);

               _commit(opCtx);
           })
        .until([token](Status status) { return status.isOK() || token.isCanceled(); })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, CancellationToken::uncancelable());
}

void CreateCollectionCoordinator::_finalize(OperationContext* opCtx) noexcept {
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

    // Is it really necessary to refresh all shards? or can I assume that the shard version will be
    // unknown and refreshed eventually?
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
        try {
            auto refreshCmdResponse = uassertStatusOK(shard->runCommandWithFixedRetryAttempts(
                opCtx,
                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                "admin",
                BSON("_flushRoutingTableCacheUpdates" << nss().ns()),
                Seconds{30},
                Shard::RetryPolicy::kIdempotent));

            uassertStatusOK(refreshCmdResponse.commandStatus);
        } catch (const DBException& ex) {
            LOGV2_WARNING(5277909,
                          "Could not refresh shard",
                          "shardId"_attr = shard->getId(),
                          "error"_attr = redact(ex.reason()));
        }
        shardsRefreshed.emplace(chunkShardId);
    }

    LOGV2(5277901,
          "Created initial chunk(s)",
          "namespace"_attr = nss(),
          "numInitialChunks"_attr = _initialChunks.chunks.size(),
          "initialCollectionVersion"_attr = _initialChunks.collVersion());


    ShardingLogging::get(opCtx)->logChange(
        opCtx,
        "shardCollection.end",
        nss().ns(),
        BSON("version" << _initialChunks.collVersion().toString() << "numChunks"
                       << static_cast<int>(_initialChunks.chunks.size())),
        ShardingCatalogClient::kMajorityWriteConcern);

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

// Phase change and document handling API.
void CreateCollectionCoordinator::_insertCoordinatorDocument(CoordDoc&& doc) {
    auto docBSON = _doc.toBSON();
    auto coorMetadata = doc.getShardingDDLCoordinatorMetadata();
    coorMetadata.setRecoveredFromDisk(true);
    doc.setShardingDDLCoordinatorMetadata(coorMetadata);

    auto opCtx = cc().makeOperationContext();
    PersistentTaskStore<CoordDoc> store(NamespaceString::kShardingDDLCoordinatorsNamespace);
    store.add(opCtx.get(), doc, WriteConcerns::kMajorityWriteConcern);
    _doc = std::move(doc);
}

void CreateCollectionCoordinator::_updateCoordinatorDocument(CoordDoc&& newDoc) {
    auto opCtx = cc().makeOperationContext();
    PersistentTaskStore<CoordDoc> store(NamespaceString::kShardingDDLCoordinatorsNamespace);
    store.update(opCtx.get(),
                 BSON(CoordDoc::kIdFieldName << _doc.getId().toBSON()),
                 newDoc.toBSON(),
                 WriteConcerns::kMajorityWriteConcern);

    _doc = std::move(newDoc);
}

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
        _insertCoordinatorDocument(std::move(newDoc));
        return;
    }
    _updateCoordinatorDocument(std::move(newDoc));
}

}  // namespace mongo
