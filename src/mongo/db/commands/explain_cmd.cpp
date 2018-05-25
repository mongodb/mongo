/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/command_can_run_here.h"
#include "mongo/db/commands.h"
#include "mongo/db/query/explain.h"
#include "mongo/util/mongoutils/str.h"

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
class CmdExplain final : public Command {
public:
    CmdExplain() : Command("explain") {}

    std::unique_ptr<CommandInvocation> parse(OperationContext* opCtx,
                                             const OpMsgRequest& request) override;

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

private:
    class Invocation;
};

class CmdExplain::Invocation final : public CommandInvocation {
public:
    Invocation(const CmdExplain* explainCommand,
               const OpMsgRequest& request,
               ExplainOptions::Verbosity verbosity,
               std::unique_ptr<OpMsgRequest> innerRequest,
               std::unique_ptr<CommandInvocation> innerInvocation)
        : CommandInvocation(explainCommand),
          _outerRequest{&request},
          _dbName{_outerRequest->getDatabase().toString()},
          _verbosity{std::move(verbosity)},
          _innerRequest{std::move(innerRequest)},
          _innerInvocation{std::move(innerInvocation)} {}

    void run(OperationContext* opCtx, CommandReplyBuilder* result) override {
        uassert(50746,
                "Explain's child command cannot run on this node. "
                "Are you explaining a write command on a secondary?",
                commandCanRunHere(opCtx, _dbName, _innerInvocation->definition()));
        try {
            BSONObjBuilder bob = result->getBodyBuilder();
            _innerInvocation->explain(opCtx, _verbosity, &bob);
        } catch (const ExceptionFor<ErrorCodes::Unauthorized>&) {
            CommandHelpers::logAuthViolation(opCtx, this, *_outerRequest, ErrorCodes::Unauthorized);
            throw;
        }
    }

    void explain(OperationContext* opCtx,
                 ExplainOptions::Verbosity verbosity,
                 BSONObjBuilder* result) override {
        uasserted(ErrorCodes::IllegalOperation, "Explain cannot explain itself.");
    }

    NamespaceString ns() const override {
        return _innerInvocation->ns();
    }

    bool supportsWriteConcern() const override {
        return false;
    }

    /**
     * You are authorized to run an explain if you are authorized to run
     * the command that you are explaining. The auth check is performed recursively
     * on the nested command.
     */
    void doCheckAuthorization(OperationContext* opCtx) const override {
        _innerInvocation->checkAuthorization(opCtx, *_innerRequest);
    }

private:
    const CmdExplain* command() const {
        return static_cast<const CmdExplain*>(definition());
    }

    const OpMsgRequest* _outerRequest;
    const std::string _dbName;
    const NamespaceString _ns;
    ExplainOptions::Verbosity _verbosity;
    std::unique_ptr<OpMsgRequest> _innerRequest;  // Lifespan must enclose that of _innerInvocation.
    std::unique_ptr<CommandInvocation> _innerInvocation;
};

std::unique_ptr<CommandInvocation> CmdExplain::parse(OperationContext* opCtx,
                                                     const OpMsgRequest& request) {
    CommandHelpers::uassertNoDocumentSequences(getName(), request);
    std::string dbname = request.getDatabase().toString();
    const BSONObj& cmdObj = request.body;
    ExplainOptions::Verbosity verbosity = uassertStatusOK(ExplainOptions::parseCmdBSON(cmdObj));
    uassert(ErrorCodes::BadValue,
            "explain command requires a nested object",
            cmdObj.firstElement().type() == Object);
    auto explainedObj = cmdObj.firstElement().Obj();
    if (auto innerDb = explainedObj["$db"]) {
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Mismatched $db in explain command. Expected " << dbname
                              << " but got "
                              << innerDb.checkAndGetStringData(),
                innerDb.checkAndGetStringData() == dbname);
    }
    auto explainedCommand = CommandHelpers::findCommand(explainedObj.firstElementFieldName());
    uassert(ErrorCodes::CommandNotFound,
            str::stream() << "Explain failed due to unknown command: "
                          << explainedObj.firstElementFieldName(),
            explainedCommand);
    auto innerRequest =
        stdx::make_unique<OpMsgRequest>(OpMsgRequest::fromDBAndBody(dbname, explainedObj));
    auto innerInvocation = explainedCommand->parse(opCtx, *innerRequest);
    return stdx::make_unique<Invocation>(
        this, request, std::move(verbosity), std::move(innerRequest), std::move(innerInvocation));
}

CmdExplain cmdExplain;

}  // namespace
}  // namespace mongo
