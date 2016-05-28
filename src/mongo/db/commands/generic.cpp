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

#include <time.h>

#include "mongo/bson/util/builder.h"
#include "mongo/client/dbclient_rs.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/background.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/shutdown.h"
#include "mongo/db/db.h"
#include "mongo/db/instance.h"
#include "mongo/db/introspect.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/log_process_details.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/md5.hpp"
#include "mongo/util/ntservice.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/ramlog.h"
#include "mongo/util/version.h"

namespace mongo {

using std::endl;
using std::string;
using std::stringstream;
using std::vector;

class CmdBuildInfo : public Command {
public:
    CmdBuildInfo() : Command("buildInfo", true, "buildinfo") {}
    virtual bool slaveOk() const {
        return true;
    }
    virtual bool adminOnly() const {
        return false;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {}  // No auth required
    virtual void help(stringstream& help) const {
        help << "get version #, etc.\n";
        help << "{ buildinfo:1 }";
    }

    bool run(OperationContext* txn,
             const std::string& dbname,
             BSONObj& jsobj,
             int,  // options
             std::string& errmsg,
             BSONObjBuilder& result) {
        appendBuildInfo(result);
        appendStorageEngineList(&result);
        return true;
    }

} cmdBuildInfo;


class PingCommand : public Command {
public:
    PingCommand() : Command("ping") {}
    virtual bool slaveOk() const {
        return true;
    }
    virtual void help(stringstream& help) const {
        help << "a way to check that the server is alive. responds immediately even if server is "
                "in a db lock.";
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {}  // No auth required
    virtual bool run(OperationContext* txn,
                     const string& badns,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        // IMPORTANT: Don't put anything in here that might lock db - including authentication
        return true;
    }
} pingCmd;

class FeaturesCmd : public Command {
public:
    FeaturesCmd() : Command("features", true) {}
    void help(stringstream& h) const {
        h << "return build level feature settings";
    }
    virtual bool slaveOk() const {
        return true;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {}  // No auth required
    virtual bool run(OperationContext* txn,
                     const string& ns,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        if (globalScriptEngine) {
            BSONObjBuilder bb(result.subobjStart("js"));
            result.append("utf8", globalScriptEngine->utf8Ok());
            bb.done();
        }
        if (cmdObj["oidReset"].trueValue()) {
            result.append("oidMachineOld", OID::getMachineId());
            OID::regenMachineId();
        }
        result.append("oidMachine", OID::getMachineId());
        return true;
    }

} featuresCmd;

class HostInfoCmd : public Command {
public:
    HostInfoCmd() : Command("hostInfo", true) {}
    virtual bool slaveOk() const {
        return true;
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    virtual void help(stringstream& help) const {
        help << "returns information about the daemon's host";
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::hostInfo);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }
    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        ProcessInfo p;
        BSONObjBuilder bSys, bOs;

        bSys.appendDate("currentTime", jsTime());
        bSys.append("hostname", prettyHostName());
        bSys.append("cpuAddrSize", p.getAddrSize());
        bSys.append("memSizeMB", static_cast<unsigned>(p.getMemSizeMB()));
        bSys.append("numCores", p.getNumCores());
        bSys.append("cpuArch", p.getArch());
        bSys.append("numaEnabled", p.hasNumaEnabled());
        bOs.append("type", p.getOsType());
        bOs.append("name", p.getOsName());
        bOs.append("version", p.getOsVersion());

        result.append(StringData("system"), bSys.obj());
        result.append(StringData("os"), bOs.obj());
        p.appendSystemDetails(result);

        return true;
    }

} hostInfoCmd;

class LogRotateCmd : public Command {
public:
    LogRotateCmd() : Command("logRotate") {}
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual bool slaveOk() const {
        return true;
    }
    virtual bool adminOnly() const {
        return true;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::logRotate);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }
    virtual bool run(OperationContext* txn,
                     const string& ns,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        bool didRotate = rotateLogs(serverGlobalParams.logRenameOnRotate);
        if (didRotate)
            logProcessDetailsForLogRotate();
        return didRotate;
    }

} logRotateCmd;

class ListCommandsCmd : public Command {
public:
    virtual void help(stringstream& help) const {
        help << "get a list of all db commands";
    }
    ListCommandsCmd() : Command("listCommands", false) {}
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual bool slaveOk() const {
        return true;
    }
    virtual bool adminOnly() const {
        return false;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {}  // No auth required
    virtual bool run(OperationContext* txn,
                     const string& ns,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        // sort the commands before building the result BSON
        std::vector<Command*> commands;
        for (CommandMap::const_iterator it = _commands->begin(); it != _commands->end(); ++it) {
            // don't show oldnames
            if (it->first == it->second->getName())
                commands.push_back(it->second);
        }
        std::sort(commands.begin(), commands.end(), [](Command* lhs, Command* rhs) {
            return (lhs->getName()) < (rhs->getName());
        });

        BSONObjBuilder b(result.subobjStart("commands"));
        for (const auto& c : commands) {
            BSONObjBuilder temp(b.subobjStart(c->getName()));

            {
                stringstream help;
                c->help(help);
                temp.append("help", help.str());
            }
            temp.append("slaveOk", c->slaveOk());
            temp.append("adminOnly", c->adminOnly());
            // optionally indicates that the command can be forced to run on a slave/secondary
            if (c->slaveOverrideOk())
                temp.append("slaveOverrideOk", c->slaveOverrideOk());
            temp.done();
        }
        b.done();

        return 1;
    }

} listCommandsCmd;

namespace {
MONGO_FP_DECLARE(crashOnShutdown);

int* volatile illegalAddress;
}  // namespace

void CmdShutdown::addRequiredPrivileges(const std::string& dbname,
                                        const BSONObj& cmdObj,
                                        std::vector<Privilege>* out) {
    ActionSet actions;
    actions.addAction(ActionType::shutdown);
    out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
}

void CmdShutdown::shutdownHelper() {
    MONGO_FAIL_POINT_BLOCK(crashOnShutdown, crashBlock) {
        const std::string crashHow = crashBlock.getData()["how"].str();
        if (crashHow == "fault") {
            ++*illegalAddress;
        }
        ::abort();
    }

    log() << "terminating, shutdown command received";

#if defined(_WIN32)
    // Signal the ServiceMain thread to shutdown.
    if (ntservice::shouldStartService()) {
        shutdownNoTerminate();

        // Client expects us to abruptly close the socket as part of exiting
        // so this function is not allowed to return.
        // The ServiceMain thread will quit for us so just sleep until it does.
        while (true)
            sleepsecs(60);  // Loop forever
    } else
#endif
    {
        exitCleanly(EXIT_CLEAN);  // this never returns
        invariant(false);
    }
}

/* for testing purposes only */
class CmdForceError : public Command {
public:
    virtual void help(stringstream& help) const {
        help << "for testing purposes only.  forces a user assertion exception";
    }
    virtual bool slaveOk() const {
        return true;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {}  // No auth required
    CmdForceError() : Command("forceerror") {}
    bool run(OperationContext* txn,
             const string& dbnamne,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        LastError::get(cc()).setLastError(10038, "forced error");
        return false;
    }
} cmdForceError;

class GetLogCmd : public Command {
public:
    GetLogCmd() : Command("getLog") {}

    virtual bool slaveOk() const {
        return true;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual bool adminOnly() const {
        return true;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::getLog);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }
    virtual void help(stringstream& help) const {
        help << "{ getLog : '*' }  OR { getLog : 'global' }";
    }

    virtual bool run(OperationContext* txn,
                     const string& dbname,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        BSONElement val = cmdObj.firstElement();
        if (val.type() != String) {
            return appendCommandStatus(
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

class CmdGetCmdLineOpts : Command {
public:
    CmdGetCmdLineOpts() : Command("getCmdLineOpts") {}
    void help(stringstream& h) const {
        h << "get argv";
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual bool adminOnly() const {
        return true;
    }
    virtual bool slaveOk() const {
        return true;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::getCmdLineOpts);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }
    virtual bool run(OperationContext* txn,
                     const string&,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        result.append("argv", serverGlobalParams.argvArray);
        result.append("parsed", serverGlobalParams.parsedOpts);
        return true;
    }

} cmdGetCmdLineOpts;
}
