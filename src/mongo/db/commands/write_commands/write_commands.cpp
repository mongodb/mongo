/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/base/init.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/write_commands/write_commands_common.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/json.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/ops/delete_request.h"
#include "mongo/db/ops/parsed_delete.h"
#include "mongo/db/ops/parsed_update.h"
#include "mongo/db/ops/update_lifecycle_impl.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/write_concern.h"
#include "mongo/s/stale_exception.h"

namespace mongo {

using std::string;
using std::stringstream;

namespace {

void redactTooLongLog(mutablebson::Document* cmdObj, StringData fieldName) {
    namespace mmb = mutablebson;
    mmb::Element root = cmdObj->root();
    mmb::Element field = root.findFirstChildNamed(fieldName);

    // If the cmdObj is too large, it will be a "too big" message given by CachedBSONObj.get()
    if (!field.ok()) {
        return;
    }

    // Redact the log if there are more than one documents or operations.
    if (field.countChildren() > 1) {
        field.setValueInt(field.countChildren());
    }
}

Status checkAuthForWriteCommand(ClientBasic* client,
                                BatchedCommandRequest::BatchType batchType,
                                NamespaceString ns,
                                const BSONObj& cmdObj) {
    Status status =
        auth::checkAuthForWriteCommand(AuthorizationSession::get(client), batchType, ns, cmdObj);
    if (!status.isOK()) {
        LastError::get(client).setLastError(status.code(), status.reason());
    }
    return status;
}

bool shouldSkipOutput(OperationContext* txn) {
    const WriteConcernOptions& writeConcern = txn->getWriteConcern();
    return writeConcern.wMode.empty() && writeConcern.wNumNodes == 0 &&
        (writeConcern.syncMode == WriteConcernOptions::SyncMode::NONE ||
         writeConcern.syncMode == WriteConcernOptions::SyncMode::UNSET);
}

enum class ReplyStyle { kUpdate, kNotUpdate };  // update has extra fields.
void serializeReply(OperationContext* txn,
                    ReplyStyle replyStyle,
                    bool continueOnError,
                    size_t opsInBatch,
                    const WriteResult& result,
                    BSONObjBuilder* out) {
    if (shouldSkipOutput(txn))
        return;

    long long n = 0;
    long long nModified = 0;
    std::vector<BSONObj> upsertInfo;
    std::vector<BSONObj> errors;
    BSONSizeTracker upsertInfoSizeTracker;
    BSONSizeTracker errorsSizeTracker;

    for (size_t i = 0; i < result.results.size(); i++) {
        if (result.results[i].isOK()) {
            const auto& opResult = result.results[i].getValue();
            n += opResult.n;  // Always there.
            if (replyStyle == ReplyStyle::kUpdate) {
                nModified += opResult.nModified;
                if (!opResult.upsertedId.isEmpty()) {
                    BSONObjBuilder upsertedId(upsertInfoSizeTracker);
                    upsertedId.append("index", int(i));
                    upsertedId.appendAs(opResult.upsertedId.firstElement(), "_id");
                    upsertInfo.push_back(upsertedId.obj());
                }
            }
            continue;
        }

        const auto& status = result.results[i].getStatus();
        BSONObjBuilder error(errorsSizeTracker);
        error.append("index", int(i));
        error.append("code", int(status.code()));
        error.append("errmsg", status.reason());
        errors.push_back(error.obj());
    }

    if (result.staleConfigException) {
        // For ordered:false commands we need to duplicate the StaleConfig result for all ops
        // after we stopped. result.results doesn't include the staleConfigException.
        // See the comment on WriteResult::staleConfigException for more info.
        int endIndex = continueOnError ? opsInBatch : result.results.size() + 1;
        for (int i = result.results.size(); i < endIndex; i++) {
            BSONObjBuilder error(errorsSizeTracker);
            error.append("index", i);
            error.append("code", int(ErrorCodes::StaleShardVersion));  // Different from exception!
            error.append("errmsg", result.staleConfigException->getInfo().msg);
            {
                BSONObjBuilder errInfo(error.subobjStart("errInfo"));
                result.staleConfigException->getVersionWanted().addToBSON(errInfo, "vWanted");
            }
            errors.push_back(error.obj());
        }
    }

    out->appendNumber("n", n);

    if (replyStyle == ReplyStyle::kUpdate) {
        out->appendNumber("nModified", nModified);
        if (!upsertInfo.empty()) {
            out->append("upserted", upsertInfo);
        }
    }

    if (!errors.empty()) {
        out->append("writeErrors", errors);
    }

    // writeConcernError field is handled by command processor.

    {
        // Undocumented repl fields that mongos depends on.
        auto* replCoord = repl::ReplicationCoordinator::get(txn->getServiceContext());
        const auto replMode = replCoord->getReplicationMode();
        if (replMode != repl::ReplicationCoordinator::modeNone) {
            const auto lastOp = repl::ReplClientInfo::forClient(txn->getClient()).getLastOp();
            if (lastOp.getTerm() == repl::OpTime::kUninitializedTerm) {
                out->append("opTime", lastOp.getTimestamp());
            } else {
                lastOp.append(out, "opTime");
            }

            if (replMode == repl::ReplicationCoordinator::modeReplSet) {
                out->append("electionId", replCoord->getElectionId());
            }
        }
    }
}

class WriteCommand : public Command {
public:
    explicit WriteCommand(StringData name) : Command(name) {}

    bool slaveOk() const final {
        return false;
    }

    bool shouldAffectCommandCounter() const final {
        return false;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const {
        return true;
    }

    ReadWriteType getReadWriteType() const {
        return ReadWriteType::kWrite;
    }

    bool run(OperationContext* txn,
             const std::string& dbname,
             BSONObj& cmdObj,
             int options,
             std::string& errmsg,
             BSONObjBuilder& result) final {
        try {
            runImpl(txn, dbname, cmdObj, result);
            return true;
        } catch (const DBException& ex) {
            LastError::get(txn->getClient()).setLastError(ex.getCode(), ex.getInfo().msg);
            throw;
        }
    }

    virtual void runImpl(OperationContext* txn,
                         const std::string& dbname,
                         const BSONObj& cmdObj,
                         BSONObjBuilder& result) = 0;
};

}  // namespace

class CmdInsert final : public WriteCommand {
public:
    CmdInsert() : WriteCommand("insert") {}

    void redactForLogging(mutablebson::Document* cmdObj) final {
        redactTooLongLog(cmdObj, "documents");
    }

    void help(stringstream& help) const final {
        help << "insert documents";
    }

    Status checkAuthForCommand(ClientBasic* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) final {
        return checkAuthForWriteCommand(client,
                                        BatchedCommandRequest::BatchType_Insert,
                                        NamespaceString(parseNs(dbname, cmdObj)),
                                        cmdObj);
    }

    void runImpl(OperationContext* txn,
                 const std::string& dbname,
                 const BSONObj& cmdObj,
                 BSONObjBuilder& result) final {
        const auto batch = parseInsertCommand(dbname, cmdObj);
        const auto reply = performInserts(txn, batch);
        serializeReply(txn,
                       ReplyStyle::kNotUpdate,
                       batch.continueOnError,
                       batch.documents.size(),
                       reply,
                       &result);
    }
} cmdInsert;

class CmdUpdate final : public WriteCommand {
public:
    CmdUpdate() : WriteCommand("update") {}

    void redactForLogging(mutablebson::Document* cmdObj) final {
        redactTooLongLog(cmdObj, "updates");
    }

    void help(stringstream& help) const final {
        help << "update documents";
    }

    Status checkAuthForCommand(ClientBasic* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) final {
        return checkAuthForWriteCommand(client,
                                        BatchedCommandRequest::BatchType_Update,
                                        NamespaceString(parseNs(dbname, cmdObj)),
                                        cmdObj);
    }

    void runImpl(OperationContext* txn,
                 const std::string& dbname,
                 const BSONObj& cmdObj,
                 BSONObjBuilder& result) final {
        const auto batch = parseUpdateCommand(dbname, cmdObj);
        const auto reply = performUpdates(txn, batch);
        serializeReply(
            txn, ReplyStyle::kUpdate, batch.continueOnError, batch.updates.size(), reply, &result);
    }

    Status explain(OperationContext* txn,
                   const std::string& dbname,
                   const BSONObj& cmdObj,
                   ExplainCommon::Verbosity verbosity,
                   const rpc::ServerSelectionMetadata&,
                   BSONObjBuilder* out) const final {
        const auto batch = parseUpdateCommand(dbname, cmdObj);
        uassert(ErrorCodes::InvalidLength,
                "explained write batches must be of size 1",
                batch.updates.size() == 1);

        UpdateLifecycleImpl updateLifecycle(batch.ns);
        UpdateRequest updateRequest(batch.ns);
        updateRequest.setLifecycle(&updateLifecycle);
        updateRequest.setQuery(batch.updates[0].query);
        updateRequest.setCollation(batch.updates[0].collation);
        updateRequest.setUpdates(batch.updates[0].update);
        updateRequest.setMulti(batch.updates[0].multi);
        updateRequest.setUpsert(batch.updates[0].upsert);
        updateRequest.setYieldPolicy(PlanExecutor::YIELD_AUTO);
        updateRequest.setExplain();

        ParsedUpdate parsedUpdate(txn, &updateRequest);
        uassertStatusOK(parsedUpdate.parseRequest());

        // Explains of write commands are read-only, but we take write locks so that timing
        // info is more accurate.
        ScopedTransaction scopedXact(txn, MODE_IX);
        AutoGetCollection collection(txn, batch.ns, MODE_IX);

        auto exec = uassertStatusOK(getExecutorUpdate(
            txn, &CurOp::get(txn)->debug(), collection.getCollection(), &parsedUpdate));
        Explain::explainStages(exec.get(), collection.getCollection(), verbosity, out);
        return Status::OK();
    }
} cmdUpdate;

class CmdDelete final : public WriteCommand {
public:
    CmdDelete() : WriteCommand("delete") {}

    void redactForLogging(mutablebson::Document* cmdObj) final {
        redactTooLongLog(cmdObj, "deletes");
    }

    void help(stringstream& help) const final {
        help << "delete documents";
    }

    Status checkAuthForCommand(ClientBasic* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) final {
        return checkAuthForWriteCommand(client,
                                        BatchedCommandRequest::BatchType_Delete,
                                        NamespaceString(parseNs(dbname, cmdObj)),
                                        cmdObj);
    }

    void runImpl(OperationContext* txn,
                 const std::string& dbname,
                 const BSONObj& cmdObj,
                 BSONObjBuilder& result) final {
        const auto batch = parseDeleteCommand(dbname, cmdObj);
        const auto reply = performDeletes(txn, batch);
        serializeReply(txn,
                       ReplyStyle::kNotUpdate,
                       batch.continueOnError,
                       batch.deletes.size(),
                       reply,
                       &result);
    }

    Status explain(OperationContext* txn,
                   const std::string& dbname,
                   const BSONObj& cmdObj,
                   ExplainCommon::Verbosity verbosity,
                   const rpc::ServerSelectionMetadata&,
                   BSONObjBuilder* out) const final {
        const auto batch = parseDeleteCommand(dbname, cmdObj);
        uassert(ErrorCodes::InvalidLength,
                "explained write batches must be of size 1",
                batch.deletes.size() == 1);

        DeleteRequest deleteRequest(batch.ns);
        deleteRequest.setQuery(batch.deletes[0].query);
        deleteRequest.setCollation(batch.deletes[0].collation);
        deleteRequest.setMulti(batch.deletes[0].multi);
        deleteRequest.setYieldPolicy(PlanExecutor::YIELD_AUTO);
        deleteRequest.setExplain();

        ParsedDelete parsedDelete(txn, &deleteRequest);
        uassertStatusOK(parsedDelete.parseRequest());

        // Explains of write commands are read-only, but we take write locks so that timing
        // info is more accurate.
        ScopedTransaction scopedXact(txn, MODE_IX);
        AutoGetCollection collection(txn, batch.ns, MODE_IX);

        // Explain the plan tree.
        auto exec = uassertStatusOK(getExecutorDelete(
            txn, &CurOp::get(txn)->debug(), collection.getCollection(), &parsedDelete));
        Explain::explainStages(exec.get(), collection.getCollection(), verbosity, out);
        return Status::OK();
    }
} cmdDelete;

}  // namespace mongo
