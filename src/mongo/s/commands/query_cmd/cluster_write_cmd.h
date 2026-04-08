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
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/router_role/collection_routing_info_targeter.h"
#include "mongo/db/service_context.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/write_ops/batch_write_exec.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/modules.h"

#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace mongo {

/**
 * Shared utility functions for mongos write commands.
 */
namespace cluster_write_cmd {

/**
 * Changes the shard key for the document if the response object contains a
 * WouldChangeOwningShard error. If the original command was sent as a retryable write, starts a
 * transaction on the same session and txnNum, deletes the original document, inserts the new
 * one, and commits the transaction. If the original command is part of a transaction, deletes
 * the original document and inserts the new one. Returns whether or not we actually complete
 * the delete and insert.
 */
bool handleWouldChangeOwningShardError(OperationContext* opCtx,
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
void commandOpWrite(OperationContext* opCtx,
                    const NamespaceString& nss,
                    const BSONObj& command,
                    BatchItemRef targetingBatchItem,
                    const CollectionRoutingInfoTargeter& targeter,
                    std::vector<AsyncRequestsSender::Response>* results);

/**
 * Runs a two-phase protocol to explain an updateOne/deleteOne without a shard key or _id.
 * Returns true if we successfully ran the protocol, false otherwise.
 */
bool runExplainWithoutShardKey(OperationContext* opCtx,
                               const BatchedCommandRequest& req,
                               const NamespaceString& nss,
                               ExplainOptions::Verbosity verbosity,
                               BSONObjBuilder* result);

void executeWriteOpExplain(OperationContext* opCtx,
                           const BatchedCommandRequest& batchedRequest,
                           const BSONObj& requestObj,
                           ExplainOptions::Verbosity verbosity,
                           rpc::ReplyBuilderInterface* result);

/**
 * Shared run implementation for all cluster write commands (insert, update, delete).
 */
bool runImpl(OperationContext* opCtx,
             BatchedCommandRequest& batchedRequest,
             UpdateMetrics* updateMetrics,
             BSONObjBuilder& result);

}  // namespace cluster_write_cmd

/**
 * CRTP mixin providing shared command-level overrides for cluster write commands.
 */
template <typename Derived>
class ClusterWriteCmd : public TypedCommand<Derived> {
    using TC = TypedCommand<Derived>;

public:
    using TC::TC;

    typename TC::AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return TC::AllowedOnSecondary::kNever;
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

    typename TC::ReadWriteType getReadWriteType() const final {
        return TC::ReadWriteType::kWrite;
    }

    bool enableDiagnosticPrintingOnFailure() const final {
        return true;
    }
};

template <typename Impl>
class ClusterInsertCmdBase final : public ClusterWriteCmd<ClusterInsertCmdBase<Impl>> {
    using TC = TypedCommand<ClusterInsertCmdBase<Impl>>;

public:
    using Request = write_ops::InsertCommandRequest;

    ClusterInsertCmdBase() : ClusterWriteCmd<ClusterInsertCmdBase<Impl>>(Impl::kName) {}

    const std::set<std::string>& apiVersions() const override {
        return Impl::getApiVersions();
    }

    class Invocation final : public TC::MinimalInvocationBase {
        using TC::MinimalInvocationBase::MinimalInvocationBase;
        using TC::MinimalInvocationBase::request;
        using TC::MinimalInvocationBase::unparsedRequest;

    public:
        Invocation(OperationContext* opCtx, Command* cmd, const OpMsgRequest& opMsgRequest)
            : TC::MinimalInvocationBase(opCtx, cmd, opMsgRequest), _batchedRequest(request()) {
            InsertOp::validate(request());
            uassert(ErrorCodes::InvalidNamespace,
                    "Cannot specify insert without a real namespace",
                    !request().getNamespace().isCollectionlessAggregateNS());
            checkIsTimeseriesNamespace(request().getWriteCommandRequestBase());
            _batchedRequest = BatchedCommandRequest::cloneInsertWithIds(std::move(_batchedRequest));
        }

    private:
        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* result) override {
            Impl::checkCanRunHere(opCtx);
            BSONObjBuilder bob = result->getBodyBuilder();
            bool ok = cluster_write_cmd::runImpl(opCtx, _batchedRequest, nullptr, bob);
            if (!ok)
                CommandHelpers::appendSimpleCommandStatus(bob, ok);
        }

        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     rpc::ReplyBuilderInterface* result) override {
            Impl::checkCanExplainHere(opCtx);
            uassert(ErrorCodes::InvalidLength,
                    "explained write batches must be of size 1",
                    _batchedRequest.sizeWriteOps() == 1U);
            cluster_write_cmd::executeWriteOpExplain(
                opCtx, _batchedRequest, unparsedRequest().body, verbosity, result);
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        bool supportsRawData() const override {
            return true;
        }

        NamespaceString ns() const override {
            return _batchedRequest.getNS();
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            try {
                Impl::doCheckAuthorization(AuthorizationSession::get(opCtx->getClient()),
                                           _batchedRequest.getBypassDocumentValidation(),
                                           request());
            } catch (const DBException& e) {
                NotPrimaryErrorTracker::get(opCtx->getClient()).recordError(e.code());
                throw;
            }
        }

        BatchedCommandRequest _batchedRequest;
    };

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
class ClusterUpdateCmdBase final : public ClusterWriteCmd<ClusterUpdateCmdBase<Impl>> {
    using TC = TypedCommand<ClusterUpdateCmdBase<Impl>>;

public:
    using Request = write_ops::UpdateCommandRequest;

    ClusterUpdateCmdBase() : ClusterWriteCmd<ClusterUpdateCmdBase<Impl>>(Impl::kName) {}

    const std::set<std::string>& apiVersions() const override {
        return Impl::getApiVersions();
    }

    class Invocation final : public TC::MinimalInvocationBase {
        using TC::MinimalInvocationBase::MinimalInvocationBase;
        using TC::MinimalInvocationBase::request;
        using TC::MinimalInvocationBase::unparsedRequest;

    public:
        Invocation(OperationContext* opCtx, Command* cmd, const OpMsgRequest& opMsgRequest)
            : TC::MinimalInvocationBase(opCtx, cmd, opMsgRequest), _batchedRequest(request()) {
            UpdateOp::validate(request());
            uassert(ErrorCodes::InvalidNamespace,
                    "Cannot specify update without a real namespace",
                    !request().getNamespace().isCollectionlessAggregateNS());
            checkIsTimeseriesNamespace(request().getWriteCommandRequestBase());
            if (!opCtx->isCommandForwardedFromRouter()) {
                uassert(51195,
                        "Cannot specify runtime constants option to a mongos",
                        !_batchedRequest.hasLegacyRuntimeConstants());
                _batchedRequest.setLegacyRuntimeConstants(
                    Variables::generateRuntimeConstants(opCtx));
            }
        }

    private:
        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* result) override {
            Impl::checkCanRunHere(opCtx);
            BSONObjBuilder bob = result->getBodyBuilder();
            bool ok = cluster_write_cmd::runImpl(opCtx, _batchedRequest, _getUpdateMetrics(), bob);
            if (!ok)
                CommandHelpers::appendSimpleCommandStatus(bob, ok);
        }

        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     rpc::ReplyBuilderInterface* result) override {
            Impl::checkCanExplainHere(opCtx);
            uassert(ErrorCodes::InvalidLength,
                    "explained write batches must be of size 1",
                    _batchedRequest.sizeWriteOps() == 1U);
            cluster_write_cmd::executeWriteOpExplain(
                opCtx, _batchedRequest, unparsedRequest().body, verbosity, result);
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        bool supportsRawData() const override {
            return true;
        }

        NamespaceString ns() const override {
            return _batchedRequest.getNS();
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            try {
                Impl::doCheckAuthorization(AuthorizationSession::get(opCtx->getClient()),
                                           _batchedRequest.getBypassDocumentValidation(),
                                           request());
            } catch (const DBException& e) {
                NotPrimaryErrorTracker::get(opCtx->getClient()).recordError(e.code());
                throw;
            }
        }

        UpdateMetrics* _getUpdateMetrics() const {
            auto* cmd = static_cast<const ClusterUpdateCmdBase*>(this->definition());
            tassert(11052600,
                    str::stream() << "Missing UpdateMetrics in " << cmd->getName(),
                    cmd->_updateMetrics);
            return &*cmd->_updateMetrics;
        }

        BatchedCommandRequest _batchedRequest;
    };

    std::string help() const override {
        return "update documents";
    }

    LogicalOp getLogicalOp() const override {
        return LogicalOp::opUpdate;
    }

    const AuthorizationContract* getAuthorizationContract() const final {
        return &::mongo::write_ops::UpdateCommandRequest::kAuthorizationContract;
    }

protected:
    void doInitializeClusterRole(ClusterRole role) override {
        ClusterWriteCmd<ClusterUpdateCmdBase<Impl>>::doInitializeClusterRole(role);
        _updateMetrics.emplace(this->getName(), role);
    }

private:
    // Update related command execution metrics.
    mutable boost::optional<UpdateMetrics> _updateMetrics;
};

template <typename Impl>
class ClusterDeleteCmdBase final : public ClusterWriteCmd<ClusterDeleteCmdBase<Impl>> {
    using TC = TypedCommand<ClusterDeleteCmdBase<Impl>>;

public:
    using Request = write_ops::DeleteCommandRequest;

    ClusterDeleteCmdBase() : ClusterWriteCmd<ClusterDeleteCmdBase<Impl>>(Impl::kName) {}

    const std::set<std::string>& apiVersions() const override {
        return Impl::getApiVersions();
    }

    class Invocation final : public TC::MinimalInvocationBase {
        using TC::MinimalInvocationBase::MinimalInvocationBase;
        using TC::MinimalInvocationBase::request;
        using TC::MinimalInvocationBase::unparsedRequest;

    public:
        Invocation(OperationContext* opCtx, Command* cmd, const OpMsgRequest& opMsgRequest)
            : TC::MinimalInvocationBase(opCtx, cmd, opMsgRequest), _batchedRequest(request()) {
            DeleteOp::validate(request());
            uassert(ErrorCodes::InvalidNamespace,
                    "Cannot specify delete without a real namespace",
                    !request().getNamespace().isCollectionlessAggregateNS());
            checkIsTimeseriesNamespace(request().getWriteCommandRequestBase());
        }

    private:
        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* result) override {
            Impl::checkCanRunHere(opCtx);
            BSONObjBuilder bob = result->getBodyBuilder();
            bool ok = cluster_write_cmd::runImpl(opCtx, _batchedRequest, nullptr, bob);
            if (!ok)
                CommandHelpers::appendSimpleCommandStatus(bob, ok);
        }

        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     rpc::ReplyBuilderInterface* result) override {
            Impl::checkCanExplainHere(opCtx);
            uassert(ErrorCodes::InvalidLength,
                    "explained write batches must be of size 1",
                    _batchedRequest.sizeWriteOps() == 1U);
            cluster_write_cmd::executeWriteOpExplain(
                opCtx, _batchedRequest, unparsedRequest().body, verbosity, result);
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        bool supportsRawData() const override {
            return true;
        }

        NamespaceString ns() const override {
            return _batchedRequest.getNS();
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            try {
                Impl::doCheckAuthorization(AuthorizationSession::get(opCtx->getClient()),
                                           _batchedRequest.getBypassDocumentValidation(),
                                           request());
            } catch (const DBException& e) {
                NotPrimaryErrorTracker::get(opCtx->getClient()).recordError(e.code());
                throw;
            }
        }

        BatchedCommandRequest _batchedRequest;
    };

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
