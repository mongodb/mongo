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
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/session_catalog.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/write_concern.h"
#include "mongo/s/stale_exception.h"

namespace mongo {
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
        field.setValueInt(field.countChildren()).transitional_ignore();
    }
}

bool shouldSkipOutput(OperationContext* opCtx) {
    const WriteConcernOptions& writeConcern = opCtx->getWriteConcern();
    return writeConcern.wMode.empty() && writeConcern.wNumNodes == 0 &&
        (writeConcern.syncMode == WriteConcernOptions::SyncMode::NONE ||
         writeConcern.syncMode == WriteConcernOptions::SyncMode::UNSET);
}

enum class ReplyStyle { kUpdate, kNotUpdate };  // update has extra fields.
void serializeReply(OperationContext* opCtx,
                    ReplyStyle replyStyle,
                    bool continueOnError,
                    size_t opsInBatch,
                    WriteResult result,
                    BSONObjBuilder* out) {
    if (shouldSkipOutput(opCtx))
        return;

    if (continueOnError && !result.results.empty()) {
        const auto& lastResult = result.results.back();
        if (lastResult == ErrorCodes::StaleConfig ||
            lastResult == ErrorCodes::CannotImplicitlyCreateCollection) {
            // For ordered:false commands we need to duplicate these error results for all ops
            // after we stopped. See handleError() in write_ops_exec.cpp for more info.
            auto err = result.results.back();
            while (result.results.size() < opsInBatch) {
                result.results.emplace_back(err);
            }
        }
    }

    long long n = 0;
    long long nModified = 0;
    std::vector<BSONObj> upsertInfo;
    std::vector<BSONObj> errors;
    BSONSizeTracker upsertInfoSizeTracker;
    BSONSizeTracker errorsSizeTracker;

    auto errorMessage = [&, errorSize = size_t(0) ](StringData rawMessage) mutable {
        // Start truncating error messages once both of these limits are exceeded.
        constexpr size_t kErrorSizeTruncationMin = 1024 * 1024;
        constexpr size_t kErrorCountTruncationMin = 2;
        if (errorSize >= kErrorSizeTruncationMin && errors.size() >= kErrorCountTruncationMin) {
            return ""_sd;
        }

        errorSize += rawMessage.size();
        return rawMessage;
    };

    for (size_t i = 0; i < result.results.size(); i++) {
        if (result.results[i].isOK()) {
            const auto& opResult = result.results[i].getValue();
            n += opResult.getN();  // Always there.
            if (replyStyle == ReplyStyle::kUpdate) {
                nModified += opResult.getNModified();
                if (auto idElement = opResult.getUpsertedId().firstElement()) {
                    BSONObjBuilder upsertedId(upsertInfoSizeTracker);
                    upsertedId.append("index", int(i));
                    upsertedId.appendAs(idElement, "_id");
                    upsertInfo.push_back(upsertedId.obj());
                }
            }
            continue;
        }

        const auto& status = result.results[i].getStatus();
        BSONObjBuilder error(errorsSizeTracker);
        error.append("index", int(i));
        if (auto staleInfo = status.extraInfo<StaleConfigInfo>()) {
            error.append("code", int(ErrorCodes::StaleShardVersion));  // Different from exception!
            {
                BSONObjBuilder errInfo(error.subobjStart("errInfo"));
                staleInfo->getVersionWanted().addToBSON(errInfo, "vWanted");
            }
        } else {
            error.append("code", int(status.code()));
        }
        error.append("errmsg", errorMessage(status.reason()));
        errors.push_back(error.obj());
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
        auto* replCoord = repl::ReplicationCoordinator::get(opCtx->getServiceContext());
        const auto replMode = replCoord->getReplicationMode();
        if (replMode != repl::ReplicationCoordinator::modeNone) {
            const auto lastOp = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
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

    std::unique_ptr<CommandInvocation> parse(OperationContext* opCtx,
                                             const OpMsgRequest& request) final;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kNever;
    }

    bool shouldAffectCommandCounter() const final {
        return false;
    }

    ReadWriteType getReadWriteType() const {
        return ReadWriteType::kWrite;
    }

    virtual void runImpl(OperationContext* opCtx,
                         const OpMsgRequest& request,
                         BSONObjBuilder& result) const = 0;

    virtual Status explainImpl(OperationContext* opCtx,
                               const OpMsgRequest& request,
                               ExplainOptions::Verbosity verbosity,
                               BSONObjBuilder* out) const {
        return {ErrorCodes::IllegalOperation, str::stream() << "Cannot explain cmd: " << getName()};
    }

private:
    class Invocation;

    virtual BatchedCommandRequest::BatchType writeType() const = 0;
};

class WriteCommand::Invocation : public CommandInvocation {
public:
    Invocation(const WriteCommand* writeCommand, const OpMsgRequest& request, NamespaceString ns)
        : CommandInvocation(writeCommand), _request{&request}, _ns{std::move(ns)} {}

private:
    void run(OperationContext* opCtx, CommandReplyBuilder* result) final {
        try {
            try {
                _transactionChecks(opCtx);
                BSONObjBuilder bob = result->getBodyBuilder();
                command()->runImpl(opCtx, *_request, bob);
                CommandHelpers::extractOrAppendOk(bob);
            } catch (const DBException& ex) {
                LastError::get(opCtx->getClient()).setLastError(ex.code(), ex.reason());
                throw;
            }
        } catch (const ExceptionFor<ErrorCodes::Unauthorized>&) {
            CommandHelpers::logAuthViolation(opCtx, command(), *_request, ErrorCodes::Unauthorized);
            throw;
        }
    }

    void explain(OperationContext* opCtx,
                 ExplainOptions::Verbosity verbosity,
                 BSONObjBuilder* result) final {
        uassertStatusOK(command()->explainImpl(opCtx, *_request, verbosity, result));
    }

    NamespaceString ns() const final {
        return _ns;
    }

    Command::AllowedOnSecondary secondaryAllowed(ServiceContext* context) const final {
        return command()->secondaryAllowed(context);
    }

    bool supportsReadConcern(repl::ReadConcernLevel level) const final {
        return level == repl::ReadConcernLevel::kLocalReadConcern ||
            level == repl::ReadConcernLevel::kSnapshotReadConcern;
    }

    bool supportsWriteConcern() const final {
        return true;
    }

    void doCheckAuthorization(OperationContext* opCtx) const final {
        try {
            auth::checkAuthForWriteCommand(
                AuthorizationSession::get(opCtx->getClient()), command()->writeType(), *_request);
        } catch (const DBException& e) {
            LastError::get(opCtx->getClient()).setLastError(e.code(), e.reason());
            throw;
        }
    }

    const WriteCommand* command() const {
        return static_cast<const WriteCommand*>(definition());
    }

    void _transactionChecks(OperationContext* opCtx) const {
        auto session = OperationContextSession::get(opCtx);
        if (!session || !session->inSnapshotReadOrMultiDocumentTransaction())
            return;
        uassert(50791,
                str::stream() << "Cannot write to system collection " << ns().toString()
                              << " within a transaction.",
                !ns().isSystem());
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        uassert(50790,
                str::stream() << "Cannot write to unreplicated collection " << ns().toString()
                              << " within a transaction.",
                !replCoord->isOplogDisabledFor(opCtx, ns()));
    }

    const OpMsgRequest* _request;
    NamespaceString _ns;
};

std::unique_ptr<CommandInvocation> WriteCommand::parse(OperationContext* opCtx,
                                                       const OpMsgRequest& request) {
    return stdx::make_unique<Invocation>(
        this, request, NamespaceString(parseNs(request.getDatabase().toString(), request.body)));
}

class CmdInsert final : public WriteCommand {
public:
    CmdInsert() : WriteCommand("insert") {}

    void redactForLogging(mutablebson::Document* cmdObj) const final {
        redactTooLongLog(cmdObj, "documents");
    }

    std::string help() const final {
        return "insert documents";
    }

    void runImpl(OperationContext* opCtx,
                 const OpMsgRequest& request,
                 BSONObjBuilder& result) const final {
        const auto batch = InsertOp::parse(request);
        auto reply = performInserts(opCtx, batch);
        serializeReply(opCtx,
                       ReplyStyle::kNotUpdate,
                       !batch.getWriteCommandBase().getOrdered(),
                       batch.getDocuments().size(),
                       std::move(reply),
                       &result);
    }

    BatchedCommandRequest::BatchType writeType() const override {
        return BatchedCommandRequest::BatchType_Insert;
    }
} cmdInsert;

class CmdUpdate final : public WriteCommand {
public:
    CmdUpdate() : WriteCommand("update") {}

    void redactForLogging(mutablebson::Document* cmdObj) const final {
        redactTooLongLog(cmdObj, "updates");
    }

    std::string help() const final {
        return "update documents";
    }

    void runImpl(OperationContext* opCtx,
                 const OpMsgRequest& request,
                 BSONObjBuilder& result) const final {
        const auto batch = UpdateOp::parse(request);
        auto reply = performUpdates(opCtx, batch);
        serializeReply(opCtx,
                       ReplyStyle::kUpdate,
                       !batch.getWriteCommandBase().getOrdered(),
                       batch.getUpdates().size(),
                       std::move(reply),
                       &result);
    }

    Status explainImpl(OperationContext* opCtx,
                       const OpMsgRequest& opMsgRequest,
                       ExplainOptions::Verbosity verbosity,
                       BSONObjBuilder* out) const final {
        const auto batch = UpdateOp::parse(opMsgRequest);
        uassert(ErrorCodes::InvalidLength,
                "explained write batches must be of size 1",
                batch.getUpdates().size() == 1);

        UpdateLifecycleImpl updateLifecycle(batch.getNamespace());
        UpdateRequest updateRequest(batch.getNamespace());
        updateRequest.setLifecycle(&updateLifecycle);
        updateRequest.setQuery(batch.getUpdates()[0].getQ());
        updateRequest.setUpdates(batch.getUpdates()[0].getU());
        updateRequest.setCollation(write_ops::collationOf(batch.getUpdates()[0]));
        updateRequest.setArrayFilters(write_ops::arrayFiltersOf(batch.getUpdates()[0]));
        updateRequest.setMulti(batch.getUpdates()[0].getMulti());
        updateRequest.setUpsert(batch.getUpdates()[0].getUpsert());
        updateRequest.setYieldPolicy(PlanExecutor::YIELD_AUTO);
        updateRequest.setExplain();

        ParsedUpdate parsedUpdate(opCtx, &updateRequest);
        uassertStatusOK(parsedUpdate.parseRequest());

        // Explains of write commands are read-only, but we take write locks so that timing
        // info is more accurate.
        AutoGetCollection collection(opCtx, batch.getNamespace(), MODE_IX);

        auto exec = uassertStatusOK(getExecutorUpdate(
            opCtx, &CurOp::get(opCtx)->debug(), collection.getCollection(), &parsedUpdate));
        Explain::explainStages(exec.get(), collection.getCollection(), verbosity, out);
        return Status::OK();
    }

    BatchedCommandRequest::BatchType writeType() const override {
        return BatchedCommandRequest::BatchType_Update;
    }
} cmdUpdate;

class CmdDelete final : public WriteCommand {
public:
    CmdDelete() : WriteCommand("delete") {}

    void redactForLogging(mutablebson::Document* cmdObj) const final {
        redactTooLongLog(cmdObj, "deletes");
    }

    std::string help() const final {
        return "delete documents";
    }

    void runImpl(OperationContext* opCtx,
                 const OpMsgRequest& request,
                 BSONObjBuilder& result) const final {
        const auto batch = DeleteOp::parse(request);
        auto reply = performDeletes(opCtx, batch);
        serializeReply(opCtx,
                       ReplyStyle::kNotUpdate,
                       !batch.getWriteCommandBase().getOrdered(),
                       batch.getDeletes().size(),
                       std::move(reply),
                       &result);
    }

    Status explainImpl(OperationContext* opCtx,
                       const OpMsgRequest& opMsgRequest,
                       ExplainOptions::Verbosity verbosity,
                       BSONObjBuilder* out) const final {
        const auto batch = DeleteOp::parse(opMsgRequest);
        uassert(ErrorCodes::InvalidLength,
                "explained write batches must be of size 1",
                batch.getDeletes().size() == 1);

        DeleteRequest deleteRequest(batch.getNamespace());
        deleteRequest.setQuery(batch.getDeletes()[0].getQ());
        deleteRequest.setCollation(write_ops::collationOf(batch.getDeletes()[0]));
        deleteRequest.setMulti(batch.getDeletes()[0].getMulti());
        deleteRequest.setYieldPolicy(PlanExecutor::YIELD_AUTO);
        deleteRequest.setExplain();

        ParsedDelete parsedDelete(opCtx, &deleteRequest);
        uassertStatusOK(parsedDelete.parseRequest());

        // Explains of write commands are read-only, but we take write locks so that timing
        // info is more accurate.
        AutoGetCollection collection(opCtx, batch.getNamespace(), MODE_IX);

        // Explain the plan tree.
        auto exec = uassertStatusOK(getExecutorDelete(
            opCtx, &CurOp::get(opCtx)->debug(), collection.getCollection(), &parsedDelete));
        Explain::explainStages(exec.get(), collection.getCollection(), verbosity, out);
        return Status::OK();
    }

    BatchedCommandRequest::BatchType writeType() const override {
        return BatchedCommandRequest::BatchType_Delete;
    }
} cmdDelete;

}  // namespace
}  // namespace mongo
