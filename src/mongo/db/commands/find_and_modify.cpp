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

#include "mongo/db/commands/find_and_modify.h"

#include <boost/optional.hpp>
#include <memory>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/update.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/delete_request.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/ops/parsed_delete.h"
#include "mongo/db/ops/parsed_update.h"
#include "mongo/db/ops/update_lifecycle_impl.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/find_and_modify_request.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/s/collection_sharding_state.h"
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
 * If the operation failed, then an error Status is returned.
 */
StatusWith<boost::optional<BSONObj>> advanceExecutor(OperationContext* opCtx,
                                                     PlanExecutor* exec,
                                                     bool isRemove) {
    BSONObj value;
    PlanExecutor::ExecState state = exec->getNext(&value, nullptr);

    if (PlanExecutor::ADVANCED == state) {
        return boost::optional<BSONObj>(std::move(value));
    }

    if (PlanExecutor::FAILURE == state || PlanExecutor::DEAD == state) {
        error() << "Plan executor error during findAndModify: " << PlanExecutor::statestr(state)
                << ", stats: " << redact(Explain::getWinningPlanStats(exec));

        if (WorkingSetCommon::isValidStatusMemberObject(value)) {
            const Status errorStatus = WorkingSetCommon::getMemberObjectStatus(value);
            invariant(!errorStatus.isOK());
            return {errorStatus.code(), errorStatus.reason()};
        }
        const std::string opstr = isRemove ? "delete" : "update";
        return {ErrorCodes::OperationFailed,
                str::stream() << "executor returned " << PlanExecutor::statestr(state)
                              << " while executing "
                              << opstr};
    }

    invariant(state == PlanExecutor::IS_EOF);
    return boost::optional<BSONObj>(boost::none);
}

void makeUpdateRequest(const FindAndModifyRequest& args,
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
    requestOut->setYieldPolicy(PlanExecutor::YIELD_AUTO);
    requestOut->setExplain(explain);
    requestOut->setLifecycle(updateLifecycle);
}

void makeDeleteRequest(const FindAndModifyRequest& args, bool explain, DeleteRequest* requestOut) {
    requestOut->setQuery(args.getQuery());
    requestOut->setProj(args.getFields());
    requestOut->setSort(args.getSort());
    requestOut->setCollation(args.getCollation());
    requestOut->setMulti(false);
    requestOut->setYieldPolicy(PlanExecutor::YIELD_AUTO);
    requestOut->setReturnDeleted(true);  // Always return the old value.
    requestOut->setExplain(explain);
}

void appendCommandResponse(PlanExecutor* exec,
                           bool isRemove,
                           const boost::optional<BSONObj>& value,
                           BSONObjBuilder& result) {
    BSONObjBuilder lastErrorObjBuilder(result.subobjStart("lastErrorObject"));

    if (isRemove) {
        lastErrorObjBuilder.appendNumber("n", getDeleteStats(exec)->docsDeleted);
    } else {
        const UpdateStats* updateStats = getUpdateStats(exec);
        lastErrorObjBuilder.appendBool("updatedExisting", updateStats->nMatched > 0);
        lastErrorObjBuilder.appendNumber("n", updateStats->inserted ? 1 : updateStats->nMatched);
        // Note we have to use the objInserted from the stats here, rather than 'value'
        // because the _id field could have been excluded by a projection.
        if (!updateStats->objInserted.isEmpty()) {
            lastErrorObjBuilder.appendAs(updateStats->objInserted["_id"], kUpsertedFieldName);
        }
    }
    lastErrorObjBuilder.done();

    if (value) {
        result.append("value", *value);
    } else {
        result.appendNull("value");
    }
}

Status checkCanAcceptWritesForDatabase(OperationContext* opCtx, const NamespaceString& nsString) {
    if (!repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(opCtx, nsString)) {
        return Status(ErrorCodes::NotMaster,
                      str::stream()
                          << "Not primary while running findAndModify command on collection "
                          << nsString.ns());
    }
    return Status::OK();
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

}  // namespace

/* Find and Modify an object returning either the old (default) or new value*/
class CmdFindAndModify : public BasicCommand {
public:
    void help(std::stringstream& help) const override {
        help << "{ findAndModify: \"collection\", query: {processed:false}, update: {$set: "
                "{processed:true}}, new: true}\n"
                "{ findAndModify: \"collection\", query: {processed:false}, remove: true, sort: "
                "{priority:-1}}\n"
                "Either update or remove is required, all other fields have default values.\n"
                "Output is in the \"value\" field\n";
    }

    CmdFindAndModify() : BasicCommand("findAndModify", "findandmodify") {}

    bool slaveOk() const override {
        return false;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) override {
        find_and_modify::addPrivilegesRequiredForFindAndModify(this, dbname, cmdObj, out);
    }

    ReadWriteType getReadWriteType() const {
        return ReadWriteType::kWrite;
    }

    Status explain(OperationContext* opCtx,
                   const std::string& dbName,
                   const BSONObj& cmdObj,
                   ExplainOptions::Verbosity verbosity,
                   BSONObjBuilder* out) const override {
        const NamespaceString fullNs = parseNsCollectionRequired(dbName, cmdObj);
        Status allowedWriteStatus = userAllowedWriteNS(fullNs.ns());
        if (!allowedWriteStatus.isOK()) {
            return allowedWriteStatus;
        }

        StatusWith<FindAndModifyRequest> parseStatus =
            FindAndModifyRequest::parseFromBSON(NamespaceString(fullNs.ns()), cmdObj);
        if (!parseStatus.isOK()) {
            return parseStatus.getStatus();
        }

        const FindAndModifyRequest& args = parseStatus.getValue();
        const NamespaceString& nsString = args.getNamespaceString();
        OpDebug* opDebug = &CurOp::get(opCtx)->debug();

        if (args.isRemove()) {
            DeleteRequest request(nsString);
            const bool isExplain = true;
            makeDeleteRequest(args, isExplain, &request);

            ParsedDelete parsedDelete(opCtx, &request);
            Status parsedDeleteStatus = parsedDelete.parseRequest();
            if (!parsedDeleteStatus.isOK()) {
                return parsedDeleteStatus;
            }

            // Explain calls of the findAndModify command are read-only, but we take write
            // locks so that the timing information is more accurate.
            AutoGetCollection autoColl(opCtx, nsString, MODE_IX);
            if (!autoColl.getDb()) {
                return {ErrorCodes::NamespaceNotFound,
                        str::stream() << "database " << dbName << " does not exist."};
            }

            auto css = CollectionShardingState::get(opCtx, nsString);
            css->checkShardVersionOrThrow(opCtx);

            Collection* const collection = autoColl.getCollection();
            auto statusWithPlanExecutor =
                getExecutorDelete(opCtx, opDebug, collection, &parsedDelete);
            if (!statusWithPlanExecutor.isOK()) {
                return statusWithPlanExecutor.getStatus();
            }
            const auto exec = std::move(statusWithPlanExecutor.getValue());
            Explain::explainStages(exec.get(), collection, verbosity, out);
        } else {
            UpdateRequest request(nsString);
            UpdateLifecycleImpl updateLifecycle(nsString);
            const bool isExplain = true;
            makeUpdateRequest(args, isExplain, &updateLifecycle, &request);

            ParsedUpdate parsedUpdate(opCtx, &request);
            Status parsedUpdateStatus = parsedUpdate.parseRequest();
            if (!parsedUpdateStatus.isOK()) {
                return parsedUpdateStatus;
            }

            // Explain calls of the findAndModify command are read-only, but we take write
            // locks so that the timing information is more accurate.
            AutoGetCollection autoColl(opCtx, nsString, MODE_IX);
            if (!autoColl.getDb()) {
                return {ErrorCodes::NamespaceNotFound,
                        str::stream() << "database " << dbName << " does not exist."};
            }

            auto css = CollectionShardingState::get(opCtx, nsString);
            css->checkShardVersionOrThrow(opCtx);

            Collection* collection = autoColl.getCollection();
            auto statusWithPlanExecutor =
                getExecutorUpdate(opCtx, opDebug, collection, &parsedUpdate);
            if (!statusWithPlanExecutor.isOK()) {
                return statusWithPlanExecutor.getStatus();
            }
            const auto exec = std::move(statusWithPlanExecutor.getValue());
            Explain::explainStages(exec.get(), collection, verbosity, out);
        }

        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        // findAndModify command is not replicated directly.
        invariant(opCtx->writesAreReplicated());
        const NamespaceString fullNs = parseNsCollectionRequired(dbName, cmdObj);
        Status allowedWriteStatus = userAllowedWriteNS(fullNs.ns());
        if (!allowedWriteStatus.isOK()) {
            return appendCommandStatus(result, allowedWriteStatus);
        }

        StatusWith<FindAndModifyRequest> parseStatus =
            FindAndModifyRequest::parseFromBSON(NamespaceString(fullNs.ns()), cmdObj);
        if (!parseStatus.isOK()) {
            return appendCommandStatus(result, parseStatus.getStatus());
        }

        const FindAndModifyRequest& args = parseStatus.getValue();
        const NamespaceString& nsString = args.getNamespaceString();

        boost::optional<DisableDocumentValidation> maybeDisableValidation;
        if (shouldBypassDocumentValidationForCommand(cmdObj))
            maybeDisableValidation.emplace(opCtx);

        auto curOp = CurOp::get(opCtx);
        OpDebug* opDebug = &curOp->debug();

        // Although usually the PlanExecutor handles WCE internally, it will throw WCEs when it is
        // executing a findAndModify. This is done to ensure that we can always match, modify, and
        // return the document under concurrency, if a matching document exists.
        bool success = writeConflictRetry(opCtx, "findAndModify", nsString.ns(), [&] {
            if (args.isRemove()) {
                DeleteRequest request(nsString);
                const bool isExplain = false;
                makeDeleteRequest(args, isExplain, &request);

                ParsedDelete parsedDelete(opCtx, &request);
                Status parsedDeleteStatus = parsedDelete.parseRequest();
                if (!parsedDeleteStatus.isOK()) {
                    appendCommandStatus(result, parsedDeleteStatus);
                    return false;
                }

                AutoGetOrCreateDb autoDb(opCtx, dbName, MODE_IX);
                Lock::CollectionLock collLock(opCtx->lockState(), nsString.ns(), MODE_IX);

                // Attach the namespace and database profiling level to the current op.
                {
                    stdx::lock_guard<Client> lk(*opCtx->getClient());
                    CurOp::get(opCtx)->enter_inlock(nsString.ns().c_str(),
                                                    autoDb.getDb()->getProfilingLevel());
                }

                auto css = CollectionShardingState::get(opCtx, nsString);
                css->checkShardVersionOrThrow(opCtx);

                Status isPrimary = checkCanAcceptWritesForDatabase(opCtx, nsString);
                if (!isPrimary.isOK()) {
                    appendCommandStatus(result, isPrimary);
                    return false;
                }

                Collection* const collection = autoDb.getDb()->getCollection(opCtx, nsString);
                if (!collection && autoDb.getDb()->getViewCatalog()->lookup(opCtx, nsString.ns())) {
                    appendCommandStatus(result,
                                        {ErrorCodes::CommandNotSupportedOnView,
                                         "findAndModify not supported on a view"});
                    return false;
                }
                auto statusWithPlanExecutor =
                    getExecutorDelete(opCtx, opDebug, collection, &parsedDelete);
                if (!statusWithPlanExecutor.isOK()) {
                    appendCommandStatus(result, statusWithPlanExecutor.getStatus());
                    return false;
                }
                const auto exec = std::move(statusWithPlanExecutor.getValue());

                {
                    stdx::lock_guard<Client> lk(*opCtx->getClient());
                    CurOp::get(opCtx)->setPlanSummary_inlock(Explain::getPlanSummary(exec.get()));
                }

                StatusWith<boost::optional<BSONObj>> advanceStatus =
                    advanceExecutor(opCtx, exec.get(), args.isRemove());
                if (!advanceStatus.isOK()) {
                    appendCommandStatus(result, advanceStatus.getStatus());
                    return false;
                }
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

                boost::optional<BSONObj> value = advanceStatus.getValue();
                appendCommandResponse(exec.get(), args.isRemove(), value, result);
            } else {
                UpdateRequest request(nsString);
                UpdateLifecycleImpl updateLifecycle(nsString);
                const bool isExplain = false;
                makeUpdateRequest(args, isExplain, &updateLifecycle, &request);

                ParsedUpdate parsedUpdate(opCtx, &request);
                Status parsedUpdateStatus = parsedUpdate.parseRequest();
                if (!parsedUpdateStatus.isOK()) {
                    appendCommandStatus(result, parsedUpdateStatus);
                    return false;
                }

                AutoGetOrCreateDb autoDb(opCtx, dbName, MODE_IX);
                Lock::CollectionLock collLock(opCtx->lockState(), nsString.ns(), MODE_IX);

                // Attach the namespace and database profiling level to the current op.
                {
                    stdx::lock_guard<Client> lk(*opCtx->getClient());
                    CurOp::get(opCtx)->enter_inlock(nsString.ns().c_str(),
                                                    autoDb.getDb()->getProfilingLevel());
                }

                auto css = CollectionShardingState::get(opCtx, nsString);
                css->checkShardVersionOrThrow(opCtx);

                Status isPrimary = checkCanAcceptWritesForDatabase(opCtx, nsString);
                if (!isPrimary.isOK()) {
                    appendCommandStatus(result, isPrimary);
                    return false;
                }

                Collection* collection = autoDb.getDb()->getCollection(opCtx, nsString.ns());
                if (!collection && autoDb.getDb()->getViewCatalog()->lookup(opCtx, nsString.ns())) {
                    appendCommandStatus(result,
                                        {ErrorCodes::CommandNotSupportedOnView,
                                         "findAndModify not supported on a view"});
                    return false;
                }

                // Create the collection if it does not exist when performing an upsert
                // because the update stage does not create its own collection.
                if (!collection && args.isUpsert()) {
                    // Release the collection lock and reacquire a lock on the database
                    // in exclusive mode in order to create the collection.
                    collLock.relockAsDatabaseExclusive(autoDb.lock());
                    collection = autoDb.getDb()->getCollection(opCtx, nsString);
                    Status isPrimaryAfterRelock = checkCanAcceptWritesForDatabase(opCtx, nsString);
                    if (!isPrimaryAfterRelock.isOK()) {
                        appendCommandStatus(result, isPrimaryAfterRelock);
                        return false;
                    }

                    if (collection) {
                        // Someone else beat us to creating the collection, do nothing.
                    } else {
                        WriteUnitOfWork wuow(opCtx);
                        Status createCollStatus =
                            userCreateNS(opCtx, autoDb.getDb(), nsString.ns(), BSONObj());
                        if (!createCollStatus.isOK()) {
                            appendCommandStatus(result, createCollStatus);
                            return false;
                        }
                        wuow.commit();

                        collection = autoDb.getDb()->getCollection(opCtx, nsString);
                        invariant(collection);
                    }
                }

                auto statusWithPlanExecutor =
                    getExecutorUpdate(opCtx, opDebug, collection, &parsedUpdate);
                if (!statusWithPlanExecutor.isOK()) {
                    appendCommandStatus(result, statusWithPlanExecutor.getStatus());
                    return false;
                }
                const auto exec = std::move(statusWithPlanExecutor.getValue());

                {
                    stdx::lock_guard<Client> lk(*opCtx->getClient());
                    CurOp::get(opCtx)->setPlanSummary_inlock(Explain::getPlanSummary(exec.get()));
                }

                StatusWith<boost::optional<BSONObj>> advanceStatus =
                    advanceExecutor(opCtx, exec.get(), args.isRemove());
                if (!advanceStatus.isOK()) {
                    appendCommandStatus(result, advanceStatus.getStatus());
                    return false;
                }
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

                boost::optional<BSONObj> value = advanceStatus.getValue();
                appendCommandResponse(exec.get(), args.isRemove(), value, result);
            }
            return true;
        });

        if (!success) {
            return false;
        }

        return true;
    }

} cmdFindAndModify;

}  // namespace mongo
