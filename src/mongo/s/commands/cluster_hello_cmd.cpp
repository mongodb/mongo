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

#include "mongo/base/string_data.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/audit.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/curop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/repl/hello_auth.h"
#include "mongo/db/repl/hello_gen.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/wire_version.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/rpc/rewrite_state_change_errors.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/s/load_balancer_support.h"
#include "mongo/s/mongos_topology_coordinator.h"
#include "mongo/transport/message_compressor_manager.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/version.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

// Hangs in the beginning of each hello command when set.
MONGO_FAIL_POINT_DEFINE(waitInHello);

MONGO_FAIL_POINT_DEFINE(appendHelloOkToHelloResponse);

namespace {

constexpr auto kHelloString = "hello"_sd;
constexpr auto kCamelCaseIsMasterString = "isMaster"_sd;
constexpr auto kLowerCaseIsMasterString = "ismaster"_sd;
const std::string kAutomationServiceDescriptorFieldName =
    HelloCommandReply::kAutomationServiceDescriptorFieldName.toString();

class CmdHello : public BasicCommandWithReplyBuilderInterface {
public:
    CmdHello() : CmdHello(kHelloString, {}) {}

    const std::set<std::string>& apiVersions() const override {
        return kApiVersions1;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const final {
        return false;
    }

    ReadConcernSupportResult supportsReadConcern(const BSONObj& cmdObj,
                                                 repl::ReadConcernLevel level,
                                                 bool isImplicitDefault) const final {
        static const Status kReadConcernNotSupported{ErrorCodes::InvalidOptions,
                                                     "read concern not supported"};
        static const Status kDefaultReadConcernNotPermitted{
            ErrorCodes::InvalidOptions, "cluster wide default read concern not permitted"};
        static const Status kImplicitDefaultReadConcernNotPermitted{
            ErrorCodes::InvalidOptions, "implicit default read concern not permitted"};
        return {{level != repl::ReadConcernLevel::kLocalReadConcern, kReadConcernNotSupported},
                {kDefaultReadConcernNotPermitted},
                {kImplicitDefaultReadConcernNotPermitted}};
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    std::string help() const override {
        return "Status information for clients negotiating a connection with this server";
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const final {
        // No auth required
    }

    bool requiresAuth() const final {
        return false;
    }

    bool runWithReplyBuilder(OperationContext* opCtx,
                             const DatabaseName& dbName,
                             const BSONObj& cmdObj,
                             rpc::ReplyBuilderInterface* replyBuilder) final {
        CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);
        const bool apiStrict = APIParameters::get(opCtx).getAPIStrict().value_or(false);
        auto cmd = HelloCommand::parse({"hello", apiStrict}, cmdObj);

        waitInHello.execute([&](const BSONObj&) {
            LOGV2(6524600,
                  "Fail point blocks Hello response until removed",
                  "cmd"_attr = cmdObj,
                  "client"_attr = opCtx->getClient()->clientAddress(true),
                  "desc"_attr = opCtx->getClient()->desc());

            waitInHello.pauseWhileSet(opCtx);
        });

        // "hello" is exempt from error code rewrites.
        rpc::RewriteStateChangeErrors::setEnabled(opCtx, false);

        auto client = opCtx->getClient();
        if (ClientMetadata::tryFinalize(client)) {
            audit::logClientMetadata(client);
        }

        // If a client is following the awaitable hello protocol, maxAwaitTimeMS should be
        // present if and only if topologyVersion is present in the request.
        auto clientTopologyVersion = cmd.getTopologyVersion();
        auto maxAwaitTimeMS = cmd.getMaxAwaitTimeMS();
        auto curOp = CurOp::get(opCtx);
        boost::optional<Date_t> deadline;
        boost::optional<ScopeGuard<std::function<void()>>> timerGuard;
        if (clientTopologyVersion && maxAwaitTimeMS) {
            uassert(51758,
                    "topologyVersion must have a non-negative counter",
                    clientTopologyVersion->getCounter() >= 0);

            uassert(51759, "maxAwaitTimeMS must be a non-negative integer", *maxAwaitTimeMS >= 0);

            deadline = opCtx->getServiceContext()->getPreciseClockSource()->now() +
                Milliseconds(*maxAwaitTimeMS);

            LOGV2_DEBUG(23871, 3, "Using maxAwaitTimeMS for awaitable hello protocol.");
            curOp->pauseTimer();
            timerGuard.emplace([curOp]() { curOp->resumeTimer(); });
        } else {
            uassert(51760,
                    (clientTopologyVersion
                         ? "A request with a 'topologyVersion' must include 'maxAwaitTimeMS'"
                         : "A request with 'maxAwaitTimeMS' must include a 'topologyVersion'"),
                    !clientTopologyVersion && !maxAwaitTimeMS);
        }

        auto result = replyBuilder->getBodyBuilder();
        const auto* mongosTopCoord = MongosTopologyCoordinator::get(opCtx);

        auto mongosHelloResponse =
            mongosTopCoord->awaitHelloResponse(opCtx, clientTopologyVersion, deadline);

        timerGuard.reset();  // Resume curOp timer.
        mongosHelloResponse->appendToBuilder(&result, useLegacyResponseFields());
        // The hello response always includes a topologyVersion.
        auto currentMongosTopologyVersion = mongosHelloResponse->getTopologyVersion();

        load_balancer_support::handleHello(opCtx, &result, cmd.getLoadBalanced().value_or(false));

        // Try to parse the optional 'helloOk' field. On mongos, if we see this field, we will
        // respond with helloOk: true so the client knows that it can continue to send the hello
        // command to mongos.
        if (auto helloOk = cmd.getHelloOk()) {
            // If the hello request contains a "helloOk" field, set _supportsHello on the Client
            // to the value.
            client->setSupportsHello(*helloOk);
            // Attach helloOk: true to the response so that the client knows the server supports
            // the hello command.
            result.append("helloOk", true);
        }

        if (MONGO_unlikely(appendHelloOkToHelloResponse.shouldFail())) {
            result.append("clientSupportsHello", client->supportsHello());
        }

        result.appendNumber(HelloCommandReply::kMaxBsonObjectSizeFieldName, BSONObjMaxUserSize);
        result.appendNumber(HelloCommandReply::kMaxMessageSizeBytesFieldName,
                            static_cast<long long>(MaxMessageSizeBytes));
        result.appendNumber(HelloCommandReply::kMaxWriteBatchSizeFieldName,
                            static_cast<long long>(write_ops::kMaxWriteBatchSize));
        result.appendDate(HelloCommandReply::kLocalTimeFieldName, jsTime());
        result.append(HelloCommandReply::kLogicalSessionTimeoutMinutesFieldName,
                      localLogicalSessionTimeoutMinutes);
        result.appendNumber(HelloCommandReply::kConnectionIdFieldName,
                            opCtx->getClient()->getConnectionId());

        // Mongos tries to keep exactly the same version range of the server for which
        // it is compiled.
        auto wireSpec = WireSpec::instance().get();
        result.append(HelloCommandReply::kMaxWireVersionFieldName,
                      wireSpec->incomingExternalClient.maxWireVersion);
        result.append(HelloCommandReply::kMinWireVersionFieldName,
                      wireSpec->incomingExternalClient.minWireVersion);

        if (auto sp = ServerParameterSet::getNodeParameterSet()->getIfExists(
                kAutomationServiceDescriptorFieldName)) {
            sp->append(opCtx, &result, kAutomationServiceDescriptorFieldName, boost::none);
        }

        MessageCompressorManager::forSession(opCtx->getClient()->session())
            .serverNegotiate(cmd.getCompression(), &result);

        if (opCtx->isExhaust()) {
            LOGV2_DEBUG(23872, 3, "Using exhaust for hello protocol");

            uassert(51763,
                    "A hello/isMaster request with exhaust must specify 'maxAwaitTimeMS'",
                    maxAwaitTimeMS);
            invariant(clientTopologyVersion);

            InExhaustHello::get(opCtx->getClient()->session().get())
                ->setInExhaust(true /* inExhaust */, getName());

            if (clientTopologyVersion->getProcessId() ==
                    currentMongosTopologyVersion.getProcessId() &&
                clientTopologyVersion->getCounter() == currentMongosTopologyVersion.getCounter()) {
                // Indicate that an exhaust message should be generated and the previous BSONObj
                // command parameters should be reused as the next BSONObj command parameters.
                replyBuilder->setNextInvocation(boost::none);
            } else {
                BSONObjBuilder niBuilder;
                for (const auto& elem : cmdObj) {
                    if (elem.fieldNameStringData() == HelloCommand::kTopologyVersionFieldName) {
                        BSONObjBuilder tvBuilder(
                            niBuilder.subobjStart(HelloCommand::kTopologyVersionFieldName));
                        currentMongosTopologyVersion.serialize(&tvBuilder);
                    } else {
                        niBuilder.append(elem);
                    }
                }
                replyBuilder->setNextInvocation(niBuilder.obj());
            }
        }

        handleHelloAuth(opCtx, dbName, cmd, &result);

        if (getTestCommandsEnabled()) {
            validateResult(&result);
        }

        return true;
    }

    void validateResult(BSONObjBuilder* result) {
        auto ret = result->asTempObj();
        if (ret[ErrorReply::kErrmsgFieldName].eoo()) {
            // Nominal success case, parse the object as-is.
            HelloCommandReply::parse(IDLParserContext{"hello.reply"}, ret);
        } else {
            // Something went wrong, still try to parse, but accept a few ignorable fields.
            StringDataSet ignorable({ErrorReply::kCodeFieldName, ErrorReply::kErrmsgFieldName});
            HelloCommandReply::parse(IDLParserContext{"hello.reply"}, ret.removeFields(ignorable));
        }
    }

protected:
    CmdHello(const StringData cmdName, const std::initializer_list<StringData>& alias)
        : BasicCommandWithReplyBuilderInterface(cmdName, alias) {}

    virtual bool useLegacyResponseFields() const {
        return false;
    }

} hello;

class CmdIsMaster : public CmdHello {
public:
    CmdIsMaster() : CmdHello(kCamelCaseIsMasterString, {kLowerCaseIsMasterString}) {}

    const std::set<std::string>& apiVersions() const final {
        return kNoApiVersions;
    }

protected:
    bool useLegacyResponseFields() const final {
        return true;
    }
} isMaster;

}  // namespace
}  // namespace mongo
