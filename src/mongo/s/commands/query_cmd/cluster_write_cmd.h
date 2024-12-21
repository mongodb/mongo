/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#pragma once

#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/update_metrics.h"
#include "mongo/db/commands/query_cmd/write_commands_common.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/not_primary_error_tracker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/explain_verbosity_gen.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/s/write_ops/batch_write_exec.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

namespace mongo {

/**
 * Base class for mongos write commands.
 */
class ClusterWriteCmd : public Command {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kNever;
    }

    bool shouldAffectCommandCounter() const final {
        return false;
    }

    bool supportsRetryableWrite() const final {
        return true;
    }

    bool allowedInTransactions() const final {
        return true;
    }

    ReadWriteType getReadWriteType() const final {
        return Command::ReadWriteType::kWrite;
    }

    bool enableDiagnosticPrintingOnFailure() const final {
        return true;
    }

    /**
     * Changes the shard key for the document if the response object contains a
     * WouldChangeOwningShard error. If the original command was sent as a retryable write, starts a
     * transaction on the same session and txnNum, deletes the original document, inserts the new
     * one, and commits the transaction. If the original command is part of a transaction, deletes
     * the original document and inserts the new one. Returns whether or not we actually complete
     * the delete and insert.
     */
    static bool handleWouldChangeOwningShardError(OperationContext* opCtx,
                                                  BatchedCommandRequest* request,
                                                  const NamespaceString& nss,
                                                  BatchedCommandResponse* response,
                                                  BatchWriteExecStats stats);

    /**
     * Executes a write command against a particular database, and targets the command based on
     * a write operation.
     *
     * Does *not* retry or retarget if the metadata is stale.
     */
    static void commandOpWrite(OperationContext* opCtx,
                               const NamespaceString& nss,
                               const BSONObj& command,
                               BatchItemRef targetingBatchItem,
                               std::vector<AsyncRequestsSender::Response>* results);

    /**
     * Runs a two-phase protocol to explain an updateOne/deleteOne without a shard key or _id.
     * Returns true if we successfully ran the protocol, false otherwise.
     */
    static bool runExplainWithoutShardKey(OperationContext* opCtx,
                                          const BatchedCommandRequest& req,
                                          const NamespaceString& nss,
                                          ExplainOptions::Verbosity verbosity,
                                          BSONObjBuilder* result);

    static void executeWriteOpExplain(OperationContext* opCtx,
                                      const BatchedCommandRequest& batchedRequest,
                                      const BSONObj& requestObj,
                                      ExplainOptions::Verbosity verbosity,
                                      rpc::ReplyBuilderInterface* result);

protected:
    class InvocationBase;

    explicit ClusterWriteCmd(StringData name) : Command(name) {}

private:
    virtual UpdateMetrics* getUpdateMetrics() {
        return nullptr;
    }
};

class ClusterWriteCmd::InvocationBase : public CommandInvocation {
public:
    InvocationBase(const ClusterWriteCmd* command,
                   const OpMsgRequest& request,
                   BatchedCommandRequest batchedRequest)
        : CommandInvocation(command),
          _request{&request},
          _batchedRequest{std::move(batchedRequest)} {}

    const BatchedCommandRequest& getBatchedRequest() const {
        return _batchedRequest;
    }

    const GenericArguments& getGenericArguments() const override {
        return getBatchedRequest().getGenericArguments();
    }

    bool getBypass() const {
        return _batchedRequest.getBypassDocumentValidation();
    }

private:
    virtual void preRunImplHook(OperationContext* opCtx) const = 0;
    virtual void preExplainImplHook(OperationContext* opCtx) const = 0;
    virtual void doCheckAuthorizationHook(AuthorizationSession* authzSession) const = 0;

    bool runImpl(OperationContext* opCtx,
                 const OpMsgRequest& request,
                 BatchedCommandRequest& batchedRequest,
                 BSONObjBuilder& result) const;

    void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* result) override;

    void explain(OperationContext* opCtx,
                 ExplainOptions::Verbosity verbosity,
                 rpc::ReplyBuilderInterface* result) override;

    NamespaceString ns() const override {
        return _batchedRequest.getNS();
    }

    const DatabaseName& db() const override {
        return _batchedRequest.getNS().dbName();
    }

    bool supportsWriteConcern() const override {
        return true;
    }

    void doCheckAuthorization(OperationContext* opCtx) const final {
        try {
            doCheckAuthorizationHook(AuthorizationSession::get(opCtx->getClient()));
        } catch (const DBException& e) {
            NotPrimaryErrorTracker::get(opCtx->getClient()).recordError(e.code());
            throw;
        }
    }

    const ClusterWriteCmd* command() const {
        return static_cast<const ClusterWriteCmd*>(definition());
    }

    const OpMsgRequest* _request;
    BatchedCommandRequest _batchedRequest;
};

template <typename Impl>
class ClusterInsertCmdBase final : public ClusterWriteCmd {
public:
    ClusterInsertCmdBase() : ClusterWriteCmd(Impl::kName) {}

    const std::set<std::string>& apiVersions() const override {
        return Impl::getApiVersions();
    }

private:
    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

    private:
        void preRunImplHook(OperationContext* opCtx) const final {
            Impl::checkCanRunHere(opCtx);
        }

        void preExplainImplHook(OperationContext* opCtx) const final {
            Impl::checkCanExplainHere(opCtx);
        }

        void doCheckAuthorizationHook(AuthorizationSession* authzSession) const final {
            Impl::doCheckAuthorization(
                authzSession, getBypass(), getBatchedRequest().getInsertRequest());
        }
    };

    std::unique_ptr<CommandInvocation> parse(OperationContext* opCtx,
                                             const OpMsgRequest& request) final {
        return std::make_unique<Invocation>(
            this,
            request,
            BatchedCommandRequest::cloneInsertWithIds(BatchedCommandRequest::parseInsert(request)));
    }

    std::string help() const override {
        return "insert documents";
    }

    LogicalOp getLogicalOp() const override {
        return LogicalOp::opInsert;
    }

    const AuthorizationContract* getAuthorizationContract() const final {
        return &::mongo::write_ops::InsertCommandRequest::kAuthorizationContract;
    }
};

template <typename Impl>
class ClusterUpdateCmdBase final : public ClusterWriteCmd {
public:
    ClusterUpdateCmdBase() : ClusterWriteCmd{Impl::kName} {}

    const std::set<std::string>& apiVersions() const override {
        return Impl::getApiVersions();
    }

protected:
    void doInitializeClusterRole(ClusterRole role) override {
        ClusterWriteCmd::doInitializeClusterRole(role);
        _updateMetrics.emplace(getName(), role);
    }

private:
    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

    private:
        void preRunImplHook(OperationContext* opCtx) const final {
            Impl::checkCanRunHere(opCtx);
        }

        void preExplainImplHook(OperationContext* opCtx) const final {
            Impl::checkCanExplainHere(opCtx);
        }

        void doCheckAuthorizationHook(AuthorizationSession* authzSession) const final {
            Impl::doCheckAuthorization(
                authzSession, getBypass(), getBatchedRequest().getUpdateRequest());
        }
    };

    std::unique_ptr<CommandInvocation> parse(OperationContext* opCtx,
                                             const OpMsgRequest& request) final {
        auto parsedRequest = BatchedCommandRequest::parseUpdate(request);
        if (!opCtx->isCommandForwardedFromRouter()) {
            uassert(51195,
                    "Cannot specify runtime constants option to a mongos",
                    !parsedRequest.hasLegacyRuntimeConstants());
            parsedRequest.setLegacyRuntimeConstants(Variables::generateRuntimeConstants(opCtx));
        }
        return std::make_unique<Invocation>(this, request, std::move(parsedRequest));
    }

    std::string help() const override {
        return "update documents";
    }

    LogicalOp getLogicalOp() const override {
        return LogicalOp::opUpdate;
    }

    const AuthorizationContract* getAuthorizationContract() const final {
        return &::mongo::write_ops::UpdateCommandRequest::kAuthorizationContract;
    }

    UpdateMetrics* getUpdateMetrics() override {
        invariant(_updateMetrics);
        return &*_updateMetrics;
    }

    // Update related command execution metrics.
    boost::optional<UpdateMetrics> _updateMetrics;
};

template <typename Impl>
class ClusterDeleteCmdBase final : public ClusterWriteCmd {
public:
    ClusterDeleteCmdBase() : ClusterWriteCmd(Impl::kName) {}

    const std::set<std::string>& apiVersions() const override {
        return Impl::getApiVersions();
    }

private:
    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

    private:
        void preRunImplHook(OperationContext* opCtx) const final {
            Impl::checkCanRunHere(opCtx);
        }

        void preExplainImplHook(OperationContext* opCtx) const final {
            Impl::checkCanExplainHere(opCtx);
        }

        void doCheckAuthorizationHook(AuthorizationSession* authzSession) const final {
            Impl::doCheckAuthorization(
                authzSession, getBypass(), getBatchedRequest().getDeleteRequest());
        }
    };

    std::unique_ptr<CommandInvocation> parse(OperationContext* opCtx,
                                             const OpMsgRequest& request) final {
        return std::make_unique<Invocation>(
            this, request, BatchedCommandRequest::parseDelete(request));
    }

    std::string help() const override {
        return "delete documents";
    }

    LogicalOp getLogicalOp() const override {
        return LogicalOp::opDelete;
    }

    const AuthorizationContract* getAuthorizationContract() const final {
        return &::mongo::write_ops::DeleteCommandRequest::kAuthorizationContract;
    }
};

}  // namespace mongo
