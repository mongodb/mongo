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

#include <sstream>
#include <string>
#include <vector>

namespace mongo {
namespace {

using std::string;
using std::stringstream;
using std::vector;

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

class EchoCommand final : public TypedCommand<EchoCommand> {
public:
    struct Request {
        static constexpr auto kCommandName = "echo"_sd;
        static Request parse(const IDLParserErrorContext&, const OpMsgRequest& request) {
            return Request{request};
        }

        const OpMsgRequest& request;
    };

    class Invocation final : public MinimalInvocationBase {
    public:
        using MinimalInvocationBase::MinimalInvocationBase;

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {}

        NamespaceString ns() const override {
            return NamespaceString(request().request.getDatabase());
        }

        void run(OperationContext* opCtx, CommandReplyBuilder* result) override {
            result->append("echo", request().request.body);
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool requiresAuth() const override {
        return false;
    }
};
constexpr StringData EchoCommand::Request::kCommandName;

MONGO_REGISTER_TEST_COMMAND(EchoCommand);

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

}  // namespace
}  // namespace mongo
