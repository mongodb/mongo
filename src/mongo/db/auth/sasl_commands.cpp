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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/authenticate.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/db/audit.h"
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
#include "mongo/db/stats/counters.h"
#include "mongo/logv2/log.h"
#include "mongo/util/base64.h"
#include "mongo/util/sequence_util.h"
#include "mongo/util/str.h"

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

        void doCheckAuthorization(OperationContext*) const final {}

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

        void doCheckAuthorization(OperationContext*) const final {}

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

StatusWith<SaslReply> doSaslStep(OperationContext* opCtx,
                                 const SaslPayload& payload,
                                 AuthenticationSession* session) try {
    auto mechanismPtr = session->getMechanism();
    invariant(mechanismPtr);
    auto& mechanism = *mechanismPtr;

    // Passing in a payload and extracting a responsePayload
    StatusWith<std::string> swResponse = mechanism.step(opCtx, payload.get());

    if (!swResponse.isOK()) {
        int64_t dLevel = 0;
        if (session->isSpeculative() &&
            (swResponse.getStatus() == ErrorCodes::MechanismUnavailable)) {
            dLevel = 5;
        }
        LOGV2_DEBUG(20249,
                    dLevel,
                    "SASL {mechanism} authentication failed for "
                    "{principalName} on {authenticationDatabase} from client "
                    "{client} ; {result}",
                    "Authentication failed",
                    "mechanism"_attr = mechanism.mechanismName(),
                    "speculative"_attr = session->isSpeculative(),
                    "principalName"_attr = mechanism.getPrincipalName(),
                    "authenticationDatabase"_attr = mechanism.getAuthenticationDatabase(),
                    "remote"_attr = opCtx->getClient()->getRemote(),
                    "result"_attr = redact(swResponse.getStatus()));

        sleepmillis(saslGlobalParams.authFailedDelay.load());
        // All the client needs to know is that authentication has failed.
        return AuthorizationManager::authenticationFailedStatus;
    }

    if (mechanism.isSuccess()) {
        UserName userName(mechanism.getPrincipalName(), mechanism.getAuthenticationDatabase());
        uassertStatusOK(
            AuthorizationSession::get(opCtx->getClient())->addAndAuthorizeUser(opCtx, userName));

        if (!serverGlobalParams.quiet.load()) {
            LOGV2(20250,
                  "Successfully authenticated as principal {principalName} on "
                  "{authenticationDatabase} from client {client} with mechanism {mechanism}",
                  "Successful authentication",
                  "mechanism"_attr = mechanism.mechanismName(),
                  "principalName"_attr = mechanism.getPrincipalName(),
                  "authenticationDatabase"_attr = mechanism.getAuthenticationDatabase(),
                  "remote"_attr = opCtx->getClient()->session()->remote());
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
} catch (const DBException& ex) {
    return ex.toStatus();
}

SaslReply doSaslStart(OperationContext* opCtx,
                      AuthenticationSession* session,
                      const SaslStartCommand& request,
                      std::string* principalName) {
    auto mechanism = uassertStatusOK(
        SASLServerMechanismRegistry::get(opCtx->getServiceContext())
            .getServerMechanism(request.getMechanism(), request.getDbName().toString()));

    uassert(ErrorCodes::BadValue,
            "Plaintext mechanisms may not be used with speculativeSaslStart",
            !session->isSpeculative() ||
                mechanism->properties().hasAllProperties(
                    SecurityPropertySet({SecurityProperty::kNoPlainText})));

    session->setMechanism(std::move(mechanism), request.getOptions());

    auto swReply = doSaslStep(opCtx, request.getPayload(), session);
    if (!swReply.isOK() || session->getMechanism()->isSuccess()) {
        // Only attempt to populate principal name if we're done (successfully or not).
        *principalName = session->getMechanism()->getPrincipalName().toString();
    }

    auto reply = uassertStatusOK(swReply);
    return reply;
}

SaslReply runSaslStart(OperationContext* opCtx,
                       AuthenticationSession* session,
                       const SaslStartCommand& request) {
    opCtx->markKillOnClientDisconnect();
    auto client = opCtx->getClient();

    auto db = request.getDbName();
    auto mechanismName = request.getMechanism().toString();

    SaslReply reply;
    std::string principalName;
    try {
        auto mechCounter = authCounter.getMechanismCounter(mechanismName);
        mechCounter.incAuthenticateReceived();
        if (session->isSpeculative()) {
            mechCounter.incSpeculativeAuthenticateReceived();
        }

        reply = doSaslStart(opCtx, session, request, &principalName);

        if (session->isClusterMember()) {
            mechCounter.incClusterAuthenticateReceived();
        }
        if (session->getMechanism()->isSuccess()) {
            mechCounter.incAuthenticateSuccessful();
            if (session->isClusterMember()) {
                mechCounter.incClusterAuthenticateSuccessful();
            }
            if (session->isSpeculative()) {
                mechCounter.incSpeculativeAuthenticateSuccessful();
            }
            audit::logAuthentication(
                client, mechanismName, UserName(principalName, db), ErrorCodes::OK);
        }
    } catch (const AssertionException& ex) {
        audit::logAuthentication(client, mechanismName, UserName(principalName, db), ex.code());
        throw;
    }

    return reply;
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
    opCtx->markKillOnClientDisconnect();
    auto* client = Client::getCurrent();

    auto mechanismPtr = session->getMechanism();
    invariant(mechanismPtr);
    auto& mechanism = *mechanismPtr;

    // Authenticating the __system@local user to the admin database on mongos is required
    // by the auth passthrough test suite.
    if (mechanism.getAuthenticationDatabase() != cmd.getDbName() && !getTestCommandsEnabled()) {
        uasserted(ErrorCodes::ProtocolError,
                  "Attempt to switch database target during SASL authentication.");
    }

    uassert(ErrorCodes::ProtocolError,
            "sasl: Mismatched conversation id",
            cmd.getConversationId() == 1);

    auto swReply = doSaslStep(opCtx, cmd.getPayload(), session);

    if (mechanism.isSuccess() || !swReply.isOK()) {
        audit::logAuthentication(
            client,
            mechanism.mechanismName(),
            UserName(mechanism.getPrincipalName(), mechanism.getAuthenticationDatabase()),
            swReply.getStatus().code());

        auto mechCounter = authCounter.getMechanismCounter(mechanism.mechanismName());
        if (mechanism.isSuccess()) {
            mechCounter.incAuthenticateSuccessful();
            if (mechanism.isClusterMember()) {
                mechCounter.incClusterAuthenticateSuccessful();
            }
            if (session->isSpeculative()) {
                mechCounter.incSpeculativeAuthenticateSuccessful();
            }
        }
    }

    return uassertStatusOK(swReply);
}

constexpr auto kDBFieldName = "db"_sd;
}  // namespace
}  // namespace auth

void doSpeculativeSaslStart(OperationContext* opCtx, BSONObj cmdObj, BSONObjBuilder* result) try {
    // TypedCommands expect DB overrides in the "$db" field,
    // but saslStart coming from the Hello command has it in the "db" field.
    // Rewrite it for handling here.
    BSONObjBuilder cmd;
    bool hasDBField = false;
    for (const auto& elem : cmdObj) {
        if (elem.fieldName() == auth::kDBFieldName) {
            cmd.appendAs(elem, auth::SaslStartCommand::kDbNameFieldName);
            hasDBField = true;
        } else {
            cmd.append(elem);
        }
    }
    if (!hasDBField) {
        return;
    }

    AuthenticationSession::doStep(
        opCtx, AuthenticationSession::StepType::kSpeculativeSaslStart, [&](auto session) {
            auto request = auth::SaslStartCommand::parse(
                IDLParserErrorContext("speculative saslStart"), cmd.obj());
            auto reply = auth::runSaslStart(opCtx, session, request);
            result->append(auth::kSpeculativeAuthenticate, reply.toBSON());
        });
} catch (...) {
    // Treat failure like we never even got a speculative start.
}

}  // namespace mongo
