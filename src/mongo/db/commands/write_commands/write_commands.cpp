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
#include "mongo/db/catalog/document_validation.h"
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

protected:
    class InvocationBase;

private:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kNever;
    }

    bool shouldAffectCommandCounter() const final {
        return false;
    }

    ReadWriteType getReadWriteType() const final {
        return ReadWriteType::kWrite;
    }
};

class WriteCommand::InvocationBase : public CommandInvocation {
public:
    InvocationBase(const WriteCommand* writeCommand, const OpMsgRequest& request)
        : CommandInvocation(writeCommand), _request(&request) {}

    bool getBypass() const {
        return shouldBypassDocumentValidationForCommand(_request->body);
    }

private:
    // Customization point for 'doCheckAuthorization'.
    virtual void doCheckAuthorizationImpl(AuthorizationSession* authzSession) const = 0;

    // Customization point for 'run'.
    virtual void runImpl(OperationContext* opCtx, BSONObjBuilder& result) const = 0;

    void run(OperationContext* opCtx, CommandReplyBuilder* result) final {
        try {
            try {
                _transactionChecks(opCtx);
                BSONObjBuilder bob = result->getBodyBuilder();
                runImpl(opCtx, bob);
                CommandHelpers::extractOrAppendOk(bob);
            } catch (const DBException& ex) {
                LastError::get(opCtx->getClient()).setLastError(ex.code(), ex.reason());
                throw;
            }
        } catch (const ExceptionFor<ErrorCodes::Unauthorized>&) {
            CommandHelpers::logAuthViolation(opCtx, this, *_request, ErrorCodes::Unauthorized);
            throw;
        }
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
            doCheckAuthorizationImpl(AuthorizationSession::get(opCtx->getClient()));
        } catch (const DBException& e) {
            LastError::get(opCtx->getClient()).setLastError(e.code(), e.reason());
            throw;
        }
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
};

class CmdInsert final : public WriteCommand {
public:
    CmdInsert() : WriteCommand("insert") {}

private:
    class Invocation final : public InvocationBase {
    public:
        Invocation(const WriteCommand* cmd, const OpMsgRequest& request)
            : InvocationBase(cmd, request), _batch(InsertOp::parse(request)) {}

    private:
        NamespaceString ns() const override {
            return _batch.getNamespace();
        }

        void doCheckAuthorizationImpl(AuthorizationSession* authzSession) const override {
            auth::checkAuthForInsertCommand(authzSession, getBypass(), _batch);
        }

        void runImpl(OperationContext* opCtx, BSONObjBuilder& result) const override {
            auto reply = performInserts(opCtx, _batch);
            serializeReply(opCtx,
                           ReplyStyle::kNotUpdate,
                           !_batch.getWriteCommandBase().getOrdered(),
                           _batch.getDocuments().size(),
                           std::move(reply),
                           &result);
        }

        write_ops::Insert _batch;
    };

    std::unique_ptr<CommandInvocation> parse(OperationContext*,
                                             const OpMsgRequest& request) override {
        return stdx::make_unique<Invocation>(this, request);
    }

    void redactForLogging(mutablebson::Document* cmdObj) const final {
        redactTooLongLog(cmdObj, "documents");
    }

    std::string help() const final {
        return "insert documents";
    }
} cmdInsert;

class CmdUpdate final : public WriteCommand {
public:
    CmdUpdate() : WriteCommand("update") {}

private:
    class Invocation final : public InvocationBase {
    public:
        Invocation(const WriteCommand* cmd, const OpMsgRequest& request)
            : InvocationBase(cmd, request), _batch(UpdateOp::parse(request)) {}

    private:
        NamespaceString ns() const override {
            return _batch.getNamespace();
        }

        void doCheckAuthorizationImpl(AuthorizationSession* authzSession) const override {
            auth::checkAuthForUpdateCommand(authzSession, getBypass(), _batch);
        }

        void runImpl(OperationContext* opCtx, BSONObjBuilder& result) const override {
            auto reply = performUpdates(opCtx, _batch);
            serializeReply(opCtx,
                           ReplyStyle::kUpdate,
                           !_batch.getWriteCommandBase().getOrdered(),
                           _batch.getUpdates().size(),
                           std::move(reply),
                           &result);
        }

        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     BSONObjBuilder* out) override {
            uassert(ErrorCodes::InvalidLength,
                    "explained write batches must be of size 1",
                    _batch.getUpdates().size() == 1);

            UpdateLifecycleImpl updateLifecycle(_batch.getNamespace());
            UpdateRequest updateRequest(_batch.getNamespace());
            updateRequest.setLifecycle(&updateLifecycle);
            updateRequest.setQuery(_batch.getUpdates()[0].getQ());
            updateRequest.setUpdates(_batch.getUpdates()[0].getU());
            updateRequest.setCollation(write_ops::collationOf(_batch.getUpdates()[0]));
            updateRequest.setArrayFilters(write_ops::arrayFiltersOf(_batch.getUpdates()[0]));
            updateRequest.setMulti(_batch.getUpdates()[0].getMulti());
            updateRequest.setUpsert(_batch.getUpdates()[0].getUpsert());
            updateRequest.setYieldPolicy(PlanExecutor::YIELD_AUTO);
            updateRequest.setExplain();

            ParsedUpdate parsedUpdate(opCtx, &updateRequest);
            uassertStatusOK(parsedUpdate.parseRequest());

            // Explains of write commands are read-only, but we take write locks so that timing
            // info is more accurate.
            AutoGetCollection collection(opCtx, _batch.getNamespace(), MODE_IX);

            auto exec = uassertStatusOK(getExecutorUpdate(
                opCtx, &CurOp::get(opCtx)->debug(), collection.getCollection(), &parsedUpdate));
            Explain::explainStages(exec.get(), collection.getCollection(), verbosity, out);
        }

        write_ops::Update _batch;
    };

    std::unique_ptr<CommandInvocation> parse(OperationContext*, const OpMsgRequest& request) {
        return stdx::make_unique<Invocation>(this, request);
    }

    void redactForLogging(mutablebson::Document* cmdObj) const final {
        redactTooLongLog(cmdObj, "updates");
    }

    std::string help() const final {
        return "update documents";
    }
} cmdUpdate;

class CmdDelete final : public WriteCommand {
public:
    CmdDelete() : WriteCommand("delete") {}

private:
    class Invocation final : public InvocationBase {
    public:
        Invocation(const WriteCommand* cmd, const OpMsgRequest& request)
            : InvocationBase(cmd, request), _batch(DeleteOp::parse(request)) {}

    private:
        NamespaceString ns() const override {
            return _batch.getNamespace();
        }

        void doCheckAuthorizationImpl(AuthorizationSession* authzSession) const override {
            auth::checkAuthForDeleteCommand(authzSession, getBypass(), _batch);
        }

        void runImpl(OperationContext* opCtx, BSONObjBuilder& result) const override {
            auto reply = performDeletes(opCtx, _batch);
            serializeReply(opCtx,
                           ReplyStyle::kNotUpdate,
                           !_batch.getWriteCommandBase().getOrdered(),
                           _batch.getDeletes().size(),
                           std::move(reply),
                           &result);
        }

        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     BSONObjBuilder* out) override {
            uassert(ErrorCodes::InvalidLength,
                    "explained write batches must be of size 1",
                    _batch.getDeletes().size() == 1);

            DeleteRequest deleteRequest(_batch.getNamespace());
            deleteRequest.setQuery(_batch.getDeletes()[0].getQ());
            deleteRequest.setCollation(write_ops::collationOf(_batch.getDeletes()[0]));
            deleteRequest.setMulti(_batch.getDeletes()[0].getMulti());
            deleteRequest.setYieldPolicy(PlanExecutor::YIELD_AUTO);
            deleteRequest.setExplain();

            ParsedDelete parsedDelete(opCtx, &deleteRequest);
            uassertStatusOK(parsedDelete.parseRequest());

            // Explains of write commands are read-only, but we take write locks so that timing
            // info is more accurate.
            AutoGetCollection collection(opCtx, _batch.getNamespace(), MODE_IX);

            // Explain the plan tree.
            auto exec = uassertStatusOK(getExecutorDelete(
                opCtx, &CurOp::get(opCtx)->debug(), collection.getCollection(), &parsedDelete));
            Explain::explainStages(exec.get(), collection.getCollection(), verbosity, out);
        }

        write_ops::Delete _batch;
    };

    std::unique_ptr<CommandInvocation> parse(OperationContext*,
                                             const OpMsgRequest& request) override {
        return stdx::make_unique<Invocation>(this, request);
    }

    void redactForLogging(mutablebson::Document* cmdObj) const final {
        redactTooLongLog(cmdObj, "deletes");
    }

    std::string help() const final {
        return "delete documents";
    }
} cmdDelete;

}  // namespace
}  // namespace mongo
