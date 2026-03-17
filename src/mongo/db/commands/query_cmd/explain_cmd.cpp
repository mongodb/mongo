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

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/client.h"
#include "mongo/db/command_can_run_here.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/explain_cmd_helpers.h"
#include "mongo/db/commands/query_cmd/explain_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/service_context.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/str.h"

#include <memory>
#include <string>


namespace mongo {
namespace {

/**
 * The explain command is used to generate explain output for any read or write operation which has
 * a query component (e.g. find, count, update, remove, distinct, etc.).
 *
 * The explain command takes as its argument a nested object which specifies the command to
 * explain, and a verbosity indicator. For example:
 *
 *    {explain: {count: "coll", query: {foo: "bar"}}, verbosity: "executionStats"}
 *
 * This command like a dispatcher: it just retrieves a pointer to the nested command and invokes
 * its explain() implementation.
 */
class CmdExplain final : public ExplainCmdVersion1Gen<CmdExplain> {
public:
    using ExplainCmdVersion1Gen<CmdExplain>::ExplainCmdVersion1Gen;
    /**
     * Running an explain on a secondary requires explicitly setting slaveOk.
     */
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kOptIn;
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    bool maintenanceOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return false;
    }

    bool collectsResourceConsumptionMetrics() const override {
        return true;
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
            auto explainedObj = request().getCommandParameter();

            auto explainedCommand =
                explain_cmd_helpers::makeExplainedCommand(opCtx,
                                                          opMsgRequest,
                                                          request().getDbName(),
                                                          request().getCommandParameter(),
                                                          _verbosity,
                                                          request().getSerializationContext());
            _innerRequest = std::move(explainedCommand.innerRequest);
            _innerInvocation = std::move(explainedCommand.innerInvocation);
        }

        NamespaceString ns() const override {
            return _innerInvocation->ns();
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                     bool isImplicitDefault) const override {
            static const Status kDefaultReadConcernNotPermitted{
                ErrorCodes::InvalidOptions,
                "Explain does not permit default readConcern to be applied."};
            return {Status::OK(), {kDefaultReadConcernNotPermitted}};
        }

        bool isSubjectToIngressAdmissionControl() const override {
            return true;
        }

        /**
         * You are authorized to run an explain if you are authorized to run
         * the command that you are explaining. The auth check is performed recursively
         * on the nested command.
         */
        void doCheckAuthorization(OperationContext* opCtx) const override {
            _innerInvocation->checkAuthorization(opCtx, *_innerRequest);
        }

        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* result) override {
            // Explain is never allowed in multi-document transactions.
            const bool inMultiDocumentTransaction = false;
            uassert(50746,
                    "Explain's child command cannot run on this node. "
                    "Are you explaining a write command on a secondary?",
                    commandCanRunHere(opCtx,
                                      request().getDbName(),
                                      _innerInvocation->definition(),
                                      inMultiDocumentTransaction));
            ON_BLOCK_EXIT([&] {
                aggregation_request_helper::restoreExplainOpDescription(opCtx,
                                                                        unparsedRequest().body);
            });
            _innerInvocation->explain(opCtx, _verbosity, result);
        }

    private:
        ExplainOptions::Verbosity _verbosity;
        std::unique_ptr<OpMsgRequest>
            _innerRequest;  // Lifespan must enclose that of _innerInvocation.
        std::unique_ptr<CommandInvocation> _innerInvocation;
    };
};
MONGO_REGISTER_COMMAND(CmdExplain).forShard();

}  // namespace
}  // namespace mongo
