/**
 *    Copyright (C) 2012-2016 MongoDB Inc.
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

#include "mongo/bson/util/bson_extract.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/log_process_details.h"
#include "mongo/util/log.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/ramlog.h"
#include "mongo/util/version.h"

#include <sstream>
#include <string>
#include <vector>

namespace mongo {
namespace {

using std::string;
using std::stringstream;
using std::vector;

class CmdBuildInfo : public BasicCommand {
public:
    CmdBuildInfo() : BasicCommand("buildInfo", "buildinfo") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    virtual bool adminOnly() const {
        return false;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {}  // No auth required
    std::string help() const override {
        return "get version #, etc.\n"
               "{ buildinfo:1 }";
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& jsobj,
             BSONObjBuilder& result) {
        VersionInfoInterface::instance().appendBuildInfo(&result);
        appendStorageEngineList(&result);
        return true;
    }

} cmdBuildInfo;

class PingCommand : public BasicCommand {
public:
    PingCommand() : BasicCommand("ping") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    std::string help() const override {
        return "a way to check that the server is alive. responds immediately even if server is "
               "in a db lock.";
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual bool allowsAfterClusterTime(const BSONObj& cmdObj) const override {
        return false;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {}  // No auth required
    virtual bool requiresAuth() const override {
        return false;
    }
    virtual bool run(OperationContext* opCtx,
                     const string& badns,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        // IMPORTANT: Don't put anything in here that might lock db - including authentication
        return true;
    }
} pingCmd;

class LogRotateCmd : public BasicCommand {
public:
    LogRotateCmd() : BasicCommand("logRotate") {}
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    bool adminOnly() const override {
        return true;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
        ActionSet actions;
        actions.addAction(ActionType::logRotate);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }
    virtual bool run(OperationContext* opCtx,
                     const string& ns,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        bool didRotate = rotateLogs(serverGlobalParams.logRenameOnRotate);
        if (didRotate)
            logProcessDetailsForLogRotate(opCtx->getServiceContext());
        return didRotate;
    }

} logRotateCmd;

class ListCommandsCmd : public BasicCommand {
public:
    std::string help() const override {
        return "get a list of all db commands";
    }
    ListCommandsCmd() : BasicCommand("listCommands") {}
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    virtual bool adminOnly() const {
        return false;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {}  // No auth required
    virtual bool run(OperationContext* opCtx,
                     const string& ns,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        // sort the commands before building the result BSON
        std::vector<Command*> commands;
        for (const auto command : globalCommandRegistry()->allCommands()) {
            // don't show oldnames
            if (command.first == command.second->getName())
                commands.push_back(command.second);
        }
        std::sort(commands.begin(), commands.end(), [](Command* lhs, Command* rhs) {
            return (lhs->getName()) < (rhs->getName());
        });

        BSONObjBuilder b(result.subobjStart("commands"));
        for (const auto& c : commands) {
            BSONObjBuilder temp(b.subobjStart(c->getName()));
            temp.append("help", c->help());
            temp.append("slaveOk",
                        c->secondaryAllowed(opCtx->getServiceContext()) ==
                            Command::AllowedOnSecondary::kAlways);
            temp.append("adminOnly", c->adminOnly());
            // optionally indicates that the command can be forced to run on a slave/secondary
            if (c->secondaryAllowed(opCtx->getServiceContext()) ==
                Command::AllowedOnSecondary::kOptIn)
                temp.append("slaveOverrideOk", true);
            temp.done();
        }
        b.done();

        return 1;
    }

} listCommandsCmd;

class GetLogCmd : public ErrmsgCommandDeprecated {
public:
    GetLogCmd() : ErrmsgCommandDeprecated("getLog") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual bool adminOnly() const {
        return true;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
        ActionSet actions;
        actions.addAction(ActionType::getLog);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }
    std::string help() const override {
        return "{ getLog : '*' }  OR { getLog : 'global' }";
    }

    virtual bool errmsgRun(OperationContext* opCtx,
                           const string& dbname,
                           const BSONObj& cmdObj,
                           string& errmsg,
                           BSONObjBuilder& result) {
        BSONElement val = cmdObj.firstElement();
        if (val.type() != String) {
            return CommandHelpers::appendCommandStatus(
                result,
                Status(ErrorCodes::TypeMismatch,
                       str::stream() << "Argument to getLog must be of type String; found "
                                     << val.toString(false)
                                     << " of type "
                                     << typeName(val.type())));
        }

        string p = val.String();
        if (p == "*") {
            vector<string> names;
            RamLog::getNames(names);

            BSONArrayBuilder arr;
            for (unsigned i = 0; i < names.size(); i++) {
                arr.append(names[i]);
            }

            result.appendArray("names", arr.arr());
        } else {
            RamLog* ramlog = RamLog::getIfExists(p);
            if (!ramlog) {
                errmsg = str::stream() << "no RamLog named: " << p;
                return false;
            }
            RamLog::LineIterator rl(ramlog);

            result.appendNumber("totalLinesWritten", rl.getTotalLinesWritten());

            BSONArrayBuilder arr(result.subarrayStart("log"));
            while (rl.more())
                arr.append(rl.next());
            arr.done();
        }
        return true;
    }

} getLogCmd;

class ClearLogCmd : public BasicCommand {
public:
    ClearLogCmd() : BasicCommand("clearLog") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual bool adminOnly() const {
        return true;
    }
    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        // No access control needed since this command is a testing-only command that must be
        // enabled at the command line.
        return Status::OK();
    }
    std::string help() const override {
        return "{ clearLog : 'global' }";
    }

    virtual bool run(OperationContext* opCtx,
                     const string& dbname,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        std::string logName;
        Status status = bsonExtractStringField(cmdObj, "clearLog", &logName);
        if (!status.isOK()) {
            return CommandHelpers::appendCommandStatus(result, status);
        }

        if (logName != "global") {
            return CommandHelpers::appendCommandStatus(
                result, Status(ErrorCodes::InvalidOptions, "Only the 'global' log can be cleared"));
        }
        RamLog* ramlog = RamLog::getIfExists(logName);
        invariant(ramlog);
        ramlog->clear();
        return true;
    }
};

MONGO_INITIALIZER(RegisterClearLogCmd)(InitializerContext* context) {
    if (getTestCommandsEnabled()) {
        // Leaked intentionally: a Command registers itself when constructed.
        new ClearLogCmd();
    }
    return Status::OK();
}

class CmdGetCmdLineOpts : public BasicCommand {
public:
    CmdGetCmdLineOpts() : BasicCommand("getCmdLineOpts") {}
    std::string help() const override {
        return "get argv";
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual bool adminOnly() const {
        return true;
    }
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
        ActionSet actions;
        actions.addAction(ActionType::getCmdLineOpts);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }
    virtual bool run(OperationContext* opCtx,
                     const string&,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        result.append("argv", serverGlobalParams.argvArray);
        result.append("parsed", serverGlobalParams.parsedOpts);
        return true;
    }

} cmdGetCmdLineOpts;
}  // namespace
}  // namespace mongo
