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

#include "mongo/db/repl/oplog_applier_utils.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonelement_comparator.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/curop.h"
#include "mongo/db/curop_metrics.h"
#include "mongo/db/database_name.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/database.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/local_catalog/document_validation.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/repl/oplog_applier_utils.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/split_prepare_session_manager.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/exceptions.h"
#include "mongo/db/tenant_id.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <absl/hash/hash.h>
#include <absl/meta/type_traits.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


MONGO_FAIL_POINT_DEFINE(hangAfterApplyingCollectionDropOplogEntry);

namespace mongo {
namespace repl {
CachedCollectionProperties::CollectionProperties
CachedCollectionProperties::getCollectionProperties(OperationContext* opCtx,
                                                    const NamespaceString& nss) {
    auto it = _cache.find(nss);
    if (it != _cache.end()) {
        return it->second;
    }

    CollectionProperties collProperties;
    if (auto collection = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss)) {
        collProperties.isCapped = collection->isCapped();
        collProperties.isClustered = collection->isClustered();
        collProperties.collator = collection->getDefaultCollator();
    }
    _cache[nss] = collProperties;
    return collProperties;
}

namespace {
/**
 * Populates a CRUD op's idHash and updates the isForCappedCollection field if necessary.
 */
void processCrudOp(OperationContext* opCtx,
                   OplogEntry* op,
                   const CachedCollectionProperties::CollectionProperties& collProperties,
                   boost::optional<size_t>& idHash) {
    // Include the _id of the document in the hash so we get parallelism even if all writes are to a
    // single collection.
    //
    // For capped collections, this is usually illegal, since capped collections must preserve
    // insertion order. One exception are clustered capped collections with a monotonically
    // increasing cluster key, which guarantee preservation of the insertion order.
    if (!collProperties.isCapped || collProperties.isClustered) {
        BSONElement id = [&]() {
            return op->getIdElement();
        }();
        BSONElementComparator elementHasher(BSONElementComparator::FieldNamesMode::kIgnore,
                                            collProperties.collator);
        idHash.emplace(elementHasher.hash(id));
    }

    if (op->getOpType() == OpTypeEnum::kInsert && collProperties.isCapped) {
        // Mark capped collection ops before storing them to ensure we do not attempt to
        // bulk insert them.
        op->setIsForCappedCollection(true);
    }
}

/**
 * Returns the ID of the writer thread that this op will be assigned to, determined by the
 * namespace string (and document key if exists) of the op.
 */
uint32_t getWriterId(OperationContext* opCtx,
                     OplogEntry* op,
                     CachedCollectionProperties* collPropertiesCache,
                     uint32_t numWriters,
                     boost::optional<uint32_t> forceWriterId = boost::none) {
    auto hash = OplogApplierUtils::getOplogEntryHash(opCtx, op, collPropertiesCache);
    return (forceWriterId ? *forceWriterId : hash) % numWriters;
}

/**
 * Returns the ID of the writer thread that this op will be assigned to, determined by the
 * session ID of the op.
 */
uint32_t getWriterIdBySessionId(OplogEntry* op, uint32_t numWriters) {
    LogicalSessionIdHash lsidHasher;

    invariant(op->getSessionId());
    auto hash = static_cast<uint32_t>(lsidHasher(*op->getSessionId()));

    return hash % numWriters;
}

/**
 * Adds an op to the writer vector of the given writer ID. The variadic arguments will be
 * forwarded to the writer vector to in-place construct the op.
 */
template <typename Operation, typename... Args>
uint32_t addToWriterVectorImpl(uint32_t writerId,
                               std::vector<std::vector<Operation>>* writerVectors,
                               Args&&... args) {
    auto& writer = (*writerVectors)[writerId];

    if (writer.empty()) {
        // Skip a few growth rounds.
        writer.reserve(8);
    }
    writer.emplace_back(std::forward<Args>(args)...);

    return writerId;
}

/**
 * Adds the top-level prepareTransaction op to the writerVectors.
 */
void addTopLevelPrepare(OperationContext* opCtx,
                        OplogEntry* prepareOp,
                        std::vector<OplogEntry>* derivedOps,
                        std::vector<std::vector<ApplierOperation>>* writerVectors) {
    auto writerId = getWriterIdBySessionId(prepareOp, writerVectors->size());
    addToWriterVectorImpl(writerId,
                          writerVectors,
                          prepareOp,
                          ApplicationInstruction::applyTopLevelPreparedTxnOp,
                          *derivedOps);
}

/**
 * Adds the top-level commitTransaction or AbortTransaction op to the writerVectors.
 */
void addTopLevelCommitOrAbort(OperationContext* opCtx,
                              OplogEntry* commitOrAbortOp,
                              std::vector<std::vector<ApplierOperation>>* writerVectors) {
    auto writerId = getWriterIdBySessionId(commitOrAbortOp, writerVectors->size());
    addToWriterVectorImpl(writerId,
                          writerVectors,
                          commitOrAbortOp,
                          ApplicationInstruction::applyTopLevelPreparedTxnOp);
}
}  // namespace

uint32_t OplogApplierUtils::getOplogEntryHash(OperationContext* opCtx,
                                              OplogEntry* op,
                                              CachedCollectionProperties* collPropertiesCache) {
    boost::optional<size_t> idHash;
    NamespaceString nss = op->getNss();

    if (op->isCrudOpType()) {
        auto collProperties = collPropertiesCache->getCollectionProperties(opCtx, nss);
        processCrudOp(opCtx, op, collProperties, idHash);
    }

    return idHash ? absl::HashOf(nss, *idHash) : absl::HashOf(nss);
}

uint32_t OplogApplierUtils::addToWriterVector(
    OperationContext* opCtx,
    OplogEntry* op,
    std::vector<std::vector<ApplierOperation>>* writerVectors,
    CachedCollectionProperties* collPropertiesCache,
    boost::optional<uint32_t> forceWriterId) {
    auto writerId =
        getWriterId(opCtx, op, collPropertiesCache, writerVectors->size(), forceWriterId);
    return addToWriterVectorImpl(writerId, writerVectors, op);
}

void OplogApplierUtils::stableSortByNamespace(std::vector<ApplierOperation>* ops) {
    auto nssComparator = [](const ApplierOperation& l, const ApplierOperation& r) {
        if (l->getNss().isCommand()) {
            if (r->getNss().isCommand())
                // l == r; now compare the namespace
                return l->getNss() < r->getNss();
            // l < r
            return true;
        }
        if (r->getNss().isCommand())
            // l > r
            return false;
        return l->getNss() < r->getNss();
    };

    // Walk through the vector, if a prepared transaction command is encountered, sort
    // the ops between the previous prepared transaction command and the current one.
    for (size_t start = 0, end = 0; end <= ops->size(); ++end) {
        // The end iterator acts as a dummy prepared transaction command, so we would
        // also sort the ops after the last real one encountered.
        if (end == ops->size() || ops->at(end)->isPreparedTransactionCommand()) {
            std::stable_sort(ops->begin() + start, ops->begin() + end, nssComparator);
            start = end + 1;
        }
    }
}

void OplogApplierUtils::addDerivedOps(OperationContext* opCtx,
                                      std::vector<OplogEntry>* derivedOps,
                                      std::vector<std::vector<ApplierOperation>>* writerVectors,
                                      CachedCollectionProperties* collPropertiesCache,
                                      bool serial) {
    // Used to determine which writer vector to assign serial ops.
    boost::optional<uint32_t> serialWriterId;

    for (auto&& op : *derivedOps) {
        auto writerId =
            addToWriterVector(opCtx, &op, writerVectors, collPropertiesCache, serialWriterId);
        if (serial && !serialWriterId) {
            serialWriterId.emplace(writerId);
        }
    }
}

void OplogApplierUtils::addDerivedPrepares(
    OperationContext* opCtx,
    OplogEntry* prepareOp,
    std::vector<OplogEntry>* derivedOps,
    std::vector<std::vector<ApplierOperation>>* writerVectors,
    CachedCollectionProperties* collPropertiesCache,
    bool serial) {

    // Get the SplitPrepareSessionManager to be used to create split sessions.
    auto splitSessManager = ReplicationCoordinator::get(opCtx)->getSplitPrepareSessionManager();
    auto splitSessFunc = [=](const std::vector<uint32_t>& writerIds) -> const auto& {
        const auto& sessions = splitSessManager->splitSession(
            *prepareOp->getSessionId(), *prepareOp->getTxnNumber(), writerIds);
        invariant(sessions.size() == writerIds.size());
        return sessions;
    };

    if (derivedOps->empty()) {
        // For empty (read-only) prepares, we use the namespace of the original prepare oplog entry
        // (admin.$cmd) to decide which writer thread to apply it, and assigned it a split session.
        // The reason that we also split an empty prepare instead of treating it as some standalone
        // prepare op (as the prepares in initial sync or recovery mode) is so that we can keep a
        // logical invariant that all prepares in secondary mode are split, and thus we can apply
        // empty and non-empty prepares in the same way.
        auto writerId = getWriterId(opCtx, prepareOp, collPropertiesCache, writerVectors->size());
        const auto& sessionInfos = splitSessFunc({writerId});
        addToWriterVectorImpl(writerId,
                              writerVectors,
                              prepareOp,
                              ApplicationInstruction::applySplitPreparedTxnOp,
                              sessionInfos[0].session,
                              std::vector<const OplogEntry*>{});
    } else {
        // For non-empty prepares, the namespace of each derived op in the transaction is used to
        // decide which writer thread to apply it. We first add all the derived ops to a buffer
        // writer vector in order to get all the writer threads needed to apply this transaction.
        // We then acquire that number of split sessions and assign each writer thread a unique
        // split session when moving the ops to the real writer vector.
        std::set<uint32_t> writerIds;
        std::vector<std::vector<const OplogEntry*>> bufWriterVectors(writerVectors->size());
        boost::optional<uint32_t> serialWriterId;
        for (auto&& op : *derivedOps) {
            auto writerId = getWriterId(opCtx, &op, collPropertiesCache, writerVectors->size());
            if (serial && !serialWriterId) {
                serialWriterId.emplace(writerId);
                writerIds.emplace(*serialWriterId);
            }
            if (serialWriterId) {
                addToWriterVectorImpl(*serialWriterId, &bufWriterVectors, &op);
            } else {
                addToWriterVectorImpl(writerId, &bufWriterVectors, &op);
                writerIds.emplace(writerId);
            }
        }

        const auto& sessionInfos = splitSessFunc({writerIds.begin(), writerIds.end()});
        for (size_t i = 0, j = 0; i < bufWriterVectors.size(); ++i) {
            auto& bufWriter = bufWriterVectors[i];
            if (!bufWriter.empty()) {
                addToWriterVectorImpl(i,
                                      writerVectors,
                                      prepareOp,
                                      ApplicationInstruction::applySplitPreparedTxnOp,
                                      sessionInfos[j++].session,
                                      std::move(bufWriter));
            }
        }
    }

    // Add the top-level transaction to the writerVectors. Applying split transactions would
    // update the TransactionParticipant states of the split sessions, however we must also
    // update the TransactionParticipant states of the original (i.e. top-level) session in
    // case later this node becomes a primary.
    addTopLevelPrepare(opCtx, prepareOp, derivedOps, writerVectors);
}

void OplogApplierUtils::addDerivedCommitsOrAborts(
    OperationContext* opCtx,
    OplogEntry* commitOrAbortOp,
    std::vector<std::vector<ApplierOperation>>* writerVectors,
    CachedCollectionProperties* collPropertiesCache) {

    auto splitSessManager = ReplicationCoordinator::get(opCtx)->getSplitPrepareSessionManager();
    const auto& sessionInfos = splitSessManager->getSplitSessions(*commitOrAbortOp->getSessionId(),
                                                                  *commitOrAbortOp->getTxnNumber());

    // When this commit refers to a non-split prepare, it means the transaction was
    // prepared when the node was primary or during inital sync/recovery. In this
    // case we do not split the commit and just add it as-is to the writer vector.
    if (!sessionInfos.has_value()) {
        addToWriterVector(opCtx, commitOrAbortOp, writerVectors, collPropertiesCache);
        return;
    }

    // When this commit refers to a split prepare, we split the commit and add them
    // to the writers that have been assigned split prepare ops.
    for (const auto& sessInfo : *sessionInfos) {
        addToWriterVectorImpl(sessInfo.requesterId,
                              writerVectors,
                              commitOrAbortOp,
                              ApplicationInstruction::applySplitPreparedTxnOp,
                              sessInfo.session);
    }

    // Add the top-level transaction to the writerVectors. Applying split transactions would
    // update the TransactionParticipant states of the split sessions, however we must also
    // update the TransactionParticipant states of the original (i.e. top-level) session in
    // case later this node becomes a primary.
    addTopLevelCommitOrAbort(opCtx, commitOrAbortOp, writerVectors);
}

NamespaceStringOrUUID OplogApplierUtils::getNsOrUUID(const NamespaceString& nss,
                                                     const OplogEntry& op) {
    if (auto ui = op.getUuid()) {
        return {nss.dbName(), ui.value()};
    }
    return nss;
}

Status OplogApplierUtils::applyOplogEntryOrGroupedInsertsCommon(
    OperationContext* opCtx,
    const OplogEntryOrGroupedInserts& entryOrGroupedInserts,
    OplogApplication::Mode oplogApplicationMode,
    const bool isDataConsistent,
    IncrementOpsAppliedStatsFn incrementOpsAppliedStats,
    OpCounters* opCounters) {
    invariant(DocumentValidationSettings::get(opCtx).isSchemaValidationDisabled());

    const auto& op = entryOrGroupedInserts.getOp();
    // Count each log op application as a separate operation, for reporting purposes.
    CurOp individualOp;
    individualOp.push(opCtx);
    ON_BLOCK_EXIT([opCtx]() { recordCurOpMetricsOplogApplication(opCtx); });

    const NamespaceString nss(op->getNss());
    auto opType = op->getOpType();

    if (gMultitenancySupport &&
        gFeatureFlagRequireTenantID.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        invariant(op->getTid() == nss.tenantId());
    } else {
        invariant(op->getTid() == boost::none);
    }

    // VersionContext fixes a FCV snapshot over the opCtx, making FCV-gated feature
    // flags checks in secondaries behave as they did on the primary, thus ensuring
    // correct application even if the FCV changed due to a concurrent setFCV.
    boost::optional<VersionContext::ScopedSetDecoration> scopedVersionContext;
    if (op->getVersionContext()) {
        scopedVersionContext.emplace(opCtx, *op->getVersionContext());
    }

    if (opType == OpTypeEnum::kNoop) {
        incrementOpsAppliedStats();
        return Status::OK();
    } else if (DurableOplogEntry::isCrudOpType(opType)) {
        auto status = writeConflictRetry(opCtx, "applyOplogEntryOrGroupedInserts_CRUD", nss, [&] {
            // Need to throw instead of returning a status for it to be properly ignored.
            try {
                boost::optional<CollectionAcquisition> coll;
                Database* db = nullptr;

                // If the collection UUID does not resolve, acquire the collection using the
                // namespace. This is so we reach `applyOperation_inlock` below and invalidate
                // the preimage / postimage for the op if applicable.

                // TODO SERVER-41371 / SERVER-73661 this code is difficult to maintain and
                // needs to be done everywhere this situation is possible. We should try
                // to consolidate this into applyOperation_inlock.
                try {
                    coll.emplace(
                        acquireCollection(opCtx,
                                          {getNsOrUUID(nss, *op),
                                           PlacementConcern::kPretendUnsharded,
                                           repl::ReadConcernArgs::get(opCtx),
                                           AcquisitionPrerequisites::kWrite},
                                          fixLockModeForSystemDotViewsChanges(nss, MODE_IX)));

                    AutoGetDb autoDb(opCtx, coll->nss().dbName(), MODE_IX);
                    db = autoDb.getDb();
                } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>& ex) {
                    if (!isDataConsistent) {
                        coll.emplace(
                            acquireCollection(opCtx,
                                              {nss,
                                               PlacementConcern::kPretendUnsharded,
                                               repl::ReadConcernArgs::get(opCtx),
                                               AcquisitionPrerequisites::kWrite},
                                              fixLockModeForSystemDotViewsChanges(nss, MODE_IX)));

                        AutoGetDb autoDb(opCtx, coll->nss().dbName(), MODE_IX);
                        db = autoDb.ensureDbExists(opCtx);
                    } else {
                        throw ex;
                    }
                }

                invariant(coll);
                uassert(ErrorCodes::NamespaceNotFound,
                        str::stream()
                            << "missing database (" << nss.dbName().toStringForErrorMsg() << ")",
                        db);

                AutoStatsTracker statsTracker(
                    opCtx,
                    nss,
                    Top::LockType::WriteLocked,
                    AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                    DatabaseProfileSettings::get(opCtx->getServiceContext())
                        .getDatabaseProfileLevel(nss.dbName()));
                // We convert updates to upserts in secondary mode when the
                // oplogApplicationEnforcesSteadyStateConstraints parameter is false, to avoid
                // failing on the constraint that updates in steady state mode always update
                // an existing document.
                //
                // In initial sync and recovery modes we always ignore errors about missing
                // documents on update, so there is no reason to convert the updates to upsert.

                bool shouldAlwaysUpsert = !oplogApplicationEnforcesSteadyStateConstraints &&
                    oplogApplicationMode == OplogApplication::Mode::kSecondary;
                Status status = applyOperation_inlock(opCtx,
                                                      *coll,
                                                      entryOrGroupedInserts,
                                                      shouldAlwaysUpsert,
                                                      oplogApplicationMode,
                                                      isDataConsistent,
                                                      incrementOpsAppliedStats);
                if (!status.isOK() && status.code() == ErrorCodes::WriteConflict) {
                    throwWriteConflictException(str::stream()
                                                << "WriteConflict caught when applying operation."
                                                << " Original error: " << status.reason());
                }
                return status;
            } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>& ex) {
                // This can happen in initial sync or recovery modes (when a delete of the
                // namespace appears later in the oplog), but we will ignore it in the caller.
                //
                // When we're not enforcing steady-state constraints, the error is ignored
                // only for deletes, on the grounds that deleting from a non-existent collection
                // is a no-op.
                if (opType == OpTypeEnum::kDelete &&
                    !oplogApplicationEnforcesSteadyStateConstraints &&
                    oplogApplicationMode == OplogApplication::Mode::kSecondary) {
                    LOGV2_DEBUG(8994800,
                                1,
                                "Acceptable error when applying CRUD op",
                                "error"_attr = redact(ex),
                                "op"_attr = redact(entryOrGroupedInserts.toBSON()));
                    if (opCounters) {
                        const auto& opObj = redact(op->toBSONForLogging());
                        opCounters->gotDeleteFromMissingNamespace();
                        logOplogConstraintViolation(
                            opCtx,
                            op->getNss(),
                            OplogConstraintViolationEnum::kDeleteOnMissingNs,
                            "delete",
                            opObj,
                            boost::none /* status */);
                    }
                    return Status::OK();
                }

                ex.addContext(str::stream() << "Failed to apply operation: "
                                            << redact(entryOrGroupedInserts.toBSON()));
                throw;
            } catch (ExceptionFor<ErrorCodes::CommandNotSupportedOnView>& ex) {
                // This can happen in initial sync or unstable recovery mode when a time-series
                // collection is created in place of a dropped regular collection and oplog entries
                // are being applied that were originally performed on the regular collection.
                if (oplogApplicationMode == OplogApplication::Mode::kInitialSync ||
                    oplogApplicationMode == OplogApplication::Mode::kUnstableRecovering) {
                    LOGV2_DEBUG(8994801,
                                1,
                                "Acceptable error when applying CRUD op",
                                "error"_attr = redact(ex),
                                "op"_attr = redact(entryOrGroupedInserts.toBSON()));
                    return Status::OK();
                }

                throw;
            }
        });
        return status;
    } else if (DurableOplogEntry::isContainerOpType(opType)) {
        return writeConflictRetry(opCtx, "applyOplogEntryOrGroupedInserts_container", nss, [&] {
            auto coll = acquireCollection(opCtx,
                                          {nss,
                                           PlacementConcern::kPretendUnsharded,
                                           ReadConcernArgs::get(opCtx),
                                           AcquisitionPrerequisites::kWrite},
                                          MODE_IX);
            Status status = applyContainerOperation_inlock(opCtx, op, oplogApplicationMode);
            incrementOpsAppliedStats();
            return status;
        });
    } else if (opType == OpTypeEnum::kCommand) {
        auto status =
            writeConflictRetry(opCtx, "applyOplogEntryOrGroupedInserts_command", nss, [&] {
                // A special case apply for commands to avoid implicit database creation.
                Status status = applyCommand_inlock(opCtx, op, oplogApplicationMode);
                incrementOpsAppliedStats();
                return status;
            });
        if (op->getCommandType() == mongo::repl::OplogEntry::CommandType::kDrop) {
            hangAfterApplyingCollectionDropOplogEntry.executeIf(
                [&](const BSONObj&) {
                    hangAfterApplyingCollectionDropOplogEntry.pauseWhileSet();
                    LOGV2(5863600,
                          "Hanging due to 'hangAfterApplyingCollectionDropOplogEntry' failpoint.");
                },
                [&](const BSONObj& data) {
                    const auto fpDbName = DatabaseNameUtil::parseFailPointData(data, "dbName");
                    return nss.dbName() == fpDbName;
                });
        }
        return status;
    }

    MONGO_UNREACHABLE;
}

Status OplogApplierUtils::applyOplogBatchCommon(
    OperationContext* opCtx,
    std::vector<ApplierOperation>* ops,
    OplogApplication::Mode oplogApplicationMode,
    bool allowNamespaceNotFoundErrorsOnCrudOps,
    const bool isDataConsistent,
    InsertGroup::ApplyFunc applyOplogEntryOrGroupedInserts) noexcept {

    // We cannot do document validation, because document validation could have been disabled when
    // these oplog entries were generated.
    DisableDocumentValidation validationDisabler(opCtx);
    // Group the operations by namespace in order to get larger groups for bulk inserts, but do not
    // mix up the current order of oplog entries within the same namespace (thus *stable* sort).
    stableSortByNamespace(ops);
    InsertGroup insertGroup(
        ops, opCtx, oplogApplicationMode, isDataConsistent, applyOplogEntryOrGroupedInserts);

    const bool inStableRecovery = oplogApplicationMode == OplogApplication::Mode::kStableRecovering;
    for (auto it = ops->cbegin(); it != ops->cend(); ++it) {
        const auto& op = *it;

        // If we are successful in grouping and applying inserts, advance the current iterator
        // past the end of the inserted group of entries.
        auto groupResult = insertGroup.groupAndApplyInserts(it);
        if (groupResult.isOK()) {
            it = groupResult.getValue();
            continue;
        }

        // If we didn't create a group, try to apply the op individually.
        try {
            Status status =
                applyOplogEntryOrGroupedInserts(opCtx, op, oplogApplicationMode, isDataConsistent);

            if (!status.isOK()) {
                // Tried to apply an update operation but the document is missing, there must be
                // a delete operation for the document later in the oplog.
                // Server will crash on oplog application failure during recovery from stable
                // checkpoint in the test environment.
                if (status == ErrorCodes::UpdateOperationFailed &&
                    (oplogApplicationMode == OplogApplication::Mode::kInitialSync ||
                     OplogApplication::inRecovering(oplogApplicationMode))) {
                    if (inStableRecovery) {
                        repl::OplogApplication::checkOnOplogFailureForRecovery(
                            opCtx, op->getNss(), redact(op->toBSONForLogging()), redact(status));
                    }
                    continue;
                }

                LOGV2_FATAL_CONTINUE(21237,
                                     "Error applying operation",
                                     "oplogEntry"_attr = redact(op->toBSONForLogging()),
                                     "error"_attr = causedBy(redact(status)));
                return status;
            }
        } catch (const ExceptionFor<ErrorCodes::DuplicateKey>& e) {
            auto info = e.extraInfo<DuplicateKeyErrorInfo>();
            LOGV2_FATAL_CONTINUE(5689600,
                                 "Writer worker caught duplicate key exception",
                                 "keyPattern"_attr = info->getKeyPattern(),
                                 "keyValue"_attr = redact(info->getDuplicatedKeyValue()),
                                 "error"_attr = redact(e.reason()),
                                 "oplogEntry"_attr = redact(op->toBSONForLogging()));
            return e.toStatus();
        } catch (const DBException& e) {
            // SERVER-24927 If we have a NamespaceNotFound exception, then this document will be
            // dropped before initial sync or recovery ends anyways and we should ignore it.
            // Server will crash on oplog application failure during recovery from stable checkpoint
            // in the test environment.
            if (e.code() == ErrorCodes::NamespaceNotFound && op->isCrudOpType() &&
                allowNamespaceNotFoundErrorsOnCrudOps) {
                if (inStableRecovery) {
                    repl::OplogApplication::checkOnOplogFailureForRecovery(
                        opCtx, op->getNss(), redact(op->toBSONForLogging()), redact(e));
                } else {
                    LOGV2_DEBUG(
                        9067401,
                        2,
                        "Attempted to apply operation to missing namespace when this was allowed",
                        "error"_attr = redact(e),
                        "oplogEntry"_attr = redact(op->toBSONForLogging()));
                }
                continue;
            }

            LOGV2_FATAL_CONTINUE(21238,
                                 "Writer worker caught exception",
                                 "error"_attr = redact(e),
                                 "oplogEntry"_attr = redact(op->toBSONForLogging()));
            return e.toStatus();
        }
    }
    return Status::OK();
}

}  // namespace repl
}  // namespace mongo
