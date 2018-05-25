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

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/command_generic_argument.h"
#include "mongo/db/commands.h"
#include "mongo/db/query/explain.h"
#include "mongo/s/query/cluster_find.h"

namespace mongo {
namespace {

/**
 * Implements the explain command on mongos.
 *
 * "Old-style" explains (i.e. queries which have the $explain flag set), do not run
 * through this path. Such explains will be supported for backwards compatibility,
 * and must succeed in multiversion clusters.
 *
 * "New-style" explains use the explain command. When the explain command is routed
 * through mongos, it is forwarded to all relevant shards. If *any* shard does not
 * support a new-style explain, then the entire explain will fail (i.e. new-style
 * explains cannot be used in multiversion clusters).
 */

class ClusterExplainCmd final : public Command {
public:
    ClusterExplainCmd() : Command("explain") {}

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

class ClusterExplainCmd::Invocation final : public CommandInvocation {
public:
    Invocation(const ClusterExplainCmd* explainCommand,
               const OpMsgRequest& request,
               ExplainOptions::Verbosity verbosity,
               std::unique_ptr<OpMsgRequest> innerRequest,
               std::unique_ptr<CommandInvocation> innerInvocation)
        : CommandInvocation(explainCommand),
          _outerRequest{&request},
          _dbName{_outerRequest->getDatabase().toString()},
          _ns{CommandHelpers::parseNsFromCommand(_dbName, _outerRequest->body)},
          _verbosity{std::move(verbosity)},
          _innerRequest{std::move(innerRequest)},
          _innerInvocation{std::move(innerInvocation)} {}

private:
    void run(OperationContext* opCtx, CommandReplyBuilder* result) override {
        try {
            auto bob = result->getBodyBuilder();
            _innerInvocation->explain(opCtx, _verbosity, &bob);
        } catch (const ExceptionFor<ErrorCodes::Unauthorized>&) {
            CommandHelpers::auditLogAuthEvent(
                opCtx, this, *_outerRequest, ErrorCodes::Unauthorized);
            throw;
        }
    }

    void explain(OperationContext* opCtx,
                 ExplainOptions::Verbosity verbosity,
                 BSONObjBuilder* result) override {
        uasserted(ErrorCodes::IllegalOperation, "Explain cannot explain itself.");
    }

    NamespaceString ns() const override {
        return _ns;
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

    const ClusterExplainCmd* command() const {
        return static_cast<const ClusterExplainCmd*>(definition());
    }

    const OpMsgRequest* _outerRequest;
    const std::string _dbName;
    NamespaceString _ns;
    ExplainOptions::Verbosity _verbosity;
    std::unique_ptr<OpMsgRequest> _innerRequest;  // Lifespan must enclose that of _innerInvocation.
    std::unique_ptr<CommandInvocation> _innerInvocation;
};

/**
 * Synthesize a BSONObj for the command to be explained.
 * To do this we must copy generic arguments from the enclosing explain command.
 */
BSONObj makeExplainedObj(const BSONObj& outerObj, StringData dbName) {
    const auto& first = outerObj.firstElement();
    uassert(
        ErrorCodes::BadValue, "explain command requires a nested object", first.type() == Object);
    const BSONObj& innerObj = first.Obj();

    if (auto innerDb = innerObj["$db"]) {
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Mismatched $db in explain command. Expected " << dbName
                              << " but got "
                              << innerDb.checkAndGetStringData(),
                innerDb.checkAndGetStringData() == dbName);
    }

    BSONObjBuilder bob;
    bob.appendElements(innerObj);
    for (auto outerElem : outerObj) {
        // If the argument is in both the inner and outer command, we currently let the
        // inner version take precedence.
        const auto name = outerElem.fieldNameStringData();
        if (isGenericArgument(name) && !innerObj.hasField(name)) {
            bob.append(outerElem);
        }
    }
    return bob.obj();
}

std::unique_ptr<CommandInvocation> ClusterExplainCmd::parse(OperationContext* opCtx,
                                                            const OpMsgRequest& request) {
    CommandHelpers::uassertNoDocumentSequences(getName(), request);
    std::string dbName = request.getDatabase().toString();
    const BSONObj& cmdObj = request.body;
    ExplainOptions::Verbosity verbosity = uassertStatusOK(ExplainOptions::parseCmdBSON(cmdObj));

    // This is the nested command which we are explaining. We need to propagate generic
    // arguments into the inner command since it is what is passed to the virtual
    // CommandInvocation::explain() method.
    const BSONObj explainedObj = makeExplainedObj(cmdObj, dbName);
    const std::string cmdName = explainedObj.firstElementFieldName();
    auto explainedCommand = CommandHelpers::findCommand(cmdName);
    uassert(ErrorCodes::CommandNotFound,
            str::stream() << "Explain failed due to unknown command: " << cmdName,
            explainedCommand);
    auto innerRequest = std::make_unique<OpMsgRequest>(OpMsg{explainedObj});
    auto innerInvocation = explainedCommand->parse(opCtx, *innerRequest);
    return stdx::make_unique<Invocation>(
        this, request, std::move(verbosity), std::move(innerRequest), std::move(innerInvocation));
}

ClusterExplainCmd cmdExplainCluster;

}  // namespace
}  // namespace mongo
