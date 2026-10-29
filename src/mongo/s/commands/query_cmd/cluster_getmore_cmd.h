// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/commands.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/memory_tracking/query_memory_load_shedding.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/stats/counters.h"
#include "mongo/s/query/exec/cluster_cursor_manager.h"
#include "mongo/s/query/planner/cluster_find.h"
#include "mongo/util/modules.h"

namespace mongo {

// getMore can run with any readConcern, because cursor-creating commands like find can run with any
// readConcern.  However, since getMore automatically uses the readConcern of the command that
// created the cursor, it is not appropriate to apply the default readConcern (just as
// client-specified readConcern isn't appropriate).
inline const ReadConcernSupportResult kSupportsReadConcernResult{
    Status::OK(),
    {{ErrorCodes::InvalidOptions,
      "default read concern not permitted (getMore uses the cursor's read concern)"}}};

/**
 * Implements the cluster getMore command on both mongos (router) and shard servers.
 *
 * Retrieves more from an existing mongos cursor corresponding to the cursor id passed from the
 * application. In order to generate these results, may issue getMore commands to remote nodes in
 * one or more shards.
 *
 * The 'Impl' template parameter is a small struct that provides the properties that differ between
 * the 'ClusterGetMoreCmdD' and 'ClusterGetMoreCmdS' variants, such as:
 *  - The command name.
 *  - The stable API version of the command via 'getApiVersions()'.
 *  - Whether the command can run via 'checkCanRunHere()'.
 *
 * The class uses CRTP with 'TypedCommand' where:
 *  - The type ClusterGetMoreCmdBase<Impl> is passed as the concrete type for the template
 * parameter.
 *  - 'Impl' provides the command-specific behavior.
 *
 * This template class provides the shared functionality between the command variants such as the
 * 'run()' function.
 *
 * See 'cluster_getmore_cmd_d.cpp' and 'cluster_getmore_cmd_s.cpp' for more details.
 */
template <typename Impl>
class ClusterGetMoreCmdBase final : public TypedCommand<ClusterGetMoreCmdBase<Impl>> {
public:
    using TC = TypedCommand<ClusterGetMoreCmdBase<Impl>>;
    using Request = typename Impl::Request;
    using Reply = typename Impl::Reply;
    ClusterGetMoreCmdBase() : TC(Impl::kCommandName) {}

    const std::set<std::string>& apiVersions() const override {
        return Impl::getApiVersions();
    }

    bool allowedInTransactions() const final {
        return true;
    }

    bool enableDiagnosticPrintingOnFailure() const final {
        return true;
    }

    class Invocation final : public TC::MinimalInvocationBase {
    public:
        using TC::MinimalInvocationBase::MinimalInvocationBase;
        using TC::MinimalInvocationBase::request;

    private:
        NamespaceString ns() const override {
            return NamespaceStringUtil::deserialize(request().getDbName(),
                                                    request().getCollection());
        }

        const DatabaseName& db() const override {
            return request().getDbName();
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                     bool isImplicitDefault) const override {
            return kSupportsReadConcernResult;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            Impl::doCheckAuthorization(
                opCtx, ns(), request().getCommandParameter(), request().getTerm().is_initialized());
        }

        const GenericArguments& getGenericArguments() const override {
            return request().getGenericArguments();
        }

        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* reply) override {
            // Counted as a getMore, not as a command.
            globalOpCounters().gotGetMore();

            Impl::checkCanRunHere(opCtx);
            markOperationQueryMemorySheddingEligible(opCtx);

            auto bob = reply->getBodyBuilder();
            auto response = uassertStatusOK(ClusterFind::runGetMore(opCtx, request()));
            if (opCtx->isExhaust() && response.getCursorId() != 0) {
                // Indicate that an exhaust message should be generated and the previous BSONObj
                // command parameters should be reused as the next BSONObj command parameters.
                reply->setNextInvocation(boost::none);
            }
            response.addToBSON(CursorResponse::ResponseType::SubsequentResponse, &bob);

            if (getTestCommandsEnabled()) {
                validateResult(bob.asTempObj());
            }
        }

        // TODO SERVER-121034: Remove validateResult() once CursorResponseBuilder is typed.
        void validateResult(const BSONObj& replyObj) {
            CursorGetMoreReply::parse(replyObj.removeField("ok"),
                                      IDLParserContext{"CursorGetMoreReply"});
        }
    };

    typename TC::AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return TC::AllowedOnSecondary::kAlways;
    }

    bool maintenanceOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return false;
    }

    /**
     * A getMore command increments the getMore counter, not the command counter.
     */
    bool shouldAffectCommandCounter() const override {
        return false;
    }

    typename TC::ReadWriteType getReadWriteType() const override {
        return TC::ReadWriteType::kRead;
    }

    std::string help() const override {
        return "retrieve more documents for a cursor id";
    }

    LogicalOp getLogicalOp() const override {
        return LogicalOp::opGetMore;
    }
};


}  // namespace mongo
