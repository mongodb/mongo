/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/mongod_process_interface.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/session_catalog.h"
#include "mongo/db/stats/fill_locker_info.h"
#include "mongo/db/stats/storage_stats.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace mongo {

using boost::intrusive_ptr;
using std::shared_ptr;
using std::string;
using std::unique_ptr;
using write_ops::Update;

MongoDInterface::MongoDInterface(OperationContext* opCtx) : _client(opCtx) {}

void MongoDInterface::setOperationContext(OperationContext* opCtx) {
    _client.setOpCtx(opCtx);
}

DBClientBase* MongoDInterface::directClient() {
    return &_client;
}

bool MongoDInterface::isSharded(OperationContext* opCtx, const NamespaceString& nss) {
    AutoGetCollectionForReadCommand autoColl(opCtx, nss);
    auto const css = CollectionShardingState::get(opCtx, nss);
    return css->getMetadata(opCtx)->isSharded();
}

void MongoDInterface::insert(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                             const NamespaceString& ns,
                             const std::vector<BSONObj>& objs) {
    boost::optional<DisableDocumentValidation> maybeDisableValidation;
    if (expCtx->bypassDocumentValidation)
        maybeDisableValidation.emplace(expCtx->opCtx);

    _client.insert(ns.ns(), objs);
    uassert(16996,
            str::stream() << "Insert failed: " << _client.getLastError(),
            _client.getLastError().empty());
}

void MongoDInterface::update(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                             const NamespaceString& ns,
                             const std::vector<BSONObj>& queries,
                             const std::vector<BSONObj>& updates,
                             bool upsert,
                             bool multi) {
    BSONObjBuilder updateBob;
    updateBob.append(Update::kCommandName, ns.coll());
    updateBob.append(Update::kDbNameFieldName, ns.db());
    updateBob.append(Update::kBypassDocumentValidationFieldName, expCtx->bypassDocumentValidation);

    // Build the array of UpdateOp entries.
    invariant(queries.size() == updates.size());
    BSONArrayBuilder updatesArray;
    for (size_t index = 0; index < queries.size(); ++index) {
        updatesArray.append(BSON("q" << queries[index] << "u" << updates[index] << "multi" << multi
                                     << "upsert"
                                     << upsert));
    }
    updateBob.append(Update::kUpdatesFieldName, updatesArray.arr());

    auto updateObj = updateBob.done();
    auto writeResults =
        performUpdates(expCtx->opCtx, Update::parse(IDLParserErrorContext("update"), updateObj));

    // Verify that each of the update results is successful.
    for (const auto& result : writeResults.results) {
        uassert(50904,
                str::stream() << "Update failed: " << result.getStatus(),
                result.getStatus() == Status::OK());
    }
}

CollectionIndexUsageMap MongoDInterface::getIndexStats(OperationContext* opCtx,
                                                       const NamespaceString& ns) {
    AutoGetCollectionForReadCommand autoColl(opCtx, ns);

    Collection* collection = autoColl.getCollection();
    if (!collection) {
        LOG(2) << "Collection not found on index stats retrieval: " << ns.ns();
        return CollectionIndexUsageMap();
    }

    return collection->infoCache()->getIndexUsageStats();
}

void MongoDInterface::appendLatencyStats(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         bool includeHistograms,
                                         BSONObjBuilder* builder) const {
    Top::get(opCtx->getServiceContext()).appendLatencyStats(nss.ns(), includeHistograms, builder);
}

Status MongoDInterface::appendStorageStats(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           const BSONObj& param,
                                           BSONObjBuilder* builder) const {
    return appendCollectionStorageStats(opCtx, nss, param, builder);
}

Status MongoDInterface::appendRecordCount(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          BSONObjBuilder* builder) const {
    return appendCollectionRecordCount(opCtx, nss, builder);
}

BSONObj MongoDInterface::getCollectionOptions(const NamespaceString& nss) {
    const auto infos = _client.getCollectionInfos(nss.db().toString(), BSON("name" << nss.coll()));
    return infos.empty() ? BSONObj() : infos.front().getObjectField("options").getOwned();
}

void MongoDInterface::renameIfOptionsAndIndexesHaveNotChanged(
    OperationContext* opCtx,
    const BSONObj& renameCommandObj,
    const NamespaceString& targetNs,
    const BSONObj& originalCollectionOptions,
    const std::list<BSONObj>& originalIndexes) {
    Lock::GlobalWrite globalLock(opCtx);

    uassert(ErrorCodes::CommandFailed,
            str::stream() << "collection options of target collection " << targetNs.ns()
                          << " changed during processing. Original options: "
                          << originalCollectionOptions
                          << ", new options: "
                          << getCollectionOptions(targetNs),
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

StatusWith<std::unique_ptr<Pipeline, PipelineDeleter>> MongoDInterface::makePipeline(
    const std::vector<BSONObj>& rawPipeline,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const MakePipelineOptions opts) {
    auto pipeline = Pipeline::parse(rawPipeline, expCtx);
    if (!pipeline.isOK()) {
        return pipeline.getStatus();
    }

    if (opts.optimize) {
        pipeline.getValue()->optimizePipeline();
    }

    Status cursorStatus = Status::OK();

    if (opts.attachCursorSource) {
        cursorStatus = attachCursorSourceToPipeline(expCtx, pipeline.getValue().get());
    }

    return cursorStatus.isOK() ? std::move(pipeline) : cursorStatus;
}

Status MongoDInterface::attachCursorSourceToPipeline(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, Pipeline* pipeline) {
    invariant(pipeline->getSources().empty() ||
              !dynamic_cast<DocumentSourceCursor*>(pipeline->getSources().front().get()));

    boost::optional<AutoGetCollectionForReadCommand> autoColl;
    if (expCtx->uuid) {
        try {
            autoColl.emplace(expCtx->opCtx,
                             NamespaceStringOrUUID{expCtx->ns.db().toString(), *expCtx->uuid});
        } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>& ex) {
            // The UUID doesn't exist anymore
            return ex.toStatus();
        }
    } else {
        autoColl.emplace(expCtx->opCtx, expCtx->ns);
    }

    // makePipeline() is only called to perform secondary aggregation requests and expects the
    // collection representing the document source to be not-sharded. We confirm sharding state
    // here to avoid taking a collection lock elsewhere for this purpose alone.
    // TODO SERVER-27616: This check is incorrect in that we don't acquire a collection cursor
    // until after we release the lock, leaving room for a collection to be sharded in-between.
    auto css = CollectionShardingState::get(expCtx->opCtx, expCtx->ns);
    uassert(4567,
            str::stream() << "from collection (" << expCtx->ns.ns() << ") cannot be sharded",
            !css->getMetadata(expCtx->opCtx)->isSharded());

    PipelineD::prepareCursorSource(autoColl->getCollection(), expCtx->ns, nullptr, pipeline);

    return Status::OK();
}

std::string MongoDInterface::getShardName(OperationContext* opCtx) const {
    if (ShardingState::get(opCtx)->enabled()) {
        return ShardingState::get(opCtx)->shardId().toString();
    }

    return std::string();
}

std::pair<std::vector<FieldPath>, bool> MongoDInterface::collectDocumentKeyFields(
    OperationContext* opCtx, UUID uuid) const {
    if (serverGlobalParams.clusterRole != ClusterRole::ShardServer) {
        return {{"_id"}, false};  // Nothing is sharded.
    }

    // An empty namespace indicates that the collection has been dropped. Treat it as unsharded and
    // mark the fields as final.
    auto nss = UUIDCatalog::get(opCtx).lookupNSSByUUID(uuid);
    if (nss.isEmpty()) {
        return {{"_id"}, true};
    }

    // Before taking a collection lock to retrieve the shard key fields, consult the catalog cache
    // to determine whether the collection is sharded in the first place.
    auto catalogCache = Grid::get(opCtx)->catalogCache();

    const bool collectionIsSharded = catalogCache && [&]() {
        auto routingInfo = catalogCache->getCollectionRoutingInfo(opCtx, nss);
        return routingInfo.isOK() && routingInfo.getValue().cm();
    }();

    // Collection exists and is not sharded, mark as not final.
    if (!collectionIsSharded) {
        return {{"_id"}, false};
    }

    auto scm = [opCtx, &nss]() -> ScopedCollectionMetadata {
        AutoGetCollection autoColl(opCtx, nss, MODE_IS);
        return CollectionShardingState::get(opCtx, nss)->getMetadata(opCtx);
    }();

    // Collection is not sharded or UUID mismatch implies collection has been dropped and recreated
    // as sharded.
    if (!scm->isSharded() || !scm->uuidMatches(uuid)) {
        return {{"_id"}, false};
    }

    // Unpack the shard key.
    std::vector<FieldPath> result;
    bool gotId = false;
    for (auto& field : scm->getKeyPatternFields()) {
        result.emplace_back(field->dottedField());
        gotId |= (result.back().fullPath() == "_id");
    }
    if (!gotId) {  // If not part of the shard key, "_id" comes last.
        result.emplace_back("_id");
    }
    // Collection is now sharded so the document key fields will never change, mark as final.
    return {result, true};
}

std::vector<GenericCursor> MongoDInterface::getCursors(
    const intrusive_ptr<ExpressionContext>& expCtx) const {
    return CursorManager::getAllCursors(expCtx->opCtx);
}

boost::optional<Document> MongoDInterface::lookupSingleDocument(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    UUID collectionUUID,
    const Document& documentKey,
    boost::optional<BSONObj> readConcern) {
    invariant(!readConcern);  // We don't currently support a read concern on mongod - it's only
                              // expected to be necessary on mongos.

    std::unique_ptr<Pipeline, PipelineDeleter> pipeline;
    try {
        // Be sure to do the lookup using the collection default collation
        auto foreignExpCtx = expCtx->copyWith(
            nss,
            collectionUUID,
            _getCollectionDefaultCollator(expCtx->opCtx, nss.db(), collectionUUID));
        pipeline = uassertStatusOK(makePipeline({BSON("$match" << documentKey)}, foreignExpCtx));
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        return boost::none;
    }

    auto lookedUpDocument = pipeline->getNext();
    if (auto next = pipeline->getNext()) {
        uasserted(ErrorCodes::TooManyMatchingDocuments,
                  str::stream() << "found more than one document with document key "
                                << documentKey.toString()
                                << " ["
                                << lookedUpDocument->toString()
                                << ", "
                                << next->toString()
                                << "]");
    }
    return lookedUpDocument;
}

BSONObj MongoDInterface::_reportCurrentOpForClient(OperationContext* opCtx,
                                                   Client* client,
                                                   CurrentOpTruncateMode truncateOps) const {
    BSONObjBuilder builder;

    CurOp::reportCurrentOpForClient(
        opCtx, client, (truncateOps == CurrentOpTruncateMode::kTruncateOps), &builder);

    OperationContext* clientOpCtx = client->getOperationContext();

    if (clientOpCtx) {
        if (auto opCtxSession = OperationContextSession::get(clientOpCtx)) {
            opCtxSession->reportUnstashedState(repl::ReadConcernArgs::get(clientOpCtx), &builder);
        }

        // Append lock stats before returning.
        if (auto lockerInfo = clientOpCtx->lockState()->getLockerInfo()) {
            fillLockerInfo(*lockerInfo, builder);
        }
    }

    return builder.obj();
}

void MongoDInterface::_reportCurrentOpsForIdleSessions(OperationContext* opCtx,
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

    sessionCatalog->scanSessions(opCtx,
                                 {std::move(sessionFilter)},
                                 [&](OperationContext* opCtx, Session* session) {
                                     auto op = session->reportStashedState();
                                     if (!op.isEmpty()) {
                                         ops->emplace_back(op);
                                     }
                                 });
}

std::unique_ptr<CollatorInterface> MongoDInterface::_getCollectionDefaultCollator(
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

}  // namespace mongo
