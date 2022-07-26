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

#include "mongo/bson/util/bson_extract.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/generic_gen.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/log_process_details.h"
#include "mongo/logv2/log.h"
#include "mongo/util/processinfo.h"

#include <sstream>
#include <string>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {
namespace {

using std::string;
using std::stringstream;
using std::vector;

class PingCommand : public PingCmdVersion1Gen<PingCommand> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    std::string help() const final {
        return "a way to check that the server is alive. responds immediately even if server is "
               "in a db lock.";
    }

    bool requiresAuth() const final {
        return false;
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;
        bool supportsWriteConcern() const final {
            return false;
        }
        bool allowsAfterClusterTime() const final {
            return false;
        }
        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }
        Reply typedRun(OperationContext* opCtx) final {
            // IMPORTANT: Don't put anything in here that might lock db - including authentication
            return Reply{};
        }
    };
} pingCmd;

class EchoCommand final : public TypedCommand<EchoCommand> {
public:
    struct Request {
        static constexpr auto kCommandName = "echo"_sd;
        static Request parse(const IDLParserContext&, const OpMsgRequest& request) {
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

        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* result) override {
            auto sequences = request().request.sequences;
            for (auto& docSeq : sequences) {
                auto docBuilder = result->getDocSequenceBuilder(docSeq.name);
                for (auto& bson : docSeq.objs) {
                    docBuilder.append(bson);
                }
            }

            result->getBodyBuilder().append("echo", request().request.body);
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
    bool requiresAuth() const final {
        return false;
    }
    virtual bool run(OperationContext* opCtx,
                     const string& ns,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        // Sort the command names before building the result BSON.
        std::vector<Command*> commands;
        for (const auto& command : globalCommandRegistry()->allCommands()) {
            // Don't show oldnames
            if (command.first == command.second->getName())
                commands.push_back(command.second);
        }
        std::sort(commands.begin(), commands.end(), [](Command* lhs, Command* rhs) {
            return (lhs->getName()) < (rhs->getName());
        });

        BSONObjBuilder b(result.subobjStart("commands"));
        for (const auto& command : commands) {
            BSONObjBuilder temp(b.subobjStart(command->getName()));
            temp.append("help", command->help());
            temp.append("requiresAuth", command->requiresAuth());
            temp.append("secondaryOk",
                        command->secondaryAllowed(opCtx->getServiceContext()) ==
                            Command::AllowedOnSecondary::kAlways);
            temp.append("adminOnly", command->adminOnly());
            // Optionally indicates that the command can be forced to run on a secondary.
            if (command->secondaryAllowed(opCtx->getServiceContext()) ==
                Command::AllowedOnSecondary::kOptIn)
                temp.append("secondaryOverrideOk", true);
            temp.append("apiVersions", command->apiVersions());
            temp.append("deprecatedApiVersions", command->deprecatedApiVersions());
            temp.done();
        }

        b.done();

        return 1;
    }

} listCommandsCmd;

class CmdLogMessage : public TypedCommand<CmdLogMessage> {
public:
    using Request = LogMessageCommand;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            auto cmd = request();

            logv2::DynamicAttributes attrs;
            attrs.add("msg", cmd.getCommandParameter());
            if (auto extra = cmd.getExtra()) {
                attrs.add("extra", *extra);
            }

            auto options = logv2::LogOptions{logv2::LogComponent::kDefault};
            LOGV2_IMPL(5060500, getSeverity(cmd), options, "logMessage", attrs);
        }

    private:
        static logv2::LogSeverity getSeverity(const Request& cmd) {
            auto severity = cmd.getSeverity();
            auto optDebugLevel = cmd.getDebugLevel();

            if (optDebugLevel && (severity != MessageSeverityEnum::kDebug)) {
                auto obj = cmd.toBSON({});
                LOGV2_DEBUG(5060599,
                            3,
                            "Non-debug severity levels must not pass 'debugLevel'",
                            "severity"_attr = obj[Request::kSeverityFieldName].valueStringData(),
                            "debugLevel"_attr = optDebugLevel.get());
            }

            switch (severity) {
                case MessageSeverityEnum::kSevere:
                    return logv2::LogSeverity::Severe();
                case MessageSeverityEnum::kError:
                    return logv2::LogSeverity::Error();
                case MessageSeverityEnum::kWarning:
                    return logv2::LogSeverity::Warning();
                case MessageSeverityEnum::kInfo:
                    return logv2::LogSeverity::Info();
                case MessageSeverityEnum::kLog:
                    return logv2::LogSeverity::Log();
                case MessageSeverityEnum::kDebug:
                    return logv2::LogSeverity::Debug(
                        boost::get_optional_value_or(optDebugLevel, 1));
            }

            MONGO_UNREACHABLE;
        }

        bool supportsWriteConcern() const final {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            auto* client = opCtx->getClient();
            auto* as = AuthorizationSession::get(client);
            uassert(ErrorCodes::Unauthorized,
                    "Not authorized to send custom message to log",
                    as->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                         ActionType::applicationMessage));
        }

        NamespaceString ns() const final {
            return NamespaceString(request().getDbName(), "");
        }
    };

    bool adminOnly() const final {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }
};

MONGO_REGISTER_TEST_COMMAND(CmdLogMessage);

}  // namespace
}  // namespace mongo
