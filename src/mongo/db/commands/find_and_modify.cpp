/**
 *    Copyright (C) 2012-2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include <boost/optional.hpp>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/find_and_modify_common.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/update.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/delete_request.h"
#include "mongo/db/ops/find_and_modify_result.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/ops/parsed_delete.h"
#include "mongo/db/ops/parsed_update.h"
#include "mongo/db/ops/update_lifecycle_impl.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/ops/write_ops_retryability.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/find_and_modify_request.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/retryable_writes_stats.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/session_catalog.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/write_concern.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

const UpdateStats* getUpdateStats(const PlanExecutor* exec) {
    // The stats may refer to an update stage, or a projection stage wrapping an update stage.
    if (StageType::STAGE_PROJECTION == exec->getRootStage()->stageType()) {
        invariant(exec->getRootStage()->getChildren().size() == 1U);
        invariant(StageType::STAGE_UPDATE == exec->getRootStage()->child()->stageType());
        const SpecificStats* stats = exec->getRootStage()->child()->getSpecificStats();
        return static_cast<const UpdateStats*>(stats);
    } else {
        invariant(StageType::STAGE_UPDATE == exec->getRootStage()->stageType());
        return static_cast<const UpdateStats*>(exec->getRootStage()->getSpecificStats());
    }
}

const DeleteStats* getDeleteStats(const PlanExecutor* exec) {
    // The stats may refer to a delete stage, or a projection stage wrapping a delete stage.
    if (StageType::STAGE_PROJECTION == exec->getRootStage()->stageType()) {
        invariant(exec->getRootStage()->getChildren().size() == 1U);
        invariant(StageType::STAGE_DELETE == exec->getRootStage()->child()->stageType());
        const SpecificStats* stats = exec->getRootStage()->child()->getSpecificStats();
        return static_cast<const DeleteStats*>(stats);
    } else {
        invariant(StageType::STAGE_DELETE == exec->getRootStage()->stageType());
        return static_cast<const DeleteStats*>(exec->getRootStage()->getSpecificStats());
    }
}

/**
 * If the operation succeeded, then Status::OK() is returned, possibly with a document value
 * to return to the client. If no matching document to update or remove was found, then none
 * is returned. Otherwise, the updated or deleted document is returned.
 *
 * If the operation failed, throws.
 */
boost::optional<BSONObj> advanceExecutor(OperationContext* opCtx,
                                         PlanExecutor* exec,
                                         bool isRemove) {
    BSONObj value;
    PlanExecutor::ExecState state = exec->getNext(&value, nullptr);

    if (PlanExecutor::ADVANCED == state) {
        return {std::move(value)};
    }

    if (PlanExecutor::FAILURE == state || PlanExecutor::DEAD == state) {
        error() << "Plan executor error during findAndModify: " << PlanExecutor::statestr(state)
                << ", stats: " << redact(Explain::getWinningPlanStats(exec));

        uassertStatusOKWithContext(WorkingSetCommon::getMemberObjectStatus(value),
                                   "Plan executor error during findAndModify");
        MONGO_UNREACHABLE;
    }

    invariant(state == PlanExecutor::IS_EOF);
    return boost::none;
}

void makeUpdateRequest(const OperationContext* opCtx,
                       const FindAndModifyRequest& args,
                       bool explain,
                       UpdateLifecycleImpl* updateLifecycle,
                       UpdateRequest* requestOut) {
    requestOut->setQuery(args.getQuery());
    requestOut->setProj(args.getFields());
    requestOut->setUpdates(args.getUpdateObj());
    requestOut->setSort(args.getSort());
    requestOut->setCollation(args.getCollation());
    requestOut->setArrayFilters(args.getArrayFilters());
    requestOut->setUpsert(args.isUpsert());
    requestOut->setReturnDocs(args.shouldReturnNew() ? UpdateRequest::RETURN_NEW
                                                     : UpdateRequest::RETURN_OLD);
    requestOut->setMulti(false);
    requestOut->setExplain(explain);
    requestOut->setLifecycle(updateLifecycle);

    const auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    requestOut->setYieldPolicy(readConcernArgs.getLevel() ==
                                       repl::ReadConcernLevel::kSnapshotReadConcern
                                   ? PlanExecutor::INTERRUPT_ONLY
                                   : PlanExecutor::YIELD_AUTO);
}

void makeDeleteRequest(const OperationContext* opCtx,
                       const FindAndModifyRequest& args,
                       bool explain,
                       DeleteRequest* requestOut) {
    requestOut->setQuery(args.getQuery());
    requestOut->setProj(args.getFields());
    requestOut->setSort(args.getSort());
    requestOut->setCollation(args.getCollation());
    requestOut->setMulti(false);
    requestOut->setReturnDeleted(true);  // Always return the old value.
    requestOut->setExplain(explain);

    const auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    requestOut->setYieldPolicy(readConcernArgs.getLevel() ==
                                       repl::ReadConcernLevel::kSnapshotReadConcern
                                   ? PlanExecutor::INTERRUPT_ONLY
                                   : PlanExecutor::YIELD_AUTO);
}

void appendCommandResponse(const PlanExecutor* exec,
                           bool isRemove,
                           const boost::optional<BSONObj>& value,
                           BSONObjBuilder* result) {
    if (isRemove) {
        find_and_modify::serializeRemove(getDeleteStats(exec)->docsDeleted, value, result);
    } else {
        const auto updateStats = getUpdateStats(exec);

        // Note we have to use the objInserted from the stats here, rather than 'value' because the
        // _id field could have been excluded by a projection.
        find_and_modify::serializeUpsert(updateStats->inserted ? 1 : updateStats->nMatched,
                                         value,
                                         updateStats->nMatched > 0,
                                         updateStats->objInserted,
                                         result);
    }
}

void assertCanWrite(OperationContext* opCtx, const NamespaceString& nsString) {
    uassert(ErrorCodes::NotMaster,
            str::stream() << "Not primary while running findAndModify command on collection "
                          << nsString.ns(),
            repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nsString));

    // Check for shard version match
    auto css = CollectionShardingState::get(opCtx, nsString);
    css->checkShardVersionOrThrow(opCtx);
}

void recordStatsForTopCommand(OperationContext* opCtx) {
    auto curOp = CurOp::get(opCtx);
    Top::get(opCtx->getClient()->getServiceContext())
        .record(opCtx,
                curOp->getNS(),
                curOp->getLogicalOp(),
                Top::LockType::WriteLocked,
                durationCount<Microseconds>(curOp->elapsedTimeExcludingPauses()),
                curOp->isCommand(),
                curOp->getReadWriteType());
}

class CmdFindAndModify : public BasicCommand {
public:
    CmdFindAndModify() : BasicCommand("findAndModify", "findandmodify") {}

    std::string help() const override {
        return "{ findAndModify: \"collection\", query: {processed:false}, update: {$set: "
               "{processed:true}}, new: true}\n"
               "{ findAndModify: \"collection\", query: {processed:false}, remove: true, sort: "
               "{priority:-1}}\n"
               "Either update or remove is required, all other fields have default values.\n"
               "Output is in the \"value\" field\n";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool supportsReadConcern(const std::string& dbName,
                             const BSONObj& cmdObj,
                             repl::ReadConcernLevel level) const final {
        return level == repl::ReadConcernLevel::kLocalReadConcern ||
            level == repl::ReadConcernLevel::kSnapshotReadConcern;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {
        find_and_modify::addPrivilegesRequiredForFindAndModify(this, dbname, cmdObj, out);
    }

    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kWrite;
    }

    Status explain(OperationContext* opCtx,
                   const OpMsgRequest& request,
                   ExplainOptions::Verbosity verbosity,
                   BSONObjBuilder* out) const override {
        std::string dbName = request.getDatabase().toString();
        const BSONObj& cmdObj = request.body;
        const auto args(uassertStatusOK(FindAndModifyRequest::parseFromBSON(
            CommandHelpers::parseNsCollectionRequired(dbName, cmdObj), cmdObj)));
        const NamespaceString& nsString = args.getNamespaceString();
        uassertStatusOK(userAllowedWriteNS(nsString));
        auto const curOp = CurOp::get(opCtx);
        OpDebug* const opDebug = &curOp->debug();

        if (args.isRemove()) {
            DeleteRequest request(nsString);
            const bool isExplain = true;
            makeDeleteRequest(opCtx, args, isExplain, &request);

            ParsedDelete parsedDelete(opCtx, &request);
            uassertStatusOK(parsedDelete.parseRequest());

            // Explain calls of the findAndModify command are read-only, but we take write
            // locks so that the timing information is more accurate.
            AutoGetCollection autoColl(opCtx, nsString, MODE_IX);
            uassert(ErrorCodes::NamespaceNotFound,
                    str::stream() << "database " << dbName << " does not exist",
                    autoColl.getDb());

            auto css = CollectionShardingState::get(opCtx, nsString);
            css->checkShardVersionOrThrow(opCtx);

            Collection* const collection = autoColl.getCollection();
            const auto exec =
                uassertStatusOK(getExecutorDelete(opCtx, opDebug, collection, &parsedDelete));

            Explain::explainStages(exec.get(), collection, verbosity, out);
        } else {
            UpdateRequest request(nsString);
            UpdateLifecycleImpl updateLifecycle(nsString);
            const bool isExplain = true;
            makeUpdateRequest(opCtx, args, isExplain, &updateLifecycle, &request);

            ParsedUpdate parsedUpdate(opCtx, &request);
            uassertStatusOK(parsedUpdate.parseRequest());

            // Explain calls of the findAndModify command are read-only, but we take write
            // locks so that the timing information is more accurate.
            AutoGetCollection autoColl(opCtx, nsString, MODE_IX);
            uassert(ErrorCodes::NamespaceNotFound,
                    str::stream() << "database " << dbName << " does not exist",
                    autoColl.getDb());

            auto css = CollectionShardingState::get(opCtx, nsString);
            css->checkShardVersionOrThrow(opCtx);

            Collection* const collection = autoColl.getCollection();
            const auto exec =
                uassertStatusOK(getExecutorUpdate(opCtx, opDebug, collection, &parsedUpdate));

            Explain::explainStages(exec.get(), collection, verbosity, out);
        }

        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const auto args(uassertStatusOK(FindAndModifyRequest::parseFromBSON(
            CommandHelpers::parseNsCollectionRequired(dbName, cmdObj), cmdObj)));
        const NamespaceString& nsString = args.getNamespaceString();
        uassertStatusOK(userAllowedWriteNS(nsString));
        auto const curOp = CurOp::get(opCtx);
        OpDebug* const opDebug = &curOp->debug();

        boost::optional<DisableDocumentValidation> maybeDisableValidation;
        if (shouldBypassDocumentValidationForCommand(cmdObj))
            maybeDisableValidation.emplace(opCtx);

        const auto session = OperationContextSession::get(opCtx);
        const auto inTransaction = session && session->inMultiDocumentTransaction();
        uassert(50781,
                str::stream() << "Cannot write to system collection " << nsString.ns()
                              << " within a transaction.",
                !(inTransaction && nsString.isSystem()));

        const auto replCoord = repl::ReplicationCoordinator::get(opCtx->getServiceContext());
        uassert(50777,
                str::stream() << "Cannot write to unreplicated collection " << nsString.ns()
                              << " within a transaction.",
                !(inTransaction && replCoord->isOplogDisabledFor(opCtx, nsString)));


        const auto stmtId = 0;
        if (opCtx->getTxnNumber()) {
            auto session = OperationContextSession::get(opCtx);
            if (auto entry =
                    session->checkStatementExecuted(opCtx, *opCtx->getTxnNumber(), stmtId)) {
                RetryableWritesStats::get(opCtx)->incrementRetriedCommandsCount();
                RetryableWritesStats::get(opCtx)->incrementRetriedStatementsCount();
                parseOplogEntryForFindAndModify(opCtx, args, *entry, &result);

                // Make sure to wait for writeConcern on the opTime that will include this write.
                // Needs to set to the system last opTime to get the latest term in an event when
                // an election happened after the actual write.
                auto& replClient = repl::ReplClientInfo::forClient(opCtx->getClient());
                replClient.setLastOpToSystemLastOpTime(opCtx);

                return true;
            }
        }

        // Although usually the PlanExecutor handles WCE internally, it will throw WCEs when it is
        // executing a findAndModify. This is done to ensure that we can always match, modify, and
        // return the document under concurrency, if a matching document exists.
        return writeConflictRetry(opCtx, "findAndModify", nsString.ns(), [&] {
            if (args.isRemove()) {
                DeleteRequest request(nsString);
                const bool isExplain = false;
                makeDeleteRequest(opCtx, args, isExplain, &request);

                if (opCtx->getTxnNumber()) {
                    request.setStmtId(stmtId);
                }

                ParsedDelete parsedDelete(opCtx, &request);
                uassertStatusOK(parsedDelete.parseRequest());

                AutoGetCollection autoColl(opCtx, nsString, MODE_IX);

                {
                    boost::optional<int> dbProfilingLevel;
                    if (autoColl.getDb())
                        dbProfilingLevel = autoColl.getDb()->getProfilingLevel();

                    stdx::lock_guard<Client> lk(*opCtx->getClient());
                    CurOp::get(opCtx)->enter_inlock(nsString.ns().c_str(), dbProfilingLevel);
                }

                assertCanWrite(opCtx, nsString);

                Collection* const collection = autoColl.getCollection();
                const auto exec =
                    uassertStatusOK(getExecutorDelete(opCtx, opDebug, collection, &parsedDelete));

                {
                    stdx::lock_guard<Client> lk(*opCtx->getClient());
                    CurOp::get(opCtx)->setPlanSummary_inlock(Explain::getPlanSummary(exec.get()));
                }

                auto docFound = advanceExecutor(opCtx, exec.get(), args.isRemove());
                // Nothing after advancing the plan executor should throw a WriteConflictException,
                // so the following bookkeeping with execution stats won't end up being done
                // multiple times.

                PlanSummaryStats summaryStats;
                Explain::getSummaryStats(*exec, &summaryStats);
                if (collection) {
                    collection->infoCache()->notifyOfQuery(opCtx, summaryStats.indexesUsed);
                }
                opDebug->setPlanSummaryMetrics(summaryStats);

                // Fill out OpDebug with the number of deleted docs.
                opDebug->ndeleted = getDeleteStats(exec.get())->docsDeleted;

                if (curOp->shouldDBProfile()) {
                    BSONObjBuilder execStatsBob;
                    Explain::getWinningPlanStats(exec.get(), &execStatsBob);
                    curOp->debug().execStats = execStatsBob.obj();
                }
                recordStatsForTopCommand(opCtx);

                appendCommandResponse(exec.get(), args.isRemove(), docFound, &result);
            } else {
                UpdateRequest request(nsString);
                UpdateLifecycleImpl updateLifecycle(nsString);
                const bool isExplain = false;
                makeUpdateRequest(opCtx, args, isExplain, &updateLifecycle, &request);

                if (opCtx->getTxnNumber()) {
                    request.setStmtId(stmtId);
                }

                ParsedUpdate parsedUpdate(opCtx, &request);
                uassertStatusOK(parsedUpdate.parseRequest());

                // These are boost::optional, because if the database or collection does not exist,
                // they will have to be reacquired in MODE_X
                boost::optional<AutoGetOrCreateDb> autoDb;
                boost::optional<AutoGetCollection> autoColl;

                autoColl.emplace(opCtx, nsString, MODE_IX);

                {
                    boost::optional<int> dbProfilingLevel;
                    if (autoColl->getDb())
                        dbProfilingLevel = autoColl->getDb()->getProfilingLevel();

                    stdx::lock_guard<Client> lk(*opCtx->getClient());
                    CurOp::get(opCtx)->enter_inlock(nsString.ns().c_str(), dbProfilingLevel);
                }

                assertCanWrite(opCtx, nsString);

                Collection* collection = autoColl->getCollection();

                // Create the collection if it does not exist when performing an upsert because the
                // update stage does not create its own collection
                if (!collection && args.isUpsert()) {
                    uassert(ErrorCodes::NamespaceNotFound,
                            str::stream() << "Cannot create namespace " << nsString.ns()
                                          << " in multi-document transaction.",
                            !inTransaction);

                    // Release the collection lock and reacquire a lock on the database in exclusive
                    // mode in order to create the collection
                    autoColl.reset();
                    autoDb.emplace(opCtx, dbName, MODE_X);

                    assertCanWrite(opCtx, nsString);

                    collection = autoDb->getDb()->getCollection(opCtx, nsString);

                    // If someone else beat us to creating the collection, do nothing
                    if (!collection) {
                        uassertStatusOK(userAllowedCreateNS(nsString.db(), nsString.coll()));
                        WriteUnitOfWork wuow(opCtx);
                        CollectionOptions collectionOptions;
                        uassertStatusOK(collectionOptions.parse(
                            BSONObj(), CollectionOptions::ParseKind::parseForCommand));
                        uassertStatusOK(Database::userCreateNS(
                            opCtx, autoDb->getDb(), nsString.ns(), collectionOptions));
                        wuow.commit();

                        collection = autoDb->getDb()->getCollection(opCtx, nsString);
                    }

                    invariant(collection);
                }

                const auto exec =
                    uassertStatusOK(getExecutorUpdate(opCtx, opDebug, collection, &parsedUpdate));

                {
                    stdx::lock_guard<Client> lk(*opCtx->getClient());
                    CurOp::get(opCtx)->setPlanSummary_inlock(Explain::getPlanSummary(exec.get()));
                }

                auto docFound = advanceExecutor(opCtx, exec.get(), args.isRemove());
                // Nothing after advancing the plan executor should throw a WriteConflictException,
                // so the following bookkeeping with execution stats won't end up being done
                // multiple times.

                PlanSummaryStats summaryStats;
                Explain::getSummaryStats(*exec, &summaryStats);
                if (collection) {
                    collection->infoCache()->notifyOfQuery(opCtx, summaryStats.indexesUsed);
                }
                UpdateStage::recordUpdateStatsInOpDebug(getUpdateStats(exec.get()), opDebug);
                opDebug->setPlanSummaryMetrics(summaryStats);

                if (curOp->shouldDBProfile()) {
                    BSONObjBuilder execStatsBob;
                    Explain::getWinningPlanStats(exec.get(), &execStatsBob);
                    curOp->debug().execStats = execStatsBob.obj();
                }
                recordStatsForTopCommand(opCtx);

                appendCommandResponse(exec.get(), args.isRemove(), docFound, &result);
            }

            return true;
        });
    }

} cmdFindAndModify;

}  // namespace
}  // namespace mongo
