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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kAccessControl

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/authenticate.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/authentication_session.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/db/auth/authz_session_external_state_mock.h"
#include "mongo/db/auth/sasl_command_constants.h"
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
namespace {

using std::stringstream;

const bool autoAuthorizeDefault = true;

class CmdSaslStart : public BasicCommand {
public:
    CmdSaslStart();
    virtual ~CmdSaslStart();

    virtual void addRequiredPrivileges(const std::string&,
                                       const BSONObj&,
                                       std::vector<Privilege>*) const {}

    StringData sensitiveFieldName() const final {
        return "payload"_sd;
    }

    virtual bool run(OperationContext* opCtx,
                     const std::string& db,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result);

    virtual std::string help() const override;
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    bool requiresAuth() const override {
        return false;
    }
};

class CmdSaslContinue : public BasicCommand {
public:
    CmdSaslContinue();
    virtual ~CmdSaslContinue();

    virtual void addRequiredPrivileges(const std::string&,
                                       const BSONObj&,
                                       std::vector<Privilege>*) const {}

    StringData sensitiveFieldName() const final {
        return "payload"_sd;
    }

    virtual bool run(OperationContext* opCtx,
                     const std::string& db,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result);

    std::string help() const override;
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    bool requiresAuth() const override {
        return false;
    }
};

CmdSaslStart cmdSaslStart;
CmdSaslContinue cmdSaslContinue;

Status buildResponse(const AuthenticationSession* session,
                     const std::string& responsePayload,
                     BSONType responsePayloadType,
                     BSONObjBuilder* result) {
    result->appendIntOrLL(saslCommandConversationIdFieldName, 1);
    result->appendBool(saslCommandDoneFieldName, session->getMechanism().isSuccess());

    if (responsePayload.size() > size_t(std::numeric_limits<int>::max())) {
        return Status(ErrorCodes::InvalidLength, "Response payload too long");
    }
    if (responsePayloadType == BinData) {
        result->appendBinData(saslCommandPayloadFieldName,
                              int(responsePayload.size()),
                              BinDataGeneral,
                              responsePayload.data());
    } else if (responsePayloadType == String) {
        result->append(saslCommandPayloadFieldName, base64::encode(responsePayload));
    } else {
        fassertFailed(4003);
    }

    return Status::OK();
}

Status extractConversationId(const BSONObj& cmdObj, int64_t* conversationId) {
    BSONElement element;
    Status status = bsonExtractField(cmdObj, saslCommandConversationIdFieldName, &element);
    if (!status.isOK())
        return status;

    if (!element.isNumber()) {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream() << "Wrong type for field; expected number for " << element);
    }
    *conversationId = element.numberLong();
    return Status::OK();
}

Status extractMechanism(const BSONObj& cmdObj, std::string* mechanism) {
    return bsonExtractStringField(cmdObj, saslCommandMechanismFieldName, mechanism);
}

Status doSaslStep(OperationContext* opCtx,
                  AuthenticationSession* session,
                  const BSONObj& cmdObj,
                  BSONObjBuilder* result) {
    std::string payload;
    BSONType type = EOO;
    Status status = saslExtractPayload(cmdObj, &payload, &type);
    if (!status.isOK()) {
        return status;
    }

    auto& mechanism = session->getMechanism();

    // Passing in a payload and extracting a responsePayload
    StatusWith<std::string> swResponse = mechanism.step(opCtx, payload);

    if (!swResponse.isOK()) {
        LOGV2(20249,
              "SASL {mechanism} authentication failed for "
              "{principalName} on {authenticationDatabase} from client "
              "{client} ; {result}",
              "Authentication failed",
              "mechanism"_attr = mechanism.mechanismName(),
              "principalName"_attr = mechanism.getPrincipalName(),
              "authenticationDatabase"_attr = mechanism.getAuthenticationDatabase(),
              "client"_attr = opCtx->getClient()->getRemote().toString(),
              "result"_attr = redact(swResponse.getStatus()));

        sleepmillis(saslGlobalParams.authFailedDelay.load());
        // All the client needs to know is that authentication has failed.
        return AuthorizationManager::authenticationFailedStatus;
    }

    status = buildResponse(session, swResponse.getValue(), type, result);
    if (!status.isOK()) {
        return status;
    }

    if (mechanism.isSuccess()) {
        UserName userName(mechanism.getPrincipalName(), mechanism.getAuthenticationDatabase());
        status =
            AuthorizationSession::get(opCtx->getClient())->addAndAuthorizeUser(opCtx, userName);
        if (!status.isOK()) {
            return status;
        }

        if (!serverGlobalParams.quiet.load()) {
            LOGV2(20250,
                  "Successfully authenticated as principal {principalName} on "
                  "{authenticationDatabase} from client {client} with mechanism {mechanism}",
                  "Successful authentication",
                  "mechanism"_attr = mechanism.mechanismName(),
                  "principalName"_attr = mechanism.getPrincipalName(),
                  "authenticationDatabase"_attr = mechanism.getAuthenticationDatabase(),
                  "client"_attr = opCtx->getClient()->session()->remote());
        }
        if (session->isSpeculative()) {
            status = authCounter.incSpeculativeAuthenticateSuccessful(
                mechanism.mechanismName().toString());
        }
    }
    return status;
}

StatusWith<std::unique_ptr<AuthenticationSession>> doSaslStart(OperationContext* opCtx,
                                                               const std::string& db,
                                                               const BSONObj& cmdObj,
                                                               BSONObjBuilder* result,
                                                               std::string* principalName,
                                                               bool speculative) {
    bool autoAuthorize = false;
    Status status = bsonExtractBooleanFieldWithDefault(
        cmdObj, saslCommandAutoAuthorizeFieldName, autoAuthorizeDefault, &autoAuthorize);
    if (!status.isOK())
        return status;

    std::string mechanismName;
    status = extractMechanism(cmdObj, &mechanismName);
    if (!status.isOK())
        return status;

    StatusWith<std::unique_ptr<ServerMechanismBase>> swMech =
        SASLServerMechanismRegistry::get(opCtx->getServiceContext())
            .getServerMechanism(mechanismName, db);

    if (!swMech.isOK()) {
        return swMech.getStatus();
    }

    auto session =
        std::make_unique<AuthenticationSession>(std::move(swMech.getValue()), speculative);

    if (speculative &&
        !session->getMechanism().properties().hasAllProperties(
            SecurityPropertySet({SecurityProperty::kNoPlainText}))) {
        return {ErrorCodes::BadValue,
                "Plaintext mechanisms may not be used with speculativeSaslStart"};
    }

    auto options = cmdObj["options"];
    if (!options.eoo()) {
        if (options.type() != Object) {
            return {ErrorCodes::BadValue, "saslStart.options must be an object"};
        }
        status = session->setOptions(options.Obj());
        if (!status.isOK()) {
            return status;
        }
    }

    Status statusStep = doSaslStep(opCtx, session.get(), cmdObj, result);

    if (!statusStep.isOK() || session->getMechanism().isSuccess()) {
        // Only attempt to populate principal name if we're done (successfully or not).
        *principalName = session->getMechanism().getPrincipalName().toString();
    }

    if (!statusStep.isOK()) {
        return statusStep;
    }

    return std::move(session);
}

Status doSaslContinue(OperationContext* opCtx,
                      AuthenticationSession* session,
                      const BSONObj& cmdObj,
                      BSONObjBuilder* result) {
    int64_t conversationId = 0;
    Status status = extractConversationId(cmdObj, &conversationId);
    if (!status.isOK())
        return status;
    if (conversationId != 1)
        return Status(ErrorCodes::ProtocolError, "sasl: Mismatched conversation id");

    return doSaslStep(opCtx, session, cmdObj, result);
}

bool runSaslStart(OperationContext* opCtx,
                  const std::string& db,
                  const BSONObj& cmdObj,
                  BSONObjBuilder& result,
                  bool speculative) {
    opCtx->markKillOnClientDisconnect();
    Client* client = opCtx->getClient();
    AuthenticationSession::set(client, std::unique_ptr<AuthenticationSession>());

    std::string mechanismName;
    uassertStatusOK(extractMechanism(cmdObj, &mechanismName));

    auto status = authCounter.incAuthenticateReceived(mechanismName);
    if (!status.isOK()) {
        audit::logAuthentication(client, mechanismName, UserName("", db), status.code());
        uassertStatusOK(status);
        MONGO_UNREACHABLE;
    }

    std::string principalName;
    auto swSession = doSaslStart(opCtx, db, cmdObj, &result, &principalName, speculative);

    if (!swSession.isOK() || swSession.getValue()->getMechanism().isSuccess()) {
        audit::logAuthentication(
            client, mechanismName, UserName(principalName, db), swSession.getStatus().code());
        uassertStatusOK(swSession.getStatus());
        if (swSession.getValue()->getMechanism().isSuccess()) {
            uassertStatusOK(authCounter.incAuthenticateSuccessful(mechanismName));
        }
    } else {
        auto session = std::move(swSession.getValue());
        AuthenticationSession::swap(client, session);
    }

    return true;
}

CmdSaslStart::CmdSaslStart() : BasicCommand(saslStartCommandName) {}
CmdSaslStart::~CmdSaslStart() {}

std::string CmdSaslStart::help() const {
    return "First step in a SASL authentication conversation.";
}

bool CmdSaslStart::run(OperationContext* opCtx,
                       const std::string& db,
                       const BSONObj& cmdObj,
                       BSONObjBuilder& result) {
    return runSaslStart(opCtx, db, cmdObj, result, false);
}

CmdSaslContinue::CmdSaslContinue() : BasicCommand(saslContinueCommandName) {}
CmdSaslContinue::~CmdSaslContinue() {}

std::string CmdSaslContinue::help() const {
    return "Subsequent steps in a SASL authentication conversation.";
}

bool CmdSaslContinue::run(OperationContext* opCtx,
                          const std::string& db,
                          const BSONObj& cmdObj,
                          BSONObjBuilder& result) {
    opCtx->markKillOnClientDisconnect();
    Client* client = Client::getCurrent();
    std::unique_ptr<AuthenticationSession> sessionGuard;
    AuthenticationSession::swap(client, sessionGuard);

    if (!sessionGuard) {
        uasserted(ErrorCodes::ProtocolError, "No SASL session state found");
    }

    AuthenticationSession* session = static_cast<AuthenticationSession*>(sessionGuard.get());

    auto& mechanism = session->getMechanism();
    // Authenticating the __system@local user to the admin database on mongos is required
    // by the auth passthrough test suite.
    if (mechanism.getAuthenticationDatabase() != db && !getTestCommandsEnabled()) {
        uasserted(ErrorCodes::ProtocolError,
                  "Attempt to switch database target during SASL authentication.");
    }

    Status status = doSaslContinue(opCtx, session, cmdObj, &result);
    CommandHelpers::appendCommandStatusNoThrow(result, status);

    if (mechanism.isSuccess() || !status.isOK()) {
        audit::logAuthentication(
            client,
            mechanism.mechanismName(),
            UserName(mechanism.getPrincipalName(), mechanism.getAuthenticationDatabase()),
            status.code());
        if (mechanism.isSuccess()) {
            uassertStatusOK(
                authCounter.incAuthenticateSuccessful(mechanism.mechanismName().toString()));
        }
    } else {
        AuthenticationSession::swap(client, sessionGuard);
    }

    return status.isOK();
}

// The CyrusSaslCommands Enterprise initializer is dependent on PreSaslCommands
MONGO_INITIALIZER(PreSaslCommands)
(InitializerContext*) {
    if (!sequenceContains(saslGlobalParams.authenticationMechanisms, kX509AuthMechanism))
        disableAuthMechanism(kX509AuthMechanism);

    return Status::OK();
}

}  // namespace

void doSpeculativeSaslStart(OperationContext* opCtx, BSONObj cmdObj, BSONObjBuilder* result) try {
    auto mechElem = cmdObj["mechanism"];
    if (mechElem.type() != String) {
        return;
    }

    // Run will make sure an audit entry happens. Let it reach that point.
    authCounter.incSpeculativeAuthenticateReceived(mechElem.String()).ignore();

    auto dbElement = cmdObj["db"];
    if (dbElement.type() != String) {
        return;
    }

    BSONObjBuilder saslStartResult;
    if (runSaslStart(opCtx, dbElement.String(), cmdObj, saslStartResult, true)) {
        result->append(auth::kSpeculativeAuthenticate, saslStartResult.obj());
    }
} catch (...) {
    // Treat failure like we never even got a speculative start.
}

}  // namespace mongo
