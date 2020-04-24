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

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/run_aggregate.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline.h"

namespace mongo {
namespace {

class PipelineCommand final : public Command {
public:
    PipelineCommand() : Command("aggregate") {}

    /**
     * It's not known until after parsing whether or not an aggregation command is an explain
     * request, because it might include the `explain: true` field (ie. aggregation explains do not
     * need to arrive via the `explain` command). Therefore even parsing of regular aggregation
     * commands needs to be able to handle the explain case.
     *
     * As a result, aggregation command parsing is done in parseForExplain():
     *
     * - To parse a regular aggregation command, call parseForExplain() with `explainVerbosity` of
     *   boost::none.
     *
     * - To parse an aggregation command as the sub-command in an `explain` command, call
     *   parseForExplain() with `explainVerbosity` set to the desired verbosity.
     */
    std::unique_ptr<CommandInvocation> parse(OperationContext* opCtx,
                                             const OpMsgRequest& opMsgRequest) override {
        return parseForExplain(opCtx, opMsgRequest, boost::none);
    }

    std::unique_ptr<CommandInvocation> parseForExplain(
        OperationContext* opCtx,
        const OpMsgRequest& opMsgRequest,
        boost::optional<ExplainOptions::Verbosity> explainVerbosity) override {
        const auto aggregationRequest = uassertStatusOK(AggregationRequest::parseFromBSON(
            opMsgRequest.getDatabase().toString(), opMsgRequest.body, explainVerbosity));

        auto privileges = uassertStatusOK(
            AuthorizationSession::get(opCtx->getClient())
                ->getPrivilegesForAggregate(
                    aggregationRequest.getNamespaceString(), aggregationRequest, false));

        return std::make_unique<Invocation>(
            this, opMsgRequest, std::move(aggregationRequest), std::move(privileges));
    }

    class Invocation final : public CommandInvocation {
    public:
        Invocation(Command* cmd,
                   const OpMsgRequest& request,
                   const AggregationRequest aggregationRequest,
                   PrivilegeVector privileges)
            : CommandInvocation(cmd),
              _request(request),
              _dbName(request.getDatabase().toString()),
              _aggregationRequest(std::move(aggregationRequest)),
              _liteParsedPipeline(_aggregationRequest),
              _privileges(std::move(privileges)) {}

    private:
        bool supportsWriteConcern() const override {
            return true;
        }

        bool canIgnorePrepareConflicts() const override {
            // Aggregate is a special case for prepare conflicts. It may do writes to an output
            // collection, but it enables enforcement of prepare conflicts before doing so.
            return true;
        }

        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level) const override {
            return _liteParsedPipeline.supportsReadConcern(
                level,
                _aggregationRequest.getExplain(),
                serverGlobalParams.enableMajorityReadConcern);
        }

        bool allowsSpeculativeMajorityReads() const override {
            // Currently only change stream aggregation queries are allowed to use speculative
            // majority. The aggregation command itself will check this internally and fail if
            // necessary.
            return true;
        }

        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* reply) override {
            CommandHelpers::handleMarkKillOnClientDisconnect(
                opCtx, !Pipeline::aggHasWriteStage(_request.body));

            uassertStatusOK(runAggregate(opCtx,
                                         _aggregationRequest.getNamespaceString(),
                                         _aggregationRequest,
                                         _liteParsedPipeline,
                                         _request.body,
                                         _privileges,
                                         reply));
        }

        NamespaceString ns() const override {
            return _aggregationRequest.getNamespaceString();
        }

        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     rpc::ReplyBuilderInterface* result) override {

            uassertStatusOK(runAggregate(opCtx,
                                         _aggregationRequest.getNamespaceString(),
                                         _aggregationRequest,
                                         _liteParsedPipeline,
                                         _request.body,
                                         _privileges,
                                         result));
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForPrivileges(_privileges));
        }

        const OpMsgRequest& _request;
        const std::string _dbName;
        const AggregationRequest _aggregationRequest;
        const LiteParsedPipeline _liteParsedPipeline;
        const PrivilegeVector _privileges;
    };

    std::string help() const override {
        return "Runs the aggregation command. See http://dochub.mongodb.org/core/aggregation for "
               "more details.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kOptIn;
    }
    bool maintenanceOk() const override {
        return false;
    }
    ReadWriteType getReadWriteType() const {
        return ReadWriteType::kRead;
    }

} pipelineCmd;

}  // namespace
}  // namespace mongo
