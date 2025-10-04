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
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/generic_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/idl/generic_argument_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/attribute_storage.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <compare>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {
namespace {

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
};
MONGO_REGISTER_COMMAND(PingCommand).forRouter().forShard();

class EchoCommand final : public TypedCommand<EchoCommand> {
public:
    class Request : public BasicTypedRequest {
    public:
        static constexpr auto kCommandName = "echo"_sd;
        static Request parse(const OpMsgRequest& opMsgRequest,
                             const IDLParserContext&,
                             DeserializationContext*) {
            return Request{opMsgRequest};
        }

        explicit Request(const OpMsgRequest& opMsgRequest)
            : BasicTypedRequest{opMsgRequest}, _opMsgRequest{opMsgRequest} {}

        const OpMsgRequest& opMsgRequest() const {
            return _opMsgRequest;
        }

    private:
        const OpMsgRequest& _opMsgRequest;
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
            return NamespaceString(request().getDbName());
        }

        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* result) override {
            auto sequences = request().opMsgRequest().sequences;
            for (auto& docSeq : sequences) {
                auto docBuilder = result->getDocSequenceBuilder(docSeq.name);
                for (auto& bson : docSeq.objs) {
                    docBuilder.append(bson);
                }
            }

            result->getBodyBuilder().append("echo", request().opMsgRequest().body);
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool requiresAuth() const override {
        return false;
    }
};

MONGO_REGISTER_COMMAND(EchoCommand).testOnly().forRouter().forShard();

class ListCommandsCmd final : public TypedCommand<ListCommandsCmd> {
public:
    std::string help() const override {
        return "get a list of all db commands";
    }

    class Request : public BasicTypedRequest {
    public:
        static constexpr StringData kCommandName = "listCommands";
        static Request parse(const OpMsgRequest& opMsgRequest,
                             const IDLParserContext&,
                             DeserializationContext*) {
            return Request{opMsgRequest};
        }

        using BasicTypedRequest::BasicTypedRequest;
    };

    class Invocation final : public MinimalInvocationBase {
    public:
        using MinimalInvocationBase::MinimalInvocationBase;

        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* result) override {
            auto&& bob = result->getBodyBuilder();
            _run(opCtx, bob);
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext*) const override {}
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return false;
    }

    bool requiresAuth() const final {
        return false;
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    static void _run(OperationContext* opCtx, BSONObjBuilder& result) {
        // Sort the command names before building the result BSON.
        std::vector<Command*> commands;
        getCommandRegistry(opCtx)->forEachCommand([&](Command* c) { commands.push_back(c); });
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
        }
    }
};
MONGO_REGISTER_COMMAND(ListCommandsCmd).forRouter().forShard();

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
                auto obj = cmd.toBSON();
                LOGV2_DEBUG(5060599,
                            3,
                            "Non-debug severity levels must not pass 'debugLevel'",
                            "severity"_attr = obj[Request::kSeverityFieldName].valueStringData(),
                            "debugLevel"_attr = optDebugLevel.value());
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
                    as->isAuthorizedForActionsOnResource(
                        ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                        ActionType::applicationMessage));
        }

        NamespaceString ns() const final {
            return NamespaceString(request().getDbName());
        }
    };

    bool adminOnly() const final {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }
};

MONGO_REGISTER_COMMAND(CmdLogMessage).testOnly().forRouter().forShard();

}  // namespace
}  // namespace mongo
