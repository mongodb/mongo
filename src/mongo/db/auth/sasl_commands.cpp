/*
 *    Copyright (C) 2012 10gen, Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
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
#include "mongo/util/base64.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/sequence_util.h"
#include "mongo/util/stringutils.h"

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

    void redactForLogging(mutablebson::Document* cmdObj) const override;

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
    result->appendBool(saslCommandDoneFieldName, session->getMechanism().isDone());

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
        log() << "SASL " << mechanism.mechanismName() << " authentication failed for "
              << mechanism.getPrincipalName() << " on " << mechanism.getAuthenticationDatabase()
              << " from client " << opCtx->getClient()->getRemote().toString() << " ; "
              << redact(swResponse.getStatus());

        sleepmillis(saslGlobalParams.authFailedDelay.load());
        // All the client needs to know is that authentication has failed.
        return AuthorizationManager::authenticationFailedStatus;
    }

    status = buildResponse(session, swResponse.getValue(), type, result);
    if (!status.isOK()) {
        return status;
    }

    if (mechanism.isDone()) {
        UserName userName(mechanism.getPrincipalName(), mechanism.getAuthenticationDatabase());
        status =
            AuthorizationSession::get(opCtx->getClient())->addAndAuthorizeUser(opCtx, userName);
        if (!status.isOK()) {
            return status;
        }

        if (!serverGlobalParams.quiet.load()) {
            log() << "Successfully authenticated as principal " << mechanism.getPrincipalName()
                  << " on " << mechanism.getAuthenticationDatabase();
        }
    }
    return Status::OK();
}

StatusWith<std::unique_ptr<AuthenticationSession>> doSaslStart(OperationContext* opCtx,
                                                               const std::string& db,
                                                               const BSONObj& cmdObj,
                                                               BSONObjBuilder* result) {
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

    auto session = std::make_unique<AuthenticationSession>(std::move(swMech.getValue()));
    Status statusStep = doSaslStep(opCtx, session.get(), cmdObj, result);
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

CmdSaslStart::CmdSaslStart() : BasicCommand(saslStartCommandName) {}
CmdSaslStart::~CmdSaslStart() {}

std::string CmdSaslStart::help() const {
    return "First step in a SASL authentication conversation.";
}

void CmdSaslStart::redactForLogging(mutablebson::Document* cmdObj) const {
    mutablebson::Element element = mutablebson::findFirstChildNamed(cmdObj->root(), "payload");
    if (element.ok()) {
        element.setValueString("xxx").transitional_ignore();
    }
}

bool CmdSaslStart::run(OperationContext* opCtx,
                       const std::string& db,
                       const BSONObj& cmdObj,
                       BSONObjBuilder& result) {
    Client* client = opCtx->getClient();
    AuthenticationSession::set(client, std::unique_ptr<AuthenticationSession>());

    std::string mechanismName;
    if (!extractMechanism(cmdObj, &mechanismName).isOK()) {
        return false;
    }

    StatusWith<std::unique_ptr<AuthenticationSession>> swSession =
        doSaslStart(opCtx, db, cmdObj, &result);
    CommandHelpers::appendCommandStatus(result, swSession.getStatus());
    if (!swSession.isOK()) {
        return false;
    }
    auto session = std::move(swSession.getValue());

    auto& mechanism = session->getMechanism();
    if (mechanism.isDone()) {
        audit::logAuthentication(client,
                                 mechanismName,
                                 UserName(mechanism.getPrincipalName(), db),
                                 swSession.getStatus().code());
    } else {
        AuthenticationSession::swap(client, session);
    }
    return swSession.isOK();
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
    Client* client = Client::getCurrent();
    std::unique_ptr<AuthenticationSession> sessionGuard;
    AuthenticationSession::swap(client, sessionGuard);

    if (!sessionGuard) {
        return CommandHelpers::appendCommandStatus(
            result, Status(ErrorCodes::ProtocolError, "No SASL session state found"));
    }

    AuthenticationSession* session = static_cast<AuthenticationSession*>(sessionGuard.get());

    auto& mechanism = session->getMechanism();
    // Authenticating the __system@local user to the admin database on mongos is required
    // by the auth passthrough test suite.
    if (mechanism.getAuthenticationDatabase() != db && !getTestCommandsEnabled()) {
        return CommandHelpers::appendCommandStatus(
            result,
            Status(ErrorCodes::ProtocolError,
                   "Attempt to switch database target during SASL authentication."));
    }

    Status status = doSaslContinue(opCtx, session, cmdObj, &result);
    CommandHelpers::appendCommandStatusNoThrow(result, status);

    if (mechanism.isDone()) {
        audit::logAuthentication(
            client,
            mechanism.mechanismName(),
            UserName(mechanism.getPrincipalName(), mechanism.getAuthenticationDatabase()),
            status.code());
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
}  // namespace mongo
