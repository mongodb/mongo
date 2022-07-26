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

#include <memory>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/authenticate.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/db/auth/authentication_session.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/db/auth/authz_session_external_state_mock.h"
#include "mongo/db/auth/sasl_command_constants.h"
#include "mongo/db/auth/sasl_commands_gen.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/authentication_commands.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/attribute_storage.h"
#include "mongo/logv2/log.h"
#include "mongo/util/base64.h"
#include "mongo/util/sequence_util.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl


namespace mongo {
namespace auth {
namespace {

using std::stringstream;

class CmdSaslStart : public SaslStartCmdVersion1Gen<CmdSaslStart> {
public:
    std::set<StringData> sensitiveFieldNames() const final {
        return {SaslStartCommand::kPayloadFieldName};
    }

    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;

        bool supportsWriteConcern() const final {
            return false;
        }

        NamespaceString ns() const final {
            return NamespaceString(request().getDbName());
        }

        Reply typedRun(OperationContext* opCtx);
    };

    std::string help() const final {
        return "First step in a SASL authentication conversation.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    bool requiresAuth() const override {
        return false;
    }
} cmdSaslStart;

class CmdSaslContinue : public SaslContinueCmdVersion1Gen<CmdSaslContinue> {
public:
    std::set<StringData> sensitiveFieldNames() const final {
        return {SaslContinueCommand::kPayloadFieldName};
    }

    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;

        bool supportsWriteConcern() const final {
            return false;
        }

        NamespaceString ns() const final {
            return NamespaceString(request().getDbName());
        }

        Reply typedRun(OperationContext* opCtx);
    };

    std::string help() const final {
        return "Subsequent steps in a SASL authentication conversation.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    bool requiresAuth() const final {
        return false;
    }
} cmdSaslContinue;

SaslReply doSaslStep(OperationContext* opCtx,
                     const SaslPayload& payload,
                     AuthenticationSession* session) {
    auto mechanismPtr = session->getMechanism();
    invariant(mechanismPtr);
    auto& mechanism = *mechanismPtr;

    // Passing in a payload and extracting a responsePayload
    StatusWith<std::string> swResponse = mechanism.step(opCtx, payload.get());

    auto makeLogAttributes = [&]() {
        logv2::DynamicAttributes attrs;
        attrs.add("mechanism", mechanism.mechanismName());
        attrs.add("speculative", session->isSpeculative());
        attrs.add("principalName", mechanism.getPrincipalName());
        attrs.add("authenticationDatabase", mechanism.getAuthenticationDatabase());
        attrs.addDeepCopy("remote", opCtx->getClient()->getRemote().toString());
        {
            auto bob = BSONObjBuilder();
            mechanism.appendExtraInfo(&bob);
            attrs.add("extraInfo", bob.obj());
        }

        return attrs;
    };

    if (!swResponse.isOK()) {
        int64_t dLevel = 0;
        if (session->isSpeculative() &&
            (swResponse.getStatus() == ErrorCodes::MechanismUnavailable)) {
            dLevel = 5;
        }

        auto attrs = makeLogAttributes();
        auto errorString = redact(swResponse.getStatus());
        attrs.add("error", errorString);
        LOGV2_DEBUG(20249, dLevel, "Authentication failed", attrs);

        sleepmillis(saslGlobalParams.authFailedDelay.load());
        // All the client needs to know is that authentication has failed.
        uassertStatusOK(AuthorizationManager::authenticationFailedStatus);
    }

    if (mechanism.isSuccess()) {
        UserName userName(mechanism.getPrincipalName(), mechanism.getAuthenticationDatabase());
        uassertStatusOK(
            AuthorizationSession::get(opCtx->getClient())->addAndAuthorizeUser(opCtx, userName));

        if (!serverGlobalParams.quiet.load()) {
            auto attrs = makeLogAttributes();
            LOGV2(20250, "Authentication succeeded", attrs);
        }

        session->markSuccessful();
    }

    SaslReply reply;
    reply.setConversationId(1);
    reply.setDone(mechanism.isSuccess());

    SaslPayload replyPayload(swResponse.getValue());
    replyPayload.serializeAsBase64(payload.getSerializeAsBase64());
    reply.setPayload(std::move(replyPayload));

    return reply;
}

void warnIfCompressed(OperationContext* opCtx) {
    if (opCtx->isOpCompressed()) {
        LOGV2_WARNING(6697500,
                      "SASL commands should not be run over the OP_COMPRESSED message type. This "
                      "invocation may have security implications.");
    }
}

SaslReply doSaslStart(OperationContext* opCtx,
                      AuthenticationSession* session,
                      const SaslStartCommand& request) {
    auto mechanism = uassertStatusOK(
        SASLServerMechanismRegistry::get(opCtx->getServiceContext())
            .getServerMechanism(request.getMechanism(), request.getDbName().toString()));

    uassert(ErrorCodes::BadValue,
            "Plaintext mechanisms may not be used with speculativeSaslStart",
            !session->isSpeculative() ||
                mechanism->properties().hasAllProperties(
                    SecurityPropertySet({SecurityProperty::kNoPlainText})));

    session->setMechanism(std::move(mechanism), request.getOptions());

    return doSaslStep(opCtx, request.getPayload(), session);
}

SaslReply runSaslStart(OperationContext* opCtx,
                       AuthenticationSession* session,
                       const SaslStartCommand& request) {
    warnIfCompressed(opCtx);
    opCtx->markKillOnClientDisconnect();

    // Note that while updateDatabase can throw, it should not be able to for saslStart.
    session->updateDatabase(request.getDbName());
    session->setMechanismName(request.getMechanism());

    return doSaslStart(opCtx, session, request);
}

SaslReply runSaslContinue(OperationContext* opCtx,
                          AuthenticationSession* session,
                          const SaslContinueCommand& request);

SaslReply CmdSaslStart::Invocation::typedRun(OperationContext* opCtx) {
    return AuthenticationSession::doStep(
        opCtx, AuthenticationSession::StepType::kSaslStart, [&](auto session) {
            return runSaslStart(opCtx, session, request());
        });
}

SaslReply CmdSaslContinue::Invocation::typedRun(OperationContext* opCtx) {
    return AuthenticationSession::doStep(
        opCtx, AuthenticationSession::StepType::kSaslContinue, [&](auto session) {
            return runSaslContinue(opCtx, session, request());
        });
}

SaslReply runSaslContinue(OperationContext* opCtx,
                          AuthenticationSession* session,
                          const SaslContinueCommand& cmd) {
    warnIfCompressed(opCtx);
    opCtx->markKillOnClientDisconnect();

    uassert(ErrorCodes::ProtocolError,
            "sasl: Mismatched conversation id",
            cmd.getConversationId() == 1);

    return doSaslStep(opCtx, cmd.getPayload(), session);
}

constexpr auto kDBFieldName = "db"_sd;
}  // namespace
}  // namespace auth

void doSpeculativeSaslStart(OperationContext* opCtx,
                            const BSONObj& sourceObj,
                            BSONObjBuilder* result) try {
    auth::warnIfCompressed(opCtx);
    // TypedCommands expect DB overrides in the "$db" field,
    // but saslStart coming from the Hello command has it in the "db" field.
    // Rewrite it for handling here.
    BSONObjBuilder bob;
    bool hasDBField = false;
    for (const auto& elem : sourceObj) {
        if (elem.fieldName() == auth::kDBFieldName) {
            bob.appendAs(elem, auth::SaslStartCommand::kDbNameFieldName);
            hasDBField = true;
        } else {
            bob.append(elem);
        }
    }
    if (!hasDBField) {
        return;
    }

    const auto cmdObj = bob.obj();

    AuthenticationSession::doStep(
        opCtx, AuthenticationSession::StepType::kSpeculativeSaslStart, [&](auto session) {
            auto request =
                auth::SaslStartCommand::parse(IDLParserContext("speculative saslStart"), cmdObj);
            auto reply = auth::runSaslStart(opCtx, session, request);
            result->append(auth::kSpeculativeAuthenticate, reply.toBSON());
        });
} catch (...) {
    // Treat failure like we never even got a speculative start.
}

}  // namespace mongo
