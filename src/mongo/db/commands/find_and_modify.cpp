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

#include <memory>
#include <boost/optional.hpp>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/delete_request.h"
#include "mongo/db/ops/parsed_delete.h"
#include "mongo/db/ops/parsed_update.h"
#include "mongo/db/ops/update_lifecycle_impl.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/write_concern.h"
#include "mongo/s/d_state.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {

    /**
     * Represents the user-supplied options to the findAndModify command.
     *
     * The BSONObj members contained within this struct are owned objects.
     */
    struct FindAndModifyRequest {
    public:
        /**
         * Construct an empty request.
         */
        FindAndModifyRequest() { }

        const NamespaceString& getNamespaceString() const { return _nsString; }
        const BSONObj& getQuery() const { return _query; }
        const BSONObj& getFields() const { return _fields; }
        const BSONObj& getUpdateObj() const { return _updateObj; }
        const BSONObj& getSort() const { return _sort; }
        bool shouldReturnNew() const { return _shouldReturnNew; }
        bool isUpsert() const { return _isUpsert; }
        bool isRemove() const { return _isRemove; }

        /**
         * Construct a FindAndModifyRequest from the database name and the command specification.
         */
        static StatusWith<FindAndModifyRequest> parseFromBSON(const std::string& fullNs,
                                                              const BSONObj& cmdObj) {
            BSONObj query = cmdObj.getObjectField("query");
            BSONObj fields = cmdObj.getObjectField("fields");
            BSONObj updateObj = cmdObj.getObjectField("update");
            BSONObj sort = cmdObj.getObjectField("sort");
            bool shouldReturnNew = cmdObj["new"].trueValue();
            bool isUpsert = cmdObj["upsert"].trueValue();
            bool isRemove = cmdObj["remove"].trueValue();
            bool isUpdate = cmdObj.hasField("update");

            if (!isRemove && !isUpdate) {
                return {ErrorCodes::BadValue, "Either an update or remove=true must be specified"};
            }
            if (isRemove) {
                if (isUpdate) {
                    return {ErrorCodes::BadValue, "Cannot specify both an update and remove=true"};
                }
                if (isUpsert) {
                    return {ErrorCodes::BadValue,
                        "Cannot specify both upsert=true and remove=true"};
                }
                if (shouldReturnNew) {
                    return {ErrorCodes::BadValue,
                        "Cannot specify both new=true and remove=true;"
                        " 'remove' always returns the deleted document"};
                }
            }
            FindAndModifyRequest request(fullNs, query, fields, updateObj, sort, shouldReturnNew,
                                         isUpsert, isRemove);
            return std::move(request);
        }

    private:
        /**
         * Construct a FindAndModifyRequest from parsed BSON.
         */
        FindAndModifyRequest(const std::string& fullNs,
                             const BSONObj& query,
                             const BSONObj& fields,
                             const BSONObj& updateObj,
                             const BSONObj& sort,
                             bool shouldReturnNew,
                             bool isUpsert,
                             bool isRemove)
            : _nsString(fullNs),
              _query(query.getOwned()),
              _fields(fields.getOwned()),
              _updateObj(updateObj.getOwned()),
              _sort(sort.getOwned()),
              _shouldReturnNew(shouldReturnNew),
              _isUpsert(isUpsert),
              _isRemove(isRemove) { }

        NamespaceString _nsString;
        BSONObj _query;
        BSONObj _fields;
        BSONObj _updateObj;
        BSONObj _sort;
        bool _shouldReturnNew;
        bool _isUpsert;
        bool _isRemove;
    };

    const UpdateStats* getUpdateStats(const PlanStageStats* stats) {
        // The stats may refer to an update stage, or a projection stage wrapping an update stage.
        if (StageType::STAGE_PROJECTION == stats->stageType) {
            invariant(stats->children.size() == 1);
            stats = stats->children[0];
        }

        invariant(StageType::STAGE_UPDATE == stats->stageType);
        return static_cast<UpdateStats*>(stats->specific.get());
    }

    const DeleteStats* getDeleteStats(const PlanStageStats* stats) {
        // The stats may refer to a delete stage, or a projection stage wrapping a delete stage.
        if (StageType::STAGE_PROJECTION == stats->stageType) {
            invariant(stats->children.size() == 1);
            stats = stats->children[0];
        }

        invariant(StageType::STAGE_DELETE == stats->stageType);
        return static_cast<DeleteStats*>(stats->specific.get());
    }

    /**
     * If the operation succeeded, then Status::OK() is returned, possibly with a document value
     * to return to the client. If no matching document to update or remove was found, then none
     * is returned. Otherwise, the updated or deleted document is returned.
     *
     * If the operation failed, then an error Status is returned.
     */
    StatusWith<boost::optional<BSONObj>> advanceExecutor(PlanExecutor* exec, bool isRemove) {
        BSONObj value;
        PlanExecutor::ExecState state = exec->getNext(&value, nullptr);
        if (PlanExecutor::ADVANCED == state) {
            return boost::optional<BSONObj>(std::move(value));
        }
        if (PlanExecutor::FAILURE == state || PlanExecutor::DEAD == state) {
            if (PlanExecutor::FAILURE == state &&
                WorkingSetCommon::isValidStatusMemberObject(value)) {
                const Status errorStatus =
                    WorkingSetCommon::getMemberObjectStatus(value);
                invariant(!errorStatus.isOK());
                return {errorStatus.code(), errorStatus.reason()};
            }
            const std::string opstr = isRemove ? "delete" : "update";
            return {ErrorCodes::OperationFailed, str::stream()
                << "executor returned " << PlanExecutor::statestr(state)
                << " while executing " << opstr};
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
        requestOut->setUpsert(args.isUpsert());
        requestOut->setReturnDocs(args.shouldReturnNew()
                                  ? UpdateRequest::RETURN_NEW
                                  : UpdateRequest::RETURN_OLD);
        requestOut->setMulti(false);
        requestOut->setYieldPolicy(PlanExecutor::YIELD_AUTO);
        requestOut->setExplain(explain);
        requestOut->setLifecycle(updateLifecycle);
    }

    void makeDeleteRequest(const FindAndModifyRequest& args,
                           bool explain,
                           DeleteRequest* requestOut) {
        requestOut->setQuery(args.getQuery());
        requestOut->setProj(args.getFields());
        requestOut->setSort(args.getSort());
        requestOut->setMulti(false);
        requestOut->setYieldPolicy(PlanExecutor::YIELD_AUTO);
        requestOut->setReturnDeleted(true);  // Always return the old value.
        requestOut->setExplain(explain);
    }

    void appendCommandResponse(PlanExecutor* exec,
                               bool isRemove,
                               const boost::optional<BSONObj>& value,
                               BSONObjBuilder& result) {
        const std::unique_ptr<PlanStageStats> stats(exec->getStats());
        BSONObjBuilder lastErrorObjBuilder(result.subobjStart("lastErrorObject"));

        if (isRemove) {
            lastErrorObjBuilder.appendNumber("n", getDeleteStats(stats.get())->docsDeleted);
        }
        else {
            const UpdateStats* updateStats = getUpdateStats(stats.get());
            lastErrorObjBuilder.appendBool("updatedExisting", updateStats->nMatched > 0);
            lastErrorObjBuilder.appendNumber("n", updateStats->inserted ? 1
                                                                        : updateStats->nMatched);
            // Note we have to use the objInserted from the stats here, rather than 'value'
            // because the _id field could have been excluded by a projection.
            if (!updateStats->objInserted.isEmpty()) {
                lastErrorObjBuilder.appendAs(updateStats->objInserted["_id"], kUpsertedFieldName);
            }
        }
        lastErrorObjBuilder.done();

        if (value) {
            result.append("value", *value);
        }
        else {
            result.appendNull("value");
        }
    }

    Status checkCanAcceptWritesForDatabase(const NamespaceString& nsString) {
        if (!repl::getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(nsString.db())) {
            return Status(ErrorCodes::NotMaster, str::stream()
                << "Not primary while running findAndModify command on collection "
                << nsString.ns());
        }
        return Status::OK();
    }

}  // namespace

    /* Find and Modify an object returning either the old (default) or new value*/
    class CmdFindAndModify : public Command {
    public:
        void help(std::stringstream& help) const override {
            help <<
                 "{ findAndModify: \"collection\", query: {processed:false}, update: {$set: {processed:true}}, new: true}\n"
                 "{ findAndModify: \"collection\", query: {processed:false}, remove: true, sort: {priority:-1}}\n"
                 "Either update or remove is required, all other fields have default values.\n"
                 "Output is in the \"value\" field\n";
        }

        CmdFindAndModify() : Command("findAndModify", false, "findandmodify") { }
        bool slaveOk() const override { return false; }
        bool isWriteCommandForConfigServer() const override { return true; }
        void addRequiredPrivileges(const std::string& dbname,
                                   const BSONObj& cmdObj,
                                   std::vector<Privilege>* out) override {
            find_and_modify::addPrivilegesRequiredForFindAndModify(this, dbname, cmdObj, out);
        }

        Status explain(OperationContext* txn,
                       const std::string& dbName,
                       const BSONObj& cmdObj,
                       ExplainCommon::Verbosity verbosity,
                       BSONObjBuilder* out) const override {
            const std::string fullNs = parseNsCollectionRequired(dbName, cmdObj);

            StatusWith<FindAndModifyRequest> parseStatus =
                FindAndModifyRequest::parseFromBSON(fullNs, cmdObj);
            if (!parseStatus.isOK()) {
                return parseStatus.getStatus();
            }

            const FindAndModifyRequest& args = parseStatus.getValue();
            const NamespaceString& nsString = args.getNamespaceString();

            if (args.isRemove()) {
                DeleteRequest request(nsString);
                const bool isExplain = true;
                makeDeleteRequest(args, isExplain, &request);

                ParsedDelete parsedDelete(txn, &request);
                Status parsedDeleteStatus = parsedDelete.parseRequest();
                if (!parsedDeleteStatus.isOK()) {
                    return parsedDeleteStatus;
                }

                // Explain calls of the findAndModify command are read-only, but we take write
                // locks so that the timing information is more accurate.
                AutoGetDb autoDb(txn, dbName, MODE_IX);
                Lock::CollectionLock collLock(txn->lockState(), nsString.ns(), MODE_IX);

                ensureShardVersionOKOrThrow(nsString.ns());

                Collection* collection = nullptr;
                if (autoDb.getDb()) {
                    collection = autoDb.getDb()->getCollection(nsString.ns());
                }
                else {
                    return {ErrorCodes::DatabaseNotFound,
                            str::stream() << "database " << dbName << " does not exist."};
                }

                PlanExecutor* rawExec;
                Status execStatus = getExecutorDelete(txn, collection, &parsedDelete, &rawExec);
                if (!execStatus.isOK()) {
                    return execStatus;
                }
                const std::unique_ptr<PlanExecutor> exec(rawExec);
                Explain::explainStages(exec.get(), verbosity, out);
            }
            else {
                UpdateRequest request(nsString);
                const bool ignoreVersion = false;
                UpdateLifecycleImpl updateLifecycle(ignoreVersion, nsString);
                const bool isExplain = true;
                makeUpdateRequest(args, isExplain, &updateLifecycle, &request);

                ParsedUpdate parsedUpdate(txn, &request);
                Status parsedUpdateStatus = parsedUpdate.parseRequest();
                if (!parsedUpdateStatus.isOK()) {
                    return parsedUpdateStatus;
                }

                OpDebug* opDebug = &txn->getCurOp()->debug();

                // Explain calls of the findAndModify command are read-only, but we take write
                // locks so that the timing information is more accurate.
                AutoGetDb autoDb(txn, dbName, MODE_IX);
                Lock::CollectionLock collLock(txn->lockState(), nsString.ns(), MODE_IX);

                ensureShardVersionOKOrThrow(nsString.ns());

                Collection* collection = nullptr;
                if (autoDb.getDb()) {
                    collection = autoDb.getDb()->getCollection(nsString.ns());
                }
                else {
                    return {ErrorCodes::DatabaseNotFound,
                            str::stream() << "database " << dbName << " does not exist."};
                }

                PlanExecutor* rawExec;
                Status execStatus = getExecutorUpdate(txn, collection, &parsedUpdate, opDebug,
                                                      &rawExec);
                if (!execStatus.isOK()) {
                    return execStatus;
                }
                const std::unique_ptr<PlanExecutor> exec(rawExec);
                Explain::explainStages(exec.get(), verbosity, out);
            }

            return Status::OK();
        }

        bool run(OperationContext* txn,
                 const std::string& dbName,
                 BSONObj& cmdObj,
                 int options,
                 std::string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) override {
            invariant(!fromRepl);  // findAndModify command is not replicated directly.
            const std::string fullNs = parseNsCollectionRequired(dbName, cmdObj);

            StatusWith<FindAndModifyRequest> parseStatus =
                FindAndModifyRequest::parseFromBSON(fullNs, cmdObj);
            if (!parseStatus.isOK()) {
                return appendCommandStatus(result, parseStatus.getStatus());
            }

            const FindAndModifyRequest& args = parseStatus.getValue();
            const NamespaceString& nsString = args.getNamespaceString();

            StatusWith<WriteConcernOptions> wcResult = extractWriteConcern(cmdObj);
            if (!wcResult.isOK()) {
                return appendCommandStatus(result, wcResult.getStatus());
            }
            txn->setWriteConcern(wcResult.getValue());
            setupSynchronousCommit(txn);

            // We may encounter a WriteConflictException when creating a collection during an
            // upsert, even when holding the exclusive lock on the database (due to other load on
            // the system). The query framework should handle all other WriteConflictExceptions,
            // but we defensively wrap the operation in the retry loop anyway.
            //
            // SERVER-17579 getExecutorUpdate() and getExecutorDelete() can throw a
            // WriteConflictException when checking whether an index is ready or not.
            // (on debug builds only)
            MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                if (args.isRemove()) {
                    DeleteRequest request(nsString);
                    const bool isExplain = false;
                    makeDeleteRequest(args, isExplain, &request);

                    ParsedDelete parsedDelete(txn, &request);
                    Status parsedDeleteStatus = parsedDelete.parseRequest();
                    if (!parsedDeleteStatus.isOK()) {
                        return appendCommandStatus(result, parsedDeleteStatus);
                    }

                    AutoGetOrCreateDb autoDb(txn, dbName, MODE_IX);
                    Lock::CollectionLock collLock(txn->lockState(), nsString.ns(), MODE_IX);
                    Collection* collection = autoDb.getDb()->getCollection(nsString.ns());

                    ensureShardVersionOKOrThrow(nsString.ns());

                    Status isPrimary = checkCanAcceptWritesForDatabase(nsString);
                    if (!isPrimary.isOK()) {
                        return appendCommandStatus(result, isPrimary);
                    }

                    PlanExecutor* rawExec;
                    Status execStatus = getExecutorDelete(txn, collection, &parsedDelete, &rawExec);
                    if (!execStatus.isOK()) {
                        return appendCommandStatus(result, execStatus);
                    }
                    const std::unique_ptr<PlanExecutor> exec(rawExec);

                    StatusWith<boost::optional<BSONObj>> advanceStatus =
                        advanceExecutor(exec.get(), args.isRemove());
                    if (!advanceStatus.isOK()) {
                        return appendCommandStatus(result, advanceStatus.getStatus());
                    }

                    boost::optional<BSONObj> value = advanceStatus.getValue();
                    appendCommandResponse(exec.get(), args.isRemove(), value, result);
                }
                else {
                    UpdateRequest request(nsString);
                    const bool ignoreVersion = false;
                    UpdateLifecycleImpl updateLifecycle(ignoreVersion, nsString);
                    const bool isExplain = false;
                    makeUpdateRequest(args, isExplain, &updateLifecycle, &request);

                    ParsedUpdate parsedUpdate(txn, &request);
                    Status parsedUpdateStatus = parsedUpdate.parseRequest();
                    if (!parsedUpdateStatus.isOK()) {
                        return appendCommandStatus(result, parsedUpdateStatus);
                    }

                    OpDebug* opDebug = &txn->getCurOp()->debug();

                    AutoGetOrCreateDb autoDb(txn, dbName, MODE_IX);
                    Lock::CollectionLock collLock(txn->lockState(), nsString.ns(), MODE_IX);
                    Collection* collection = autoDb.getDb()->getCollection(nsString.ns());

                    ensureShardVersionOKOrThrow(nsString.ns());

                    Status isPrimary = checkCanAcceptWritesForDatabase(nsString);
                    if (!isPrimary.isOK()) {
                        return appendCommandStatus(result, isPrimary);
                    }

                    // Create the collection if it does not exist when performing an upsert
                    // because the update stage does not create its own collection.
                    if (!collection && args.isUpsert()) {
                        // Release the collection lock and reacquire a lock on the database
                        // in exclusive mode in order to create the collection.
                        collLock.relockWithMode(MODE_X, autoDb.lock());
                        collection = autoDb.getDb()->getCollection(nsString.ns());
                        Status isPrimaryAfterRelock = checkCanAcceptWritesForDatabase(nsString);
                        if (!isPrimaryAfterRelock.isOK()) {
                            return appendCommandStatus(result, isPrimaryAfterRelock);
                        }

                        if (collection) {
                            // Someone else beat us to creating the collection, do nothing.
                        }
                        else {
                            WriteUnitOfWork wuow(txn);
                            Status createCollStatus = userCreateNS(txn, autoDb.getDb(),
                                                                   nsString.ns(), BSONObj());
                            if (!createCollStatus.isOK()) {
                                return appendCommandStatus(result, createCollStatus);
                            }
                            wuow.commit();

                            collection = autoDb.getDb()->getCollection(nsString.ns());
                            invariant(collection);
                        }
                    }

                    PlanExecutor* rawExec;
                    Status execStatus = getExecutorUpdate(txn, collection, &parsedUpdate, opDebug,
                                                          &rawExec);
                    if (!execStatus.isOK()) {
                        return appendCommandStatus(result, execStatus);
                    }
                    const std::unique_ptr<PlanExecutor> exec(rawExec);

                    StatusWith<boost::optional<BSONObj>> advanceStatus =
                        advanceExecutor(exec.get(), args.isRemove());
                    if (!advanceStatus.isOK()) {
                        return appendCommandStatus(result, advanceStatus.getStatus());
                    }

                    boost::optional<BSONObj> value = advanceStatus.getValue();
                    appendCommandResponse(exec.get(), args.isRemove(), value, result);
                }
            } MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "findAndModify", nsString.ns());

            WriteConcernResult res;
            wcResult = waitForWriteConcern(
                    txn,
                    repl::ReplClientInfo::forClient(txn->getClient()).getLastOp(),
                    &res);
            appendCommandWCStatus(result, wcResult.getStatus());

            return true;
        }

    } cmdFindAndModify;

}  // namespace mongo
