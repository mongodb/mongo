// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/explain_cmd_helpers.h"
#include "mongo/db/commands/query_cmd/explain_gen.h"
#include "mongo/db/database_name_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/service_context.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

/**
 * Implements the explain command on mongos.
 */

class ClusterExplainCmd final : public ExplainCmdVersion1Gen<ClusterExplainCmd> {
public:
    using Request = ExplainCommandRequest;

    /**
     * Running an explain on a secondary requires explicitly setting slaveOk.
     */
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kOptIn;
    }

    bool maintenanceOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return false;
    }

    std::string help() const override {
        return "explain database reads and writes";
    }

    bool enableDiagnosticPrintingOnFailure() const final {
        return true;
    }

    class Invocation final : public MinimalInvocationBase {
    public:
        Invocation(OperationContext* opCtx, Command* cmd, const OpMsgRequest& opMsgRequest)
            : MinimalInvocationBase(opCtx, cmd, opMsgRequest),
              _verbosity(request().getVerbosity()) {
            CommandHelpers::uassertNoDocumentSequences(definition()->getName(), opMsgRequest);

            auto explainedObj = explain_cmd_helpers::makeExplainedObjForMongos(
                unparsedRequest().body, request().getCommandParameter());

            auto innerCommand =
                explain_cmd_helpers::makeExplainedCommand(opCtx,
                                                          opMsgRequest,
                                                          request().getDbName(),
                                                          explainedObj,
                                                          _verbosity,
                                                          request().getSerializationContext());
            _innerRequest = std::move(innerCommand.innerRequest);
            _innerInvocation = std::move(innerCommand.innerInvocation);

            // Fold a maxTimeMS nested inside the explained command into this command's own
            // maxTimeMS so the standard deadline machinery enforces it like a top-level maxTimeMS.
            auto& explainArgs = request().getGenericArguments();
            explainArgs.setMaxTimeMS(explain_cmd_helpers::resolveMaxTimeMS(
                explainArgs.getMaxTimeMS(),
                _innerInvocation->getGenericArguments().getMaxTimeMS()));

            // The explained command's readConcern takes precedence: semantically it is the read
            // concern of the operation being explained, the analog of a normal command's top-level
            // readConcern. Promote it onto this explain command's generic arguments so the standard
            // machinery sources it onto the opCtx.
            if (const auto& innerReadConcern =
                    _innerInvocation->getGenericArguments().getReadConcern()) {
                explainArgs.setReadConcern(*innerReadConcern);
            }
        }

        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                     bool isImplicitDefault) const override {
            static const Status kDefaultReadConcernNotPermitted{
                ErrorCodes::InvalidOptions,
                "Explain does not permit default readConcern to be applied."};
            return {Status::OK(), {kDefaultReadConcernNotPermitted}};
        }

        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* result) override {
            ON_BLOCK_EXIT([&] {
                aggregation_request_helper::restoreExplainOpDescription(opCtx,
                                                                        unparsedRequest().body);
            });
            _innerInvocation->explain(opCtx, _verbosity, result);
        }

    private:
        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        const CommandInvocation* inner() const override {
            return _innerInvocation.get();
        }

        /**
         * You are authorized to run an explain if you are authorized to run
         * the command that you are explaining. The auth check is performed recursively
         * on the nested command.
         */
        void doCheckAuthorization(OperationContext* opCtx) const override {
            _innerInvocation->checkAuthorization(opCtx, *_innerRequest);
        }

        ExplainOptions::Verbosity _verbosity;
        std::unique_ptr<OpMsgRequest>
            _innerRequest;  // Lifespan must enclose that of _innerInvocation.
        std::unique_ptr<CommandInvocation> _innerInvocation;
    };
};
MONGO_REGISTER_COMMAND(ClusterExplainCmd).forRouter();

}  // namespace
}  // namespace mongo
