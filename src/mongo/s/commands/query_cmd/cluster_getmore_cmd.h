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

#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/getmore_command_gen.h"
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
 * Implements the getMore command on mongos. Retrieves more from an existing mongos cursor
 * corresponding to the cursor id passed from the application. In order to generate these results,
 * may issue getMore commands to remote nodes in one or more shards.
 */
template <typename Impl>
class ClusterGetMoreCmdBase final : public Command {
public:
    ClusterGetMoreCmdBase() : Command(Impl::kName) {}

    const std::set<std::string>& apiVersions() const override {
        return Impl::getApiVersions();
    }

    std::unique_ptr<CommandInvocation> parse(OperationContext* opCtx,
                                             const OpMsgRequest& opMsgRequest) override {
        return std::make_unique<Invocation>(this, opMsgRequest);
    }

    bool allowedInTransactions() const final {
        return true;
    }

    bool enableDiagnosticPrintingOnFailure() const final {
        return true;
    }

    class Invocation final : public CommandInvocation {
    public:
        Invocation(Command* cmd, const OpMsgRequest& request)
            : CommandInvocation(cmd),
              _cmd(GetMoreCommandRequest::parse(request.body, IDLParserContext{Impl::kName})) {}

    private:
        NamespaceString ns() const override {
            return NamespaceStringUtil::deserialize(_cmd.getDbName(), _cmd.getCollection());
        }

        const DatabaseName& db() const override {
            return _cmd.getDbName();
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
                opCtx, ns(), _cmd.getCommandParameter(), _cmd.getTerm().is_initialized());
        }

        const GenericArguments& getGenericArguments() const override {
            return _cmd.getGenericArguments();
        }

        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* reply) override {
            // Counted as a getMore, not as a command.
            serviceOpCounters(opCtx).gotGetMore();

            Impl::checkCanRunHere(opCtx);

            auto bob = reply->getBodyBuilder();
            auto response = uassertStatusOK(ClusterFind::runGetMore(opCtx, _cmd));
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

        void validateResult(const BSONObj& replyObj) {
            CursorGetMoreReply::parse(replyObj.removeField("ok"),
                                      IDLParserContext{"CursorGetMoreReply"});
        }

        const GetMoreCommandRequest _cmd;
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
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

    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kRead;
    }

    std::string help() const override {
        return "retrieve more documents for a cursor id";
    }

    LogicalOp getLogicalOp() const override {
        return LogicalOp::opGetMore;
    }
};


}  // namespace mongo
