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

#include "mongo/db/command_can_run_here.h"
#include "mongo/db/commands.h"
#include "mongo/db/explain_gen.h"
#include "mongo/db/query/explain.h"
#include "mongo/util/str.h"

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

    const std::set<std::string>& apiVersions() const {
        return kApiVersions1;
    }

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

    bool collectsResourceConsumptionMetrics() const override {
        return true;
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

    void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* result) override {
        // Explain is never allowed in multi-document transactions.
        const bool inMultiDocumentTransaction = false;
        uassert(50746,
                "Explain's child command cannot run on this node. "
                "Are you explaining a write command on a secondary?",
                commandCanRunHere(
                    opCtx, _dbName, _innerInvocation->definition(), inMultiDocumentTransaction));
        _innerInvocation->explain(opCtx, _verbosity, result);
    }

    void explain(OperationContext* opCtx,
                 ExplainOptions::Verbosity verbosity,
                 rpc::ReplyBuilderInterface* result) override {
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

    // To enforce API versioning
    auto cmdObj = ExplainCommandRequest::parse(
        IDLParserContext(ExplainCommandRequest::kCommandName,
                         APIParameters::get(opCtx).getAPIStrict().value_or(false)),
        request.body);
    std::string dbname = cmdObj.getDbName().toString();
    ExplainOptions::Verbosity verbosity = cmdObj.getVerbosity();
    auto explainedObj = cmdObj.getCommandParameter();

    // Extract 'comment' field from the 'explainedObj' only if there is no top-level comment.
    auto commentField = explainedObj["comment"];
    if (!opCtx->getComment() && commentField) {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        opCtx->setComment(commentField.wrap());
    }

    if (auto innerDb = explainedObj["$db"]) {
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Mismatched $db in explain command. Expected " << dbname
                              << " but got " << innerDb.checkAndGetStringData(),
                innerDb.checkAndGetStringData() == dbname);
    }
    auto explainedCommand = CommandHelpers::findCommand(explainedObj.firstElementFieldName());
    uassert(ErrorCodes::CommandNotFound,
            str::stream() << "Explain failed due to unknown command: "
                          << explainedObj.firstElementFieldName(),
            explainedCommand);
    auto innerRequest =
        std::make_unique<OpMsgRequest>(OpMsgRequest::fromDBAndBody(dbname, explainedObj));
    auto innerInvocation = explainedCommand->parseForExplain(opCtx, *innerRequest, verbosity);
    return std::make_unique<Invocation>(
        this, request, std::move(verbosity), std::move(innerRequest), std::move(innerInvocation));
}

CmdExplain cmdExplain;

}  // namespace
}  // namespace mongo
