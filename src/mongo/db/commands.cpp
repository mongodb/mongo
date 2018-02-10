/**
 *    Copyright (C) 2009-2016 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/commands.h"

#include <string>
#include <vector>

#include "mongo/bson/mutable/document.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/server_parameters.h"
#include "mongo/rpc/write_concern_error_detail.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/invariant.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using logger::LogComponent;

namespace {

ExportedServerParameter<bool, ServerParameterType::kStartupOnly> testCommandsParameter(
    ServerParameterSet::getGlobal(), "enableTestCommands", &Command::testCommandsEnabled);

const char kWriteConcernField[] = "writeConcern";
const WriteConcernOptions kMajorityWriteConcern(
    WriteConcernOptions::kMajority,
    // Note: Even though we're setting UNSET here, kMajority implies JOURNAL if journaling is
    // supported by the mongod.
    WriteConcernOptions::SyncMode::UNSET,
    Seconds(60));

}  // namespace


//////////////////////////////////////////////////////////////
// CommandHelpers

BSONObj CommandHelpers::runCommandDirectly(OperationContext* opCtx, const OpMsgRequest& request) {
    auto command = globalCommandRegistry()->findCommand(request.getCommandName());
    invariant(command);
    BSONObjBuilder out;
    try {
        bool ok = command->publicRun(opCtx, request, out);
        appendCommandStatus(out, ok);
    } catch (const StaleConfigException&) {
        // These exceptions are intended to be handled at a higher level.
        throw;
    } catch (const DBException& ex) {
        out.resetToEmpty();
        appendCommandStatus(out, ex.toStatus());
    }
    return out.obj();
}

std::string CommandHelpers::parseNsFullyQualified(StringData dbname, const BSONObj& cmdObj) {
    BSONElement first = cmdObj.firstElement();
    uassert(ErrorCodes::BadValue,
            str::stream() << "collection name has invalid type " << typeName(first.type()),
            first.canonicalType() == canonicalizeBSONType(mongo::String));
    const NamespaceString nss(first.valueStringData());
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "Invalid namespace specified '" << nss.ns() << "'",
            nss.isValid());
    return nss.ns();
}

NamespaceString CommandHelpers::parseNsCollectionRequired(StringData dbname,
                                                          const BSONObj& cmdObj) {
    // Accepts both BSON String and Symbol for collection name per SERVER-16260
    // TODO(kangas) remove Symbol support in MongoDB 3.0 after Ruby driver audit
    BSONElement first = cmdObj.firstElement();
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "collection name has invalid type " << typeName(first.type()),
            first.canonicalType() == canonicalizeBSONType(mongo::String));
    const NamespaceString nss(dbname, first.valueStringData());
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "Invalid namespace specified '" << nss.ns() << "'",
            nss.isValid());
    return nss;
}

NamespaceString CommandHelpers::parseNsOrUUID(OperationContext* opCtx,
                                              StringData dbname,
                                              const BSONObj& cmdObj) {
    BSONElement first = cmdObj.firstElement();
    if (first.type() == BinData && first.binDataType() == BinDataType::newUUID) {
        UUIDCatalog& catalog = UUIDCatalog::get(opCtx);
        UUID uuid = uassertStatusOK(UUID::parse(first));
        NamespaceString nss = catalog.lookupNSSByUUID(uuid);
        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "UUID " << uuid << " specified in "
                              << cmdObj.firstElement().fieldNameStringData()
                              << " command not found in "
                              << dbname
                              << " database",
                nss.isValid() && nss.db() == dbname);

        return nss;
    } else {
        // Ensure collection identifier is not a Command
        const NamespaceString nss(parseNsCollectionRequired(dbname, cmdObj));
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid collection name specified '" << nss.ns() << "'",
                nss.isNormal());
        return nss;
    }
}

Command* CommandHelpers::findCommand(StringData name) {
    return globalCommandRegistry()->findCommand(name);
}

bool CommandHelpers::appendCommandStatus(BSONObjBuilder& result, const Status& status) {
    appendCommandStatus(result, status.isOK(), status.reason());
    BSONObj tmp = result.asTempObj();
    if (!status.isOK() && !tmp.hasField("code")) {
        result.append("code", status.code());
        result.append("codeName", ErrorCodes::errorString(status.code()));
    }
    if (auto extraInfo = status.extraInfo()) {
        extraInfo->serialize(&result);
    }
    return status.isOK();
}

void CommandHelpers::appendCommandStatus(BSONObjBuilder& result,
                                         bool ok,
                                         const std::string& errmsg) {
    BSONObj tmp = result.asTempObj();
    bool have_ok = tmp.hasField("ok");
    bool need_errmsg = !ok && !tmp.hasField("errmsg");

    if (!have_ok)
        result.append("ok", ok ? 1.0 : 0.0);

    if (need_errmsg) {
        result.append("errmsg", errmsg);
    }
}

void CommandHelpers::appendCommandWCStatus(BSONObjBuilder& result,
                                           const Status& awaitReplicationStatus,
                                           const WriteConcernResult& wcResult) {
    if (!awaitReplicationStatus.isOK() && !result.hasField("writeConcernError")) {
        WriteConcernErrorDetail wcError;
        wcError.setStatus(awaitReplicationStatus);
        if (wcResult.wTimedOut) {
            wcError.setErrInfo(BSON("wtimeout" << true));
        }
        result.append("writeConcernError", wcError.toBSON());
    }
}

BSONObj CommandHelpers::appendPassthroughFields(const BSONObj& cmdObjWithPassthroughFields,
                                                const BSONObj& request) {
    BSONObjBuilder b;
    b.appendElements(request);
    for (const auto& elem : filterCommandRequestForPassthrough(cmdObjWithPassthroughFields)) {
        const auto name = elem.fieldNameStringData();
        if (isGenericArgument(name) && !request.hasField(name)) {
            b.append(elem);
        }
    }
    return b.obj();
}

BSONObj CommandHelpers::appendMajorityWriteConcern(const BSONObj& cmdObj) {
    WriteConcernOptions newWC = kMajorityWriteConcern;

    if (cmdObj.hasField(kWriteConcernField)) {
        auto wc = cmdObj.getField(kWriteConcernField);
        // The command has a writeConcern field and it's majority, so we can
        // return it as-is.
        if (wc["w"].ok() && wc["w"].str() == "majority") {
            return cmdObj;
        }

        if (wc["wtimeout"].ok()) {
            // They set a timeout, but aren't using majority WC. We want to use their
            // timeout along with majority WC.
            newWC = WriteConcernOptions(WriteConcernOptions::kMajority,
                                        WriteConcernOptions::SyncMode::UNSET,
                                        wc["wtimeout"].Number());
        }
    }

    // Append all original fields except the writeConcern field to the new command.
    BSONObjBuilder cmdObjWithWriteConcern;
    for (const auto& elem : cmdObj) {
        const auto name = elem.fieldNameStringData();
        if (name != "writeConcern" && !cmdObjWithWriteConcern.hasField(name)) {
            cmdObjWithWriteConcern.append(elem);
        }
    }

    // Finally, add the new write concern.
    cmdObjWithWriteConcern.append(kWriteConcernField, newWC.toBSON());
    return cmdObjWithWriteConcern.obj();
}

namespace {
const stdx::unordered_set<std::string> userManagementCommands{"createUser",
                                                              "updateUser",
                                                              "dropUser",
                                                              "dropAllUsersFromDatabase",
                                                              "grantRolesToUser",
                                                              "revokeRolesFromUser",
                                                              "createRole",
                                                              "updateRole",
                                                              "dropRole",
                                                              "dropAllRolesFromDatabase",
                                                              "grantPrivilegesToRole",
                                                              "revokePrivilegesFromRole",
                                                              "grantRolesToRole",
                                                              "revokeRolesFromRole",
                                                              "_mergeAuthzCollections"};
}  // namespace

bool CommandHelpers::isUserManagementCommand(const std::string& name) {
    return userManagementCommands.count(name);
}

BSONObj CommandHelpers::filterCommandRequestForPassthrough(const BSONObj& cmdObj) {
    BSONObjIterator cmdIter(cmdObj);
    BSONObjBuilder bob;
    filterCommandRequestForPassthrough(&cmdIter, &bob);
    return bob.obj();
}

void CommandHelpers::filterCommandRequestForPassthrough(BSONObjIterator* cmdIter,
                                                        BSONObjBuilder* requestBuilder) {
    while (cmdIter->more()) {
        auto elem = cmdIter->next();
        const auto name = elem.fieldNameStringData();
        if (name == "$readPreference") {
            BSONObjBuilder(requestBuilder->subobjStart("$queryOptions")).append(elem);
        } else if (!isGenericArgument(name) ||  //
                   name == "$queryOptions" ||   //
                   name == "maxTimeMS" ||       //
                   name == "readConcern" ||     //
                   name == "writeConcern" ||    //
                   name == "lsid" ||            //
                   name == "txnNumber") {
            // This is the whitelist of generic arguments that commands can be trusted to blindly
            // forward to the shards.
            requestBuilder->append(elem);
        }
    }
}

void CommandHelpers::filterCommandReplyForPassthrough(const BSONObj& cmdObj,
                                                      BSONObjBuilder* output) {
    for (auto elem : cmdObj) {
        const auto name = elem.fieldNameStringData();
        if (name == "$configServerState" ||  //
            name == "$gleStats" ||           //
            name == "$clusterTime" ||        //
            name == "$oplogQueryData" ||     //
            name == "$replData" ||           //
            name == "operationTime") {
            continue;
        }
        output->append(elem);
    }
}

BSONObj CommandHelpers::filterCommandReplyForPassthrough(const BSONObj& cmdObj) {
    BSONObjBuilder bob;
    filterCommandReplyForPassthrough(cmdObj, &bob);
    return bob.obj();
}

bool CommandHelpers::isHelpRequest(const BSONElement& helpElem) {
    return !helpElem.eoo() && helpElem.trueValue();
}

constexpr StringData CommandHelpers::kHelpFieldName;

//////////////////////////////////////////////////////////////
// Command

Command::~Command() = default;

std::string Command::parseNs(const std::string& dbname, const BSONObj& cmdObj) const {
    BSONElement first = cmdObj.firstElement();
    if (first.type() != mongo::String)
        return dbname;

    return str::stream() << dbname << '.' << cmdObj.firstElement().valueStringData();
}

ResourcePattern Command::parseResourcePattern(const std::string& dbname,
                                              const BSONObj& cmdObj) const {
    const std::string ns = parseNs(dbname, cmdObj);
    if (!NamespaceString::validCollectionComponent(ns)) {
        return ResourcePattern::forDatabaseName(ns);
    }
    return ResourcePattern::forExactNamespace(NamespaceString(ns));
}

Command::Command(StringData name, StringData oldName)
    : _name(name.toString()),
      _commandsExecutedMetric("commands." + _name + ".total", &_commandsExecuted),
      _commandsFailedMetric("commands." + _name + ".failed", &_commandsFailed) {
    globalCommandRegistry()->registerCommand(this, name, oldName);
}

Status Command::explain(OperationContext* opCtx,
                        const std::string& dbname,
                        const BSONObj& cmdObj,
                        ExplainOptions::Verbosity verbosity,
                        BSONObjBuilder* out) const {
    return {ErrorCodes::IllegalOperation, str::stream() << "Cannot explain cmd: " << getName()};
}

Status BasicCommand::checkAuthForRequest(OperationContext* opCtx,
                                         const OpMsgRequest& request) const {
    uassertNoDocumentSequences(request);
    return checkAuthForOperation(opCtx, request.getDatabase().toString(), request.body);
}

Status BasicCommand::checkAuthForOperation(OperationContext* opCtx,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) const {
    return checkAuthForCommand(opCtx->getClient(), dbname, cmdObj);
}

Status BasicCommand::checkAuthForCommand(Client* client,
                                         const std::string& dbname,
                                         const BSONObj& cmdObj) const {
    std::vector<Privilege> privileges;
    this->addRequiredPrivileges(dbname, cmdObj, &privileges);
    if (AuthorizationSession::get(client)->isAuthorizedForPrivileges(privileges))
        return Status::OK();
    return Status(ErrorCodes::Unauthorized, "unauthorized");
}

static Status _checkAuthorizationImpl(Command* c,
                                      OperationContext* opCtx,
                                      const OpMsgRequest& request) {
    namespace mmb = mutablebson;
    auto client = opCtx->getClient();
    auto dbname = request.getDatabase();
    if (c->adminOnly() && dbname != "admin") {
        return Status(ErrorCodes::Unauthorized,
                      str::stream() << c->getName()
                                    << " may only be run against the admin database.");
    }
    if (AuthorizationSession::get(client)->getAuthorizationManager().isAuthEnabled()) {
        Status status = c->checkAuthForRequest(opCtx, request);
        if (status == ErrorCodes::Unauthorized) {
            mmb::Document cmdToLog(request.body, mmb::Document::kInPlaceDisabled);
            c->redactForLogging(&cmdToLog);
            return Status(ErrorCodes::Unauthorized,
                          str::stream() << "not authorized on " << dbname << " to execute command "
                                        << cmdToLog.toString());
        }
        if (!status.isOK()) {
            return status;
        }
    } else if (c->adminOnly() && c->localHostOnlyIfNoAuth() &&
               !client->getIsLocalHostConnection()) {
        return Status(ErrorCodes::Unauthorized,
                      str::stream() << c->getName()
                                    << " must run from localhost when running db without auth");
    }
    return Status::OK();
}

namespace {
// A facade presenting CommandDefinition as an audit::CommandInterface.
class CommandAuditHook : public audit::CommandInterface {
public:
    explicit CommandAuditHook(Command* command) : _command(command) {}

    void redactForLogging(mutablebson::Document* cmdObj) const final {
        _command->redactForLogging(cmdObj);
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const final {
        return _command->parseNs(dbname, cmdObj);
    }

private:
    Command* _command;
};
}  // namespace

Status Command::checkAuthorization(Command* c,
                                   OperationContext* opCtx,
                                   const OpMsgRequest& request) {
    Status status = _checkAuthorizationImpl(c, opCtx, request);
    if (!status.isOK()) {
        log(LogComponent::kAccessControl) << status;
    }
    CommandAuditHook hook(c);
    audit::logCommandAuthzCheck(opCtx->getClient(), request, &hook, status.code());
    return status;
}

bool Command::publicRun(OperationContext* opCtx,
                        const OpMsgRequest& request,
                        BSONObjBuilder& result) {
    try {
        return enhancedRun(opCtx, request, result);
    } catch (const DBException& e) {
        if (e.code() == ErrorCodes::Unauthorized) {
            CommandAuditHook hook(this);
            audit::logCommandAuthzCheck(
                opCtx->getClient(), request, &hook, ErrorCodes::Unauthorized);
        }
        throw;
    }
}

void Command::generateHelpResponse(OperationContext* opCtx,
                                   rpc::ReplyBuilderInterface* replyBuilder,
                                   const Command& command) {
    BSONObjBuilder helpBuilder;
    helpBuilder.append("help",
                       str::stream() << "help for: " << command.getName() << " " << command.help());
    replyBuilder->setCommandReply(helpBuilder.obj());
    replyBuilder->setMetadata(rpc::makeEmptyMetadata());
}

void BasicCommand::uassertNoDocumentSequences(const OpMsgRequest& request) const {
    uassert(40472,
            str::stream() << "The " << getName() << " command does not support document sequences.",
            request.sequences.empty());
}

bool BasicCommand::enhancedRun(OperationContext* opCtx,
                               const OpMsgRequest& request,
                               BSONObjBuilder& result) {
    uassertNoDocumentSequences(request);
    return run(opCtx, request.getDatabase().toString(), request.body, result);
}

bool ErrmsgCommandDeprecated::run(OperationContext* opCtx,
                                  const std::string& db,
                                  const BSONObj& cmdObj,
                                  BSONObjBuilder& result) {
    std::string errmsg;
    auto ok = errmsgRun(opCtx, db, cmdObj, errmsg, result);
    if (!errmsg.empty()) {
        CommandHelpers::appendCommandStatus(result, ok, errmsg);
    }
    return ok;
}

//////////////////////////////////////////////////////////////
// CommandRegistry

void CommandRegistry::registerCommand(Command* command, StringData name, StringData oldName) {
    for (StringData key : {name, oldName}) {
        if (key.empty()) {
            continue;
        }
        auto hashedKey = CommandMap::HashedKey(key);
        auto iter = _commands.find(hashedKey);
        invariant(iter == _commands.end(), str::stream() << "command name collision: " << key);
        _commands[hashedKey] = command;
    }
}

Command* CommandRegistry::findCommand(StringData name) const {
    auto it = _commands.find(name);
    if (it == _commands.end())
        return nullptr;
    return it->second;
}

CommandRegistry* globalCommandRegistry() {
    static auto reg = new CommandRegistry();
    return reg;
}

}  // namespace mongo
