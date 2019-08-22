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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/process_interface_standalone.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/curop.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/repl/speculative_majority_read_info.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/transaction_coordinator_worker_curop_info.h"
#include "mongo/db/session_catalog.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/stats/fill_locker_info.h"
#include "mongo/db/stats/storage_stats.h"
#include "mongo/db/storage/backup_cursor_hooks.h"
#include "mongo/db/transaction_history_iterator.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/query/document_source_merge_cursors.h"
#include "mongo/util/log.h"

namespace mongo {

using boost::intrusive_ptr;
using std::shared_ptr;
using std::string;
using std::unique_ptr;
using write_ops::Insert;
using write_ops::Update;
using write_ops::UpdateOpEntry;

namespace {

class MongoDResourceYielder : public ResourceYielder {
public:
    void yield(OperationContext* opCtx) override {
        // We're about to block. Check back in the session so that it's available to other
        // threads. Note that we may block on a request to _ourselves_, meaning that we may have to
        // wait for another thread which will use the same session. This step is necessary
        // to prevent deadlocks.

        Session* const session = OperationContextSession::get(opCtx);
        if (session) {
            MongoDOperationContextSession::checkIn(opCtx);
        }
        _yielded = (session != nullptr);
    }

    void unyield(OperationContext* opCtx) override {
        if (_yielded) {
            // This may block on a sub-operation on this node finishing. It's possible that while
            // blocked on the network layer, another shard could have responded, theoretically
            // unblocking this thread of execution. However, we must wait until the child operation
            // on this shard finishes so we can get the session back. This may limit the throughput
            // of the operation, but it's correct.
            MongoDOperationContextSession::checkOut(opCtx,
                                                    // Assumes this is only called from the
                                                    // 'aggregate' or 'getMore' commands.  The code
                                                    // which relies on this parameter does not
                                                    // distinguish/care about the difference so we
                                                    // simply always pass 'aggregate'.
                                                    "aggregate");
        }
    }

private:
    bool _yielded = false;
};

// Returns true if the field names of 'keyPattern' are exactly those in 'uniqueKeyPaths', and each
// of the elements of 'keyPattern' is numeric, i.e. not "text", "$**", or any other special type of
// index.
bool keyPatternNamesExactPaths(const BSONObj& keyPattern,
                               const std::set<FieldPath>& uniqueKeyPaths) {
    size_t nFieldsMatched = 0;
    for (auto&& elem : keyPattern) {
        if (!elem.isNumber()) {
            return false;
        }
        if (uniqueKeyPaths.find(elem.fieldNameStringData()) == uniqueKeyPaths.end()) {
            return false;
        }
        ++nFieldsMatched;
    }
    return nFieldsMatched == uniqueKeyPaths.size();
}

bool supportsUniqueKey(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                       const IndexCatalogEntry* index,
                       const std::set<FieldPath>& uniqueKeyPaths) {
    return (index->descriptor()->unique() && !index->descriptor()->isPartial() &&
            keyPatternNamesExactPaths(index->descriptor()->keyPattern(), uniqueKeyPaths) &&
            CollatorInterface::collatorsMatch(index->getCollator(), expCtx->getCollator()));
}

}  // namespace

MongoInterfaceStandalone::MongoInterfaceStandalone(OperationContext* opCtx) : _client(opCtx) {}

void MongoInterfaceStandalone::setOperationContext(OperationContext* opCtx) {
    _client.setOpCtx(opCtx);
}

DBClientBase* MongoInterfaceStandalone::directClient() {
    return &_client;
}

std::unique_ptr<TransactionHistoryIteratorBase>
MongoInterfaceStandalone::createTransactionHistoryIterator(repl::OpTime time) const {
    bool permitYield = true;
    return std::unique_ptr<TransactionHistoryIteratorBase>(
        new TransactionHistoryIterator(time, permitYield));
}

bool MongoInterfaceStandalone::isSharded(OperationContext* opCtx, const NamespaceString& nss) {
    Lock::DBLock dbLock(opCtx, nss.db(), MODE_IS);
    Lock::CollectionLock collLock(opCtx, nss, MODE_IS);
    const auto metadata = CollectionShardingState::get(opCtx, nss)->getCurrentMetadata();
    return metadata->isSharded();
}

Insert MongoInterfaceStandalone::buildInsertOp(const NamespaceString& nss,
                                               std::vector<BSONObj>&& objs,
                                               bool bypassDocValidation) {
    Insert insertOp(nss);
    insertOp.setDocuments(std::move(objs));
    insertOp.setWriteCommandBase([&] {
        write_ops::WriteCommandBase wcb;
        wcb.setOrdered(false);
        wcb.setBypassDocumentValidation(bypassDocValidation);
        return wcb;
    }());
    return insertOp;
}

Update MongoInterfaceStandalone::buildUpdateOp(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    BatchedObjects&& batch,
    bool upsert,
    bool multi) {
    Update updateOp(nss);
    updateOp.setUpdates([&] {
        std::vector<UpdateOpEntry> updateEntries;
        for (auto&& obj : batch) {
            updateEntries.push_back([&] {
                UpdateOpEntry entry;
                auto&& [q, u, c] = obj;
                entry.setQ(std::move(q));
                entry.setU(std::move(u));
                entry.setC(std::move(c));
                entry.setUpsert(upsert);
                entry.setMulti(multi);
                return entry;
            }());
        }
        return updateEntries;
    }());
    updateOp.setWriteCommandBase([&] {
        write_ops::WriteCommandBase wcb;
        wcb.setOrdered(false);
        wcb.setBypassDocumentValidation(expCtx->bypassDocumentValidation);
        return wcb;
    }());
    updateOp.setRuntimeConstants(expCtx->getRuntimeConstants());
    return updateOp;
}

Status MongoInterfaceStandalone::insert(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                        const NamespaceString& ns,
                                        std::vector<BSONObj>&& objs,
                                        const WriteConcernOptions& wc,
                                        boost::optional<OID> targetEpoch) {
    auto writeResults = performInserts(
        expCtx->opCtx, buildInsertOp(ns, std::move(objs), expCtx->bypassDocumentValidation));

    // Need to check each result in the batch since the writes are unordered.
    for (const auto& result : writeResults.results) {
        if (result.getStatus() != Status::OK()) {
            return result.getStatus();
        }
    }
    return Status::OK();
}

StatusWith<MongoProcessInterface::UpdateResult> MongoInterfaceStandalone::update(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& ns,
    BatchedObjects&& batch,
    const WriteConcernOptions& wc,
    bool upsert,
    bool multi,
    boost::optional<OID> targetEpoch) {
    auto writeResults =
        performUpdates(expCtx->opCtx, buildUpdateOp(expCtx, ns, std::move(batch), upsert, multi));

    // Need to check each result in the batch since the writes are unordered.
    UpdateResult updateResult;
    for (const auto& result : writeResults.results) {
        if (result.getStatus() != Status::OK()) {
            return result.getStatus();
        }

        updateResult.nMatched += result.getValue().getN();
        updateResult.nModified += result.getValue().getNModified();
    }
    return updateResult;
}

CollectionIndexUsageMap MongoInterfaceStandalone::getIndexStats(OperationContext* opCtx,
                                                                const NamespaceString& ns) {
    AutoGetCollectionForReadCommand autoColl(opCtx, ns);

    Collection* collection = autoColl.getCollection();
    if (!collection) {
        LOG(2) << "Collection not found on index stats retrieval: " << ns.ns();
        return CollectionIndexUsageMap();
    }

    return CollectionQueryInfo::get(collection).getIndexUsageStats();
}

void MongoInterfaceStandalone::appendLatencyStats(OperationContext* opCtx,
                                                  const NamespaceString& nss,
                                                  bool includeHistograms,
                                                  BSONObjBuilder* builder) const {
    Top::get(opCtx->getServiceContext()).appendLatencyStats(nss, includeHistograms, builder);
}

Status MongoInterfaceStandalone::appendStorageStats(OperationContext* opCtx,
                                                    const NamespaceString& nss,
                                                    const BSONObj& param,
                                                    BSONObjBuilder* builder) const {
    return appendCollectionStorageStats(opCtx, nss, param, builder);
}

Status MongoInterfaceStandalone::appendRecordCount(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   BSONObjBuilder* builder) const {
    return appendCollectionRecordCount(opCtx, nss, builder);
}

Status MongoInterfaceStandalone::appendQueryExecStats(OperationContext* opCtx,
                                                      const NamespaceString& nss,
                                                      BSONObjBuilder* builder) const {
    AutoGetCollectionForReadCommand autoColl(opCtx, nss);

    if (!autoColl.getDb()) {
        return {ErrorCodes::NamespaceNotFound,
                str::stream() << "Database [" << nss.db().toString() << "] not found."};
    }

    Collection* collection = autoColl.getCollection();

    if (!collection) {
        return {ErrorCodes::NamespaceNotFound,
                str::stream() << "Collection [" << nss.toString() << "] not found."};
    }

    auto collectionScanStats = CollectionQueryInfo::get(collection).getCollectionScanStats();

    dassert(collectionScanStats.collectionScans <=
            static_cast<unsigned long long>(std::numeric_limits<long long>::max()));
    dassert(collectionScanStats.collectionScansNonTailable <=
            static_cast<unsigned long long>(std::numeric_limits<long long>::max()));
    builder->append("queryExecStats",
                    BSON("collectionScans" << BSON(
                             "total" << static_cast<long long>(collectionScanStats.collectionScans)
                                     << "nonTailable"
                                     << static_cast<long long>(
                                            collectionScanStats.collectionScansNonTailable))));

    return Status::OK();
}

BSONObj MongoInterfaceStandalone::getCollectionOptions(const NamespaceString& nss) {
    std::list<BSONObj> infos;

    try {
        infos = _client.getCollectionInfos(nss.db().toString(), BSON("name" << nss.coll()));
        if (infos.empty()) {
            return BSONObj();
        }
    } catch (const DBException& e) {
        uasserted(ErrorCodes::CommandFailed, e.reason());
    }

    const auto& infoObj = infos.front();
    uassert(ErrorCodes::CommandNotSupportedOnView,
            str::stream() << nss.toString() << " is a view, not a collection",
            infoObj["type"].valueStringData() != "view"_sd);

    return infoObj.getObjectField("options").getOwned();
}

void MongoInterfaceStandalone::renameIfOptionsAndIndexesHaveNotChanged(
    OperationContext* opCtx,
    const BSONObj& renameCommandObj,
    const NamespaceString& targetNs,
    const BSONObj& originalCollectionOptions,
    const std::list<BSONObj>& originalIndexes) {
    Lock::DBLock(opCtx, targetNs.db(), MODE_X);

    uassert(ErrorCodes::CommandFailed,
            str::stream() << "collection options of target collection " << targetNs.ns()
                          << " changed during processing. Original options: "
                          << originalCollectionOptions
                          << ", new options: " << getCollectionOptions(targetNs),
            SimpleBSONObjComparator::kInstance.evaluate(originalCollectionOptions ==
                                                        getCollectionOptions(targetNs)));

    auto currentIndexes = _client.getIndexSpecs(targetNs.ns());
    uassert(ErrorCodes::CommandFailed,
            str::stream() << "indexes of target collection " << targetNs.ns()
                          << " changed during processing.",
            originalIndexes.size() == currentIndexes.size() &&
                std::equal(originalIndexes.begin(),
                           originalIndexes.end(),
                           currentIndexes.begin(),
                           SimpleBSONObjComparator::kInstance.makeEqualTo()));

    BSONObj info;
    uassert(ErrorCodes::CommandFailed,
            str::stream() << "renameCollection failed: " << info,
            _client.runCommand("admin", renameCommandObj, info));
}

std::unique_ptr<Pipeline, PipelineDeleter> MongoInterfaceStandalone::makePipeline(
    const std::vector<BSONObj>& rawPipeline,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const MakePipelineOptions opts) {
    auto pipeline = uassertStatusOK(Pipeline::parse(rawPipeline, expCtx));

    if (opts.optimize) {
        pipeline->optimizePipeline();
    }

    if (opts.attachCursorSource) {
        pipeline = attachCursorSourceToPipeline(expCtx, pipeline.release());
    }

    return pipeline;
}

unique_ptr<Pipeline, PipelineDeleter> MongoInterfaceStandalone::attachCursorSourceToPipeline(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, Pipeline* ownedPipeline) {
    return attachCursorSourceToPipelineForLocalRead(expCtx, ownedPipeline);
}

unique_ptr<Pipeline, PipelineDeleter>
MongoInterfaceStandalone::attachCursorSourceToPipelineForLocalRead(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, Pipeline* ownedPipeline) {
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline(ownedPipeline,
                                                        PipelineDeleter(expCtx->opCtx));

    invariant(pipeline->getSources().empty() ||
              !dynamic_cast<DocumentSourceCursor*>(pipeline->getSources().front().get()));

    boost::optional<AutoGetCollectionForReadCommand> autoColl;
    const NamespaceStringOrUUID nsOrUUID = expCtx->uuid
        ? NamespaceStringOrUUID{expCtx->ns.db().toString(), *expCtx->uuid}
        : expCtx->ns;
    autoColl.emplace(expCtx->opCtx,
                     nsOrUUID,
                     AutoGetCollection::ViewMode::kViewsForbidden,
                     Date_t::max(),
                     AutoStatsTracker::LogMode::kUpdateTop);

    PipelineD::buildAndAttachInnerQueryExecutorToPipeline(
        autoColl->getCollection(), expCtx->ns, nullptr, pipeline.get());

    // Optimize again, since there may be additional optimizations that can be done after adding
    // the initial cursor stage.
    pipeline->optimizePipeline();

    return pipeline;
}

std::string MongoInterfaceStandalone::getShardName(OperationContext* opCtx) const {
    if (ShardingState::get(opCtx)->enabled()) {
        return ShardingState::get(opCtx)->shardId().toString();
    }

    return std::string();
}

std::pair<std::vector<FieldPath>, bool>
MongoInterfaceStandalone::collectDocumentKeyFieldsForHostedCollection(OperationContext* opCtx,
                                                                      const NamespaceString& nss,
                                                                      UUID uuid) const {
    return {{"_id"}, false};  // Nothing is sharded.
}

std::vector<FieldPath> MongoInterfaceStandalone::collectDocumentKeyFieldsActingAsRouter(
    OperationContext* opCtx, const NamespaceString& nss) const {
    return {"_id"};  // Nothing is sharded.
}

std::vector<GenericCursor> MongoInterfaceStandalone::getIdleCursors(
    const intrusive_ptr<ExpressionContext>& expCtx, CurrentOpUserMode userMode) const {
    return CursorManager::get(expCtx->opCtx)->getIdleCursors(expCtx->opCtx, userMode);
}

boost::optional<Document> MongoInterfaceStandalone::lookupSingleDocument(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    UUID collectionUUID,
    const Document& documentKey,
    boost::optional<BSONObj> readConcern,
    bool allowSpeculativeMajorityRead) {
    invariant(!readConcern);  // We don't currently support a read concern on mongod - it's only
                              // expected to be necessary on mongos.
    invariant(!allowSpeculativeMajorityRead);  // We don't expect 'allowSpeculativeMajorityRead' on
                                               // mongod - it's only expected to be necessary on
                                               // mongos.

    std::unique_ptr<Pipeline, PipelineDeleter> pipeline;
    try {
        // Be sure to do the lookup using the collection default collation
        auto foreignExpCtx = expCtx->copyWith(
            nss,
            collectionUUID,
            _getCollectionDefaultCollator(expCtx->opCtx, nss.db(), collectionUUID));
        pipeline = makePipeline({BSON("$match" << documentKey)}, foreignExpCtx);
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        return boost::none;
    }

    auto lookedUpDocument = pipeline->getNext();
    if (auto next = pipeline->getNext()) {
        uasserted(ErrorCodes::TooManyMatchingDocuments,
                  str::stream() << "found more than one document with document key "
                                << documentKey.toString() << " [" << lookedUpDocument->toString()
                                << ", " << next->toString() << "]");
    }

    // Set the speculative read timestamp appropriately after we do a document lookup locally. We
    // set the speculative read timestamp based on the timestamp used by the transaction.
    repl::SpeculativeMajorityReadInfo& speculativeMajorityReadInfo =
        repl::SpeculativeMajorityReadInfo::get(expCtx->opCtx);
    if (speculativeMajorityReadInfo.isSpeculativeRead()) {
        // Speculative majority reads are required to use the 'kNoOverlap' read source.
        invariant(expCtx->opCtx->recoveryUnit()->getTimestampReadSource() ==
                  RecoveryUnit::ReadSource::kNoOverlap);
        boost::optional<Timestamp> readTs =
            expCtx->opCtx->recoveryUnit()->getPointInTimeReadTimestamp();
        invariant(readTs);
        speculativeMajorityReadInfo.setSpeculativeReadTimestampForward(*readTs);
    }

    return lookedUpDocument;
}

BackupCursorState MongoInterfaceStandalone::openBackupCursor(OperationContext* opCtx) {
    auto backupCursorHooks = BackupCursorHooks::get(opCtx->getServiceContext());
    if (backupCursorHooks->enabled()) {
        return backupCursorHooks->openBackupCursor(opCtx);
    } else {
        uasserted(50956, "Backup cursors are an enterprise only feature.");
    }
}

void MongoInterfaceStandalone::closeBackupCursor(OperationContext* opCtx, const UUID& backupId) {
    auto backupCursorHooks = BackupCursorHooks::get(opCtx->getServiceContext());
    if (backupCursorHooks->enabled()) {
        backupCursorHooks->closeBackupCursor(opCtx, backupId);
    } else {
        uasserted(50955, "Backup cursors are an enterprise only feature.");
    }
}

BackupCursorExtendState MongoInterfaceStandalone::extendBackupCursor(OperationContext* opCtx,
                                                                     const UUID& backupId,
                                                                     const Timestamp& extendTo) {
    auto backupCursorHooks = BackupCursorHooks::get(opCtx->getServiceContext());
    if (backupCursorHooks->enabled()) {
        return backupCursorHooks->extendBackupCursor(opCtx, backupId, extendTo);
    } else {
        uasserted(51010, "Backup cursors are an enterprise only feature.");
    }
}

std::vector<BSONObj> MongoInterfaceStandalone::getMatchingPlanCacheEntryStats(
    OperationContext* opCtx, const NamespaceString& nss, const MatchExpression* matchExp) const {
    const auto serializer = [](const PlanCacheEntry& entry) {
        BSONObjBuilder out;
        Explain::planCacheEntryToBSON(entry, &out);
        return out.obj();
    };

    const auto predicate = [&matchExp](const BSONObj& obj) {
        return !matchExp ? true : matchExp->matchesBSON(obj);
    };

    AutoGetCollection autoColl(opCtx, nss, MODE_IS);
    const auto collection = autoColl.getCollection();
    uassert(
        50933, str::stream() << "collection '" << nss.toString() << "' does not exist", collection);

    const auto planCache = CollectionQueryInfo::get(collection).getPlanCache();
    invariant(planCache);

    return planCache->getMatchingStats(serializer, predicate);
}

bool MongoInterfaceStandalone::fieldsHaveSupportingUniqueIndex(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    const std::set<FieldPath>& fieldPaths) const {
    auto* opCtx = expCtx->opCtx;
    // We purposefully avoid a helper like AutoGetCollection here because we don't want to check the
    // db version or do anything else. We simply want to protect against concurrent modifications to
    // the catalog.
    Lock::DBLock dbLock(opCtx, nss.db(), MODE_IS);
    Lock::CollectionLock collLock(opCtx, nss, MODE_IS);
    auto databaseHolder = DatabaseHolder::get(opCtx);
    auto db = databaseHolder->getDb(opCtx, nss.db());
    auto collection = db ? db->getCollection(opCtx, nss) : nullptr;
    if (!collection) {
        return fieldPaths == std::set<FieldPath>{"_id"};
    }

    auto indexIterator = collection->getIndexCatalog()->getIndexIterator(opCtx, false);
    while (indexIterator->more()) {
        const IndexCatalogEntry* entry = indexIterator->next();
        if (supportsUniqueKey(expCtx, entry, fieldPaths)) {
            return true;
        }
    }
    return false;
}

BSONObj MongoInterfaceStandalone::_reportCurrentOpForClient(
    OperationContext* opCtx,
    Client* client,
    CurrentOpTruncateMode truncateOps,
    CurrentOpBacktraceMode backtraceMode) const {
    BSONObjBuilder builder;

    CurOp::reportCurrentOpForClient(opCtx,
                                    client,
                                    (truncateOps == CurrentOpTruncateMode::kTruncateOps),
                                    (backtraceMode == CurrentOpBacktraceMode::kIncludeBacktrace),
                                    &builder);

    OperationContext* clientOpCtx = client->getOperationContext();

    if (clientOpCtx) {
        if (auto txnParticipant = TransactionParticipant::get(clientOpCtx)) {
            txnParticipant.reportUnstashedState(clientOpCtx, &builder);
        }

        // Append lock stats before returning.
        if (auto lockerInfo = clientOpCtx->lockState()->getLockerInfo(
                CurOp::get(*clientOpCtx)->getLockStatsBase())) {
            fillLockerInfo(*lockerInfo, builder);
        }

        if (auto tcWorkerRepo = getTransactionCoordinatorWorkerCurOpRepository()) {
            tcWorkerRepo->reportState(clientOpCtx, &builder);
        }

        auto flowControlStats = clientOpCtx->lockState()->getFlowControlStats();
        flowControlStats.writeToBuilder(builder);
    }

    return builder.obj();
}

void MongoInterfaceStandalone::_reportCurrentOpsForIdleSessions(OperationContext* opCtx,
                                                                CurrentOpUserMode userMode,
                                                                std::vector<BSONObj>* ops) const {
    auto sessionCatalog = SessionCatalog::get(opCtx);

    const bool authEnabled =
        AuthorizationSession::get(opCtx->getClient())->getAuthorizationManager().isAuthEnabled();

    // If the user is listing only their own ops, we use makeSessionFilterForAuthenticatedUsers to
    // create a pattern that will match against all authenticated usernames for the current client.
    // If the user is listing ops for all users, we create an empty pattern; constructing an
    // instance of SessionKiller::Matcher with this empty pattern will return all sessions.
    auto sessionFilter = (authEnabled && userMode == CurrentOpUserMode::kExcludeOthers
                              ? makeSessionFilterForAuthenticatedUsers(opCtx)
                              : KillAllSessionsByPatternSet{{}});

    sessionCatalog->scanSessions({std::move(sessionFilter)}, [&](const ObservableSession& session) {
        auto op = TransactionParticipant::get(session).reportStashedState(opCtx);
        if (!op.isEmpty()) {
            ops->emplace_back(op);
        }
    });
}

std::unique_ptr<CollatorInterface> MongoInterfaceStandalone::_getCollectionDefaultCollator(
    OperationContext* opCtx, StringData dbName, UUID collectionUUID) {
    auto it = _collatorCache.find(collectionUUID);
    if (it == _collatorCache.end()) {
        auto collator = [&]() -> std::unique_ptr<CollatorInterface> {
            AutoGetCollection autoColl(opCtx, {dbName.toString(), collectionUUID}, MODE_IS);
            if (!autoColl.getCollection()) {
                // This collection doesn't exist, so assume a nullptr default collation
                return nullptr;
            } else {
                auto defaultCollator = autoColl.getCollection()->getDefaultCollator();
                // Clone the collator so that we can safely use the pointer if the collection
                // disappears right after we release the lock.
                return defaultCollator ? defaultCollator->clone() : nullptr;
            }
        }();

        it = _collatorCache.emplace(collectionUUID, std::move(collator)).first;
    }

    auto& collator = it->second;
    return collator ? collator->clone() : nullptr;
}

std::unique_ptr<ResourceYielder> MongoInterfaceStandalone::getResourceYielder() const {
    return std::make_unique<MongoDResourceYielder>();
}


std::pair<std::set<FieldPath>, boost::optional<ChunkVersion>>
MongoInterfaceStandalone::ensureFieldsUniqueOrResolveDocumentKey(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::optional<std::vector<std::string>> fields,
    boost::optional<ChunkVersion> targetCollectionVersion,
    const NamespaceString& outputNs) const {
    if (targetCollectionVersion) {
        uassert(51123, "Unexpected target chunk version specified", expCtx->fromMongos);
        // If mongos has sent us a target shard version, we need to be sure we are prepared to
        // act as a router which is at least as recent as that mongos.
        checkRoutingInfoEpochOrThrow(expCtx, outputNs, *targetCollectionVersion);
    }

    if (!fields) {
        uassert(51124, "Expected fields to be provided from mongos", !expCtx->fromMongos);
        return {std::set<FieldPath>{"_id"}, targetCollectionVersion};
    }

    // Make sure the 'fields' array has a supporting index. Skip this check if the command is sent
    // from mongos since the 'fields' check would've happened already.
    auto fieldPaths = _convertToFieldPaths(*fields);
    if (!expCtx->fromMongos) {
        uassert(51183,
                "Cannot find index to verify that join fields will be unique",
                fieldsHaveSupportingUniqueIndex(expCtx, outputNs, fieldPaths));
    }
    return {fieldPaths, targetCollectionVersion};
}

}  // namespace mongo
