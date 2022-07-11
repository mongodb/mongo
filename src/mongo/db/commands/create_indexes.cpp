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


#include "mongo/platform/basic.h"

#include <string>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/crypto/encryption_fields_util.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/clustered_collection_util.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_uuid_mismatch.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/catalog/multi_index_block.h"
#include "mongo/db/catalog/uncommitted_catalog_updates.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/create_indexes_gen.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/storage/two_phase_index_build_knobs_gen.h"
#include "mongo/db/timeseries/catalog_helper.h"
#include "mongo/db/timeseries/timeseries_commands_conversion_helper.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/idl/command_generic_argument.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex


namespace mongo {

namespace {
// This failpoint simulates a WriteConflictException during createIndexes where the collection is
// implicitly created.
MONGO_FAIL_POINT_DEFINE(createIndexesWriteConflict);

// This failpoint causes createIndexes with an implicit collection creation to hang before the
// collection is created.
MONGO_FAIL_POINT_DEFINE(hangBeforeCreateIndexesCollectionCreate);
MONGO_FAIL_POINT_DEFINE(hangBeforeIndexBuildAbortOnInterrupt);
MONGO_FAIL_POINT_DEFINE(hangAfterIndexBuildAbort);

// This failpoint hangs between logging the index build UUID and starting the index build
// through the IndexBuildsCoordinator.
MONGO_FAIL_POINT_DEFINE(hangCreateIndexesBeforeStartingIndexBuild);

constexpr auto kCommandName = "createIndexes"_sd;
constexpr auto kAllIndexesAlreadyExist = "all indexes already exist"_sd;
constexpr auto kIndexAlreadyExists = "index already exists"_sd;

/**
 * Parses the index specifications from 'cmdObj', validates them, and returns equivalent index
 * specifications that have any missing attributes filled in. If any index specification is
 * malformed, then an error status is returned.
 */
std::vector<BSONObj> parseAndValidateIndexSpecs(OperationContext* opCtx,
                                                const CreateIndexesCommand& cmd) {
    constexpr auto k_id_ = "_id_"_sd;
    constexpr auto kStar = "*"_sd;

    const auto ns = cmd.getNamespace();
    const bool ignoreUnknownIndexOptions = cmd.getIgnoreUnknownIndexOptions();

    std::vector<BSONObj> indexSpecs;
    for (const auto& index : cmd.getIndexes()) {
        BSONObj parsedIndexSpec = index;
        if (ignoreUnknownIndexOptions) {
            parsedIndexSpec = index_key_validate::removeUnknownFields(ns, parsedIndexSpec);
        }

        auto indexSpecStatus = index_key_validate::validateIndexSpec(opCtx, parsedIndexSpec);
        uassertStatusOK(indexSpecStatus.getStatus().withContext(
            str::stream() << "Error in specification " << parsedIndexSpec.toString()));

        auto indexSpec = indexSpecStatus.getValue();
        if (IndexDescriptor::isIdIndexPattern(
                indexSpec[IndexDescriptor::kKeyPatternFieldName].Obj())) {
            uassertStatusOK(index_key_validate::validateIdIndexSpec(indexSpec));
        } else {
            uassert(ErrorCodes::BadValue,
                    str::stream() << "The index name '_id_' is reserved for the _id index, "
                                     "which must have key pattern {_id: 1}, found "
                                  << indexSpec[IndexDescriptor::kKeyPatternFieldName],
                    indexSpec[IndexDescriptor::kIndexNameFieldName].String() != k_id_);

            // An index named '*' cannot be dropped on its own, because a dropIndex oplog
            // entry with a '*' as an index name means "drop all indexes in this
            // collection".  We disallow creation of such indexes to avoid this conflict.
            uassert(ErrorCodes::BadValue,
                    "The index name '*' is not valid.",
                    indexSpec[IndexDescriptor::kIndexNameFieldName].String() != kStar);

            uassert(ErrorCodes::BadValue,
                    "Can't hide index on system collection",
                    !(ns.isSystem() && !ns.isTimeseriesBucketsCollection()) ||
                        indexSpec[IndexDescriptor::kHiddenFieldName].eoo());
        }

        indexSpecs.push_back(std::move(indexSpec));
    }

    uassert(ErrorCodes::BadValue, "Must specify at least one index to create", !indexSpecs.empty());

    return indexSpecs;
}

void appendFinalIndexFieldsToResult(CreateIndexesReply* reply,
                                    int numIndexesBefore,
                                    int numIndexesAfter,
                                    int numSpecs,
                                    boost::optional<CommitQuorumOptions> commitQuorum) {
    reply->setNumIndexesBefore(numIndexesBefore);
    reply->setNumIndexesAfter(numIndexesAfter);
    if (numIndexesAfter == numIndexesBefore) {
        reply->setNote(kAllIndexesAlreadyExist);
    } else if (numIndexesAfter < numIndexesBefore + numSpecs) {
        reply->setNote(kIndexAlreadyExists);
    }

    // commitQuorum will be populated only when two phase index build is enabled.
    if (commitQuorum) {
        reply->setCommitQuorum(commitQuorum);
    }
}


/**
 * Ensures that the options passed in for TTL indexes are valid.
 */
void validateTTLOptions(OperationContext* opCtx,
                        const NamespaceString& ns,
                        const CreateIndexesCommand& cmd) {
    const auto clusteredAndCapped = [&](LockMode mode) {
        AutoGetCollection collection(opCtx, ns, mode);
        if (collection) {
            const auto c = collection.getCollection().get();
            if (c->getClusteredInfo() && c->isCapped()) {
                return true;
            }
        }
        return false;
    }(MODE_IS);

    for (const auto& index : cmd.getIndexes()) {
        uassert(ErrorCodes::Error(6049202),
                "TTL secondary indexes are not allowed on a capped clustered collection",
                !(clusteredAndCapped && index_key_validate::isIndexTTL(index)));
        uassertStatusOK(index_key_validate::validateIndexSpecTTL(index));
    }
}

void checkEncryptedFieldIndexRestrictions(OperationContext* opCtx,
                                          const NamespaceString& ns,
                                          const CreateIndexesCommand& cmd) {
    AutoGetCollection collection(opCtx, ns, MODE_IS);
    if (!collection) {
        return;
    }

    const auto& encryptConfig = collection->getCollectionOptions().encryptedFieldConfig;
    if (!encryptConfig) {
        // this collection is not encrypted
        return;
    }

    auto& encryptedFields = encryptConfig->getFields();
    std::vector<FieldRef> encryptedFieldRefs;

    // Create the FieldRefs for each encrypted field.
    if (!encryptedFields.empty()) {
        std::transform(encryptedFields.begin(),
                       encryptedFields.end(),
                       std::back_inserter(encryptedFieldRefs),
                       [](auto& path) { return FieldRef(path.getPath()); });
    }

    for (const auto& index : cmd.getIndexes()) {
        // Do not allow TTL indexes on encrypted collections because automatic
        // deletion of encrypted documents would require the deletion tokens
        // for each encrypted field, which the server does not have.
        uassert(6346501,
                "TTL indexes are not allowed on encrypted collections",
                !index_key_validate::isIndexTTL(index));

        // Do not allow indexes on encrypted fields, or prefixes of encrypted fields.
        auto keyObject = index[IndexDescriptor::kKeyPatternFieldName].Obj();
        for (const auto& keyElement : keyObject) {
            auto match = findMatchingEncryptedField(FieldRef(keyElement.fieldNameStringData()),
                                                    encryptedFieldRefs);
            uassert(6346502,
                    str::stream() << "Index not allowed on, or a prefix of, the encrypted field "
                                  << match->encryptedField.dottedField(),
                    !match);
        }
    }
}

/**
 * Retrieves the commit quorum from 'cmdObj' if it is present. If it isn't, we provide a default
 * commit quorum, which consists of all the data-bearing nodes.
 */
boost::optional<CommitQuorumOptions> parseAndGetCommitQuorum(OperationContext* opCtx,
                                                             IndexBuildProtocol protocol,
                                                             const CreateIndexesCommand& cmd) {
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    auto commitQuorumEnabled = (enableIndexBuildCommitQuorum) ? true : false;

    auto commitQuorum = cmd.getCommitQuorum();
    if (commitQuorum) {
        uassert(ErrorCodes::BadValue,
                str::stream() << "Standalones can't specify commitQuorum",
                replCoord->isReplEnabled());
        uassert(ErrorCodes::BadValue,
                str::stream() << "commitQuorum is supported only for two phase index builds with "
                                 "commit quorum support enabled ",
                (IndexBuildProtocol::kTwoPhase == protocol && commitQuorumEnabled));
        return commitQuorum;
    }

    if (IndexBuildProtocol::kTwoPhase == protocol) {
        // Setting CommitQuorum to 0 will make the index build to opt out of voting proces.
        return (replCoord->isReplEnabled() && commitQuorumEnabled)
            ? CommitQuorumOptions(CommitQuorumOptions::kVotingMembers)
            : CommitQuorumOptions(CommitQuorumOptions::kDisabled);
    }

    return boost::none;
}

/**
 * Returns a vector of index specs with the filled in collection default options and removes any
 * indexes that already exist on the collection -- both ready indexes and in-progress builds. If the
 * returned vector is empty after returning, no new indexes need to be built. Throws on error.
 */
std::vector<BSONObj> resolveDefaultsAndRemoveExistingIndexes(OperationContext* opCtx,
                                                             const CollectionPtr& collection,
                                                             std::vector<BSONObj> indexSpecs) {
    // Normalize the specs' collations, wildcard projections, and partial filters as applicable.
    auto normalSpecs = IndexBuildsCoordinator::normalizeIndexSpecs(opCtx, collection, indexSpecs);

    return collection->getIndexCatalog()->removeExistingIndexes(
        opCtx, collection, normalSpecs, false /*removeIndexBuildsToo*/);
}

/**
 * Returns true, after filling in the command result, if the index creation can return early.
 */
bool indexesAlreadyExist(OperationContext* opCtx,
                         const CollectionPtr& collection,
                         const std::vector<BSONObj>& specs,
                         CreateIndexesReply* reply) {
    auto specsCopy = resolveDefaultsAndRemoveExistingIndexes(opCtx, collection, specs);
    if (specsCopy.size() > 0) {
        return false;
    }

    auto numIndexes = collection->getIndexCatalog()->numIndexesTotal(opCtx);
    reply->setNumIndexesBefore(numIndexes);
    reply->setNumIndexesAfter(numIndexes);
    reply->setNote(kAllIndexesAlreadyExist);

    return true;
}

/**
 * Checks database sharding state. Throws exception on error.
 */
void checkDatabaseShardingState(OperationContext* opCtx, const NamespaceString& ns) {
    auto dss = DatabaseShardingState::get(opCtx, ns.db());
    auto dssLock = DatabaseShardingState::DSSLock::lockShared(opCtx, dss);

    Lock::CollectionLock collLock(opCtx, ns, MODE_IS);
    try {
        const auto collDesc =
            CollectionShardingState::get(opCtx, ns)->getCollectionDescription(opCtx);
        if (!collDesc.isSharded()) {
            auto mpsm = dss->getMovePrimarySourceManager(dssLock);

            if (mpsm) {
                LOGV2(4909200, "assertMovePrimaryInProgress", "namespace"_attr = ns.toString());

                uasserted(ErrorCodes::MovePrimaryInProgress,
                          "movePrimary is in progress for namespace " + ns.toString());
            }
        }
    } catch (const DBException& ex) {
        if (ex.toStatus() != ErrorCodes::MovePrimaryInProgress) {
            LOGV2(4909201, "Error when getting collection description", "what"_attr = ex.what());
            return;
        }
        throw;
    }
}

/**
 * Attempts to create indexes in `specs` on a non-existent collection (or empty collection created
 * in the same multi-document transaction) with namespace `ns`. In the former case, the collection
 * is implicitly created.
 * Returns a BSONObj containing fields to be appended to the result of the calling function.
 * `commitQuorum` is passed only to be appended to the result, for completeness. It is otherwise
 * unused.
 * Expects to be run at the end of a larger writeConflictRetry loop.
 */
CreateIndexesReply runCreateIndexesOnNewCollection(
    OperationContext* opCtx,
    const NamespaceString& ns,
    const std::vector<BSONObj>& specs,
    boost::optional<CommitQuorumOptions> commitQuorum,
    bool createCollImplicitly) {
    WriteUnitOfWork wunit(opCtx);

    uassert(ErrorCodes::CommandNotSupportedOnView,
            "Cannot create indexes on a view",
            !CollectionCatalog::get(opCtx)->lookupView(opCtx, ns));

    if (createCollImplicitly) {
        for (const auto& spec : specs) {
            uassert(6100900,
                    "Cannot implicitly create a new collection with createIndex 'clustered' option",
                    !spec[IndexDescriptor::kClusteredFieldName]);
        }

        // We need to create the collection.
        BSONObjBuilder builder;
        builder.append("create", ns.coll());
        CollectionOptions options;
        builder.appendElements(options.toBSON());
        BSONObj idIndexSpec;

        if (MONGO_unlikely(hangBeforeCreateIndexesCollectionCreate.shouldFail())) {
            // Simulate a scenario where a conflicting collection creation occurs
            // mid-index build.
            LOGV2(20437,
                  "Hanging create collection due to failpoint "
                  "'hangBeforeCreateIndexesCollectionCreate'");
            hangBeforeCreateIndexesCollectionCreate.pauseWhileSet();
        }

        OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE unsafeCreateCollection(
            opCtx);
        auto createStatus =
            createCollection(opCtx, ns.db().toString(), builder.obj().getOwned(), idIndexSpec);

        if (createStatus == ErrorCodes::NamespaceExists) {
            throwWriteConflictException(
                str::stream() << "Failed to create indexes on new collection: namespace "_sd
                              << ns.ns() << " exists. Status: "_sd << createStatus.toString());
        }

        uassertStatusOK(createStatus);
    }

    // By this point, we have exclusive access to our collection, either because we created the
    // collection implicitly as part of createIndexes or because the collection was created earlier
    // in the same multi-document transaction.
    CollectionWriter collection(opCtx, ns);
    CollectionCatalog::get(opCtx)->invariantHasExclusiveAccessToCollection(opCtx, collection->ns());
    invariant(opCtx->inMultiDocumentTransaction() || createCollImplicitly);

    uassert(ErrorCodes::OperationNotSupportedInTransaction,
            str::stream() << "Cannot create new indexes on non-empty collection " << ns
                          << " in a multi-document transaction.",
            collection->isEmpty(opCtx));

    const int numIndexesBefore =
        IndexBuildsCoordinator::getNumIndexesTotal(opCtx, collection.get());
    auto filteredSpecs =
        IndexBuildsCoordinator::prepareSpecListForCreate(opCtx, collection.get(), ns, specs);
    // It's possible for 'filteredSpecs' to be empty if we receive a createIndexes request for the
    // _id index and also create the collection implicitly. By this point, the _id index has already
    // been created, and there is no more work to be done.
    if (!filteredSpecs.empty()) {
        IndexBuildsCoordinator::createIndexesOnEmptyCollection(
            opCtx, collection, filteredSpecs, false);
    }

    const int numIndexesAfter = IndexBuildsCoordinator::getNumIndexesTotal(opCtx, collection.get());

    if (MONGO_unlikely(createIndexesWriteConflict.shouldFail())) {
        throwWriteConflictException();
    }
    wunit.commit();

    CreateIndexesReply reply;

    appendFinalIndexFieldsToResult(
        &reply, numIndexesBefore, numIndexesAfter, int(specs.size()), commitQuorum);
    reply.setCreatedCollectionAutomatically(true);

    return reply;
}

bool isCreatingInternalConfigTxnsPartialIndex(const CreateIndexesCommand& cmd) {
    if (cmd.getIndexes().size() != 1) {
        return false;
    }
    const auto& index = cmd.getIndexes()[0];

    UnorderedFieldsBSONObjComparator comparator;
    return comparator.compare(index, MongoDSessionCatalog::getConfigTxnPartialIndexSpec()) == 0;
}

CreateIndexesReply runCreateIndexesWithCoordinator(OperationContext* opCtx,
                                                   const CreateIndexesCommand& cmd) {
    const auto ns = cmd.getNamespace();
    uassertStatusOK(userAllowedWriteNS(opCtx, ns));

    // Disallow users from creating new indexes on config.transactions since the sessions code
    // was optimized to not update indexes. The only exception is the partial index used to support
    // retryable transactions that the sessions code knows how to handle.
    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "not allowed to create index on " << ns.ns(),
            ns != NamespaceString::kSessionTransactionsTableNamespace ||
                isCreatingInternalConfigTxnsPartialIndex(cmd));

    uassert(ErrorCodes::OperationNotSupportedInTransaction,
            str::stream() << "Cannot write to system collection " << ns.toString()
                          << " within a transaction.",
            !opCtx->inMultiDocumentTransaction() || !ns.isSystem());

    auto specs = parseAndValidateIndexSpecs(opCtx, cmd);
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    auto indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);
    // Two phase index builds are designed to improve the availability of indexes in a replica set.
    auto protocol = !replCoord->isOplogDisabledFor(opCtx, ns) ? IndexBuildProtocol::kTwoPhase
                                                              : IndexBuildProtocol::kSinglePhase;
    auto commitQuorum = parseAndGetCommitQuorum(opCtx, protocol, cmd);
    if (commitQuorum) {
        uassertStatusOK(replCoord->checkIfCommitQuorumCanBeSatisfied(commitQuorum.get()));
    }

    validateTTLOptions(opCtx, ns, cmd);
    checkEncryptedFieldIndexRestrictions(opCtx, ns, cmd);

    // Preliminary checks before handing control over to IndexBuildsCoordinator:
    // 1) We are in a replication mode that allows for index creation.
    // 2) Check sharding state.
    // 3) Check if we can create the index without handing control to the IndexBuildsCoordinator.
    boost::optional<UUID> collectionUUID;
    CreateIndexesReply reply;
    {
        AutoGetDb autoDb(opCtx, ns.db(), MODE_IX);

        checkDatabaseShardingState(opCtx, ns);
        if (!repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, ns)) {
            uasserted(ErrorCodes::NotWritablePrimary,
                      str::stream() << "Not primary while creating indexes in " << ns.ns());
        }

        bool indexExists = writeConflictRetry(opCtx, "createCollectionWithIndexes", ns.ns(), [&] {
            AutoGetCollection collection(opCtx, ns, MODE_IX);
            CollectionShardingState::get(opCtx, ns)->checkShardVersionOrThrow(opCtx);

            checkCollectionUUIDMismatch(
                opCtx, ns, collection.getCollection(), cmd.getCollectionUUID());

            // Before potentially taking an exclusive collection lock, check if all indexes already
            // exist while holding an intent lock.
            if (collection &&
                indexesAlreadyExist(opCtx, collection.getCollection(), specs, &reply)) {
                return true;
            }

            if (collection &&
                !UncommittedCatalogUpdates::get(opCtx).isCreatedCollection(opCtx, ns)) {
                // The collection exists and was not created in the same multi-document transaction
                // as the createIndexes.
                collectionUUID = collection->uuid();
                reply.setCreatedCollectionAutomatically(false);
                return false;
            }

            const bool createCollImplicitly = collection ? false : true;

            reply = runCreateIndexesOnNewCollection(
                opCtx, ns, specs, commitQuorum, createCollImplicitly);
            return true;
        });

        if (indexExists) {
            // No need to proceed if the index either already existed or has just been built.
            return reply;
        }

        // If the index does not exist by this point, the index build must go through the index
        // builds coordinator and take an exclusive lock. We should not take exclusive locks inside
        // of transactions, so we fail early here if we are inside of a transaction.
        uassert(ErrorCodes::OperationNotSupportedInTransaction,
                str::stream() << "Cannot create new indexes on existing collection " << ns
                              << " in a multi-document transaction.",
                !opCtx->inMultiDocumentTransaction());
    }

    // Use AutoStatsTracker to update Top.
    boost::optional<AutoStatsTracker> statsTracker;
    statsTracker.emplace(opCtx,
                         ns,
                         Top::LockType::WriteLocked,
                         AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                         CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(ns.dbName()));

    auto buildUUID = UUID::gen();
    ReplIndexBuildState::IndexCatalogStats stats;
    IndexBuildsCoordinator::IndexBuildOptions indexBuildOptions = {commitQuorum};

    LOGV2(20438,
          "Index build: registering",
          "buildUUID"_attr = buildUUID,
          "namespace"_attr = ns,
          "collectionUUID"_attr = *collectionUUID,
          "indexes"_attr = specs.size(),
          "firstIndex"_attr = specs[0][IndexDescriptor::kIndexNameFieldName],
          "command"_attr = cmd.toBSON({}));
    hangCreateIndexesBeforeStartingIndexBuild.pauseWhileSet(opCtx);

    bool shouldContinueInBackground = false;
    try {
        auto buildIndexFuture =
            uassertStatusOK(indexBuildsCoord->startIndexBuild(opCtx,
                                                              cmd.getDbName().toString(),
                                                              *collectionUUID,
                                                              specs,
                                                              buildUUID,
                                                              protocol,
                                                              indexBuildOptions));

        auto deadline = opCtx->getDeadline();
        LOGV2(20440,
              "Index build: waiting for index build to complete",
              "buildUUID"_attr = buildUUID,
              "deadline"_attr = deadline);

        // Throws on error.
        try {
            stats = buildIndexFuture.get(opCtx);
        } catch (const ExceptionForCat<ErrorCategory::Interruption>& interruptionEx) {
            LOGV2(20441,
                  "Index build: received interrupt signal",
                  "buildUUID"_attr = buildUUID,
                  "signal"_attr = interruptionEx);

            hangBeforeIndexBuildAbortOnInterrupt.pauseWhileSet();

            if (IndexBuildProtocol::kTwoPhase == protocol) {
                // If this node is no longer a primary, the index build will continue to run in the
                // background and will complete when this node receives a commitIndexBuild oplog
                // entry from the new primary.
                if (ErrorCodes::InterruptedDueToReplStateChange == interruptionEx.code()) {
                    shouldContinueInBackground = true;
                    throw;
                }
            }

            // It is unclear whether the interruption originated from the current opCtx instance
            // for the createIndexes command or that the IndexBuildsCoordinator task was interrupted
            // independently of this command invocation. We'll defensively abort the index build
            // with the assumption that if the index build was already in the midst of tearing down,
            // this will be a no-op.
            {
                // The current OperationContext may be interrupted, which would prevent us from
                // taking locks. Use a new OperationContext to abort the index build.
                auto newClient = opCtx->getServiceContext()->makeClient("abort-index-build");
                AlternativeClientRegion acr(newClient);
                const auto abortCtx = cc().makeOperationContext();

                std::string abortReason(str::stream() << "Index build aborted: " << buildUUID
                                                      << ": " << interruptionEx.toString());
                if (indexBuildsCoord->abortIndexBuildByBuildUUID(
                        abortCtx.get(), buildUUID, IndexBuildAction::kPrimaryAbort, abortReason)) {
                    LOGV2(20443,
                          "Index build: aborted due to interruption",
                          "buildUUID"_attr = buildUUID);
                } else {
                    // The index build may already be in the midst of tearing down.
                    LOGV2(5010500,
                          "Index build: failed to abort index build",
                          "buildUUID"_attr = buildUUID);
                }
            }
            throw;
        } catch (const ExceptionForCat<ErrorCategory::NotPrimaryError>& ex) {
            LOGV2(20444,
                  "Index build: received interrupt signal due to change in replication state",
                  "buildUUID"_attr = buildUUID,
                  "ex"_attr = ex);

            // If this node is no longer a primary, the index build will continue to run in the
            // background and will complete when this node receives a commitIndexBuild oplog
            // entry from the new primary.
            if (IndexBuildProtocol::kTwoPhase == protocol) {
                shouldContinueInBackground = true;
                throw;
            }

            std::string abortReason(str::stream() << "Index build aborted: " << buildUUID << ": "
                                                  << ex.toString());
            if (indexBuildsCoord->abortIndexBuildByBuildUUID(
                    opCtx, buildUUID, IndexBuildAction::kPrimaryAbort, abortReason)) {
                LOGV2(20446,
                      "Index build: aborted due to NotPrimary error",
                      "buildUUID"_attr = buildUUID);
            } else {
                // The index build may already be in the midst of tearing down.
                LOGV2(5010501,
                      "Index build: failed to abort index build",
                      "buildUUID"_attr = buildUUID);
            }

            throw;
        }

        LOGV2(20447, "Index build: completed", "buildUUID"_attr = buildUUID);
    } catch (DBException& ex) {
        // If the collection is dropped after the initial checks in this function (before the
        // AutoStatsTracker is created), the IndexBuildsCoordinator (either startIndexBuild() or
        // the the task running the index build) may return NamespaceNotFound. This is not
        // considered an error and the command should return success.
        if (ErrorCodes::NamespaceNotFound == ex.code()) {
            LOGV2(20448,
                  "Index build: failed because collection dropped",
                  "buildUUID"_attr = buildUUID,
                  "namespace"_attr = ns,
                  "collectionUUID"_attr = *collectionUUID,
                  "exception"_attr = ex);
            return reply;
        }

        if (shouldContinueInBackground) {
            LOGV2(4760400,
                  "Index build: ignoring interrupt and continuing in background",
                  "buildUUID"_attr = buildUUID);
        } else {
            LOGV2(20449, "Index build: failed", "buildUUID"_attr = buildUUID, "error"_attr = ex);
        }

        // All other errors should be forwarded to the caller with index build information included.
        ex.addContext(str::stream() << "Index build failed: " << buildUUID << ": Collection " << ns
                                    << " ( " << *collectionUUID << " )");

        // Set last op on error to provide the client with a specific optime to read the state of
        // the server when the createIndexes command failed.
        repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);

        throw;
    }

    // IndexBuildsCoordinator may write the createIndexes oplog entry on a different thread.
    // The current client's last op should be synchronized with the oplog to ensure consistent
    // getLastError results as the previous non-IndexBuildsCoordinator behavior.
    repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);

    appendFinalIndexFieldsToResult(
        &reply, stats.numIndexesBefore, stats.numIndexesAfter, int(specs.size()), commitQuorum);

    return reply;
}

/**
 * { createIndexes : "bar",
 *   indexes : [ { ns : "test.bar", key : { x : 1 }, name: "x_1" } ],
 *   commitQuorum: "majority" }
 *
 * commitQuorum specifies which or how many replica set members must be ready to commit before the
 * primary will commit the index. The same values can be used for commitQuorum as writeConcern, with
 * the addition of 'votingMembers', the default. It is used to ensure secondaries can commit indexes
 * quickly, minimizing replication lag (secondaries block replication on receipt of commitIndexBuild
 * while completing the associated index). Note that commitQuorum is NOT like writeConcern: there is
 * no guarantee that indexes on secondaries are ready for use after the command returns.
 */
class CmdCreateIndexes : public CreateIndexesCmdVersion1Gen<CmdCreateIndexes> {
public:
    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        bool supportsWriteConcern() const final {
            return true;
        }

        NamespaceString ns() const final {
            return request().getNamespace();
        }

        void doCheckAuthorization(OperationContext* opCtx) const {
            Privilege p(CommandHelpers::resourcePatternForNamespace(ns().toString()),
                        ActionType::createIndex);
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())->isAuthorizedForPrivilege(p));
        }

        CreateIndexesReply typedRun(OperationContext* opCtx) {
            const auto& origCmd = request();
            const auto* cmd = &origCmd;

            // If the request namespace refers to a time-series collection, transforms the user
            // time-series index request to one on the underlying bucket.
            boost::optional<CreateIndexesCommand> timeseriesCmdOwnership;
            auto isCommandOnTimeseriesBucketNamespace =
                origCmd.getIsTimeseriesNamespace() && *origCmd.getIsTimeseriesNamespace();
            if (auto options = timeseries::getTimeseriesOptions(
                    opCtx, origCmd.getNamespace(), !isCommandOnTimeseriesBucketNamespace)) {
                timeseriesCmdOwnership =
                    timeseries::makeTimeseriesCreateIndexesCommand(opCtx, origCmd, *options);
                cmd = &timeseriesCmdOwnership.get();
            }

            // If we encounter an IndexBuildAlreadyInProgress error for any of the requested index
            // specs, then we will wait for the build(s) to finish before trying again unless we are
            // in a multi-document transaction.
            bool shouldLogMessageOnAlreadyBuildingError = true;
            while (true) {
                try {
                    return runCreateIndexesWithCoordinator(opCtx, *cmd);
                } catch (const DBException& ex) {
                    hangAfterIndexBuildAbort.pauseWhileSet();
                    // We can only wait for an existing index build to finish if we are able to
                    // release our locks, in order to allow the existing index build to proceed. We
                    // cannot release locks in transactions, so we bypass the below logic in
                    // transactions.
                    if (ex.toStatus() != ErrorCodes::IndexBuildAlreadyInProgress ||
                        opCtx->inMultiDocumentTransaction()) {
                        throw;
                    }
                    if (shouldLogMessageOnAlreadyBuildingError) {
                        LOGV2(
                            20450,
                            "Received a request to create indexes: '{indexesFieldName}', but found "
                            "that at least one of the indexes is already being built, '{error}'. "
                            "This request will wait for the pre-existing index build to finish "
                            "before proceeding",
                            "Received a request to create indexes, "
                            "but found that at least one of the indexes is already being built."
                            "This request will wait for the pre-existing index build to finish "
                            "before proceeding",
                            "indexesFieldName"_attr = cmd->getIndexes(),
                            "error"_attr = ex);
                        shouldLogMessageOnAlreadyBuildingError = false;
                    }
                    // Reset the snapshot because we have released locks and need a fresh snapshot
                    // if we reacquire the locks again later.
                    opCtx->recoveryUnit()->abandonSnapshot();
                    // This is a bit racy since we are not holding a lock across discovering an
                    // in-progress build and starting to listen for completion. It is good enough,
                    // however: we can only wait longer than needed, not less.
                    IndexBuildsCoordinator::get(opCtx)->waitUntilAnIndexBuildFinishes(opCtx);
                }
            }
        }
    };

    bool collectsResourceConsumptionMetrics() const final {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kNever;
    }

    bool allowedInTransactions() const final {
        return true;
    }

} cmdCreateIndex;

}  // namespace
}  // namespace mongo
