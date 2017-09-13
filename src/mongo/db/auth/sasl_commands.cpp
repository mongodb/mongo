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


#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/db/auth/authz_session_external_state_mock.h"
#include "mongo/db/auth/mongo_authentication_session.h"
#include "mongo/db/auth/sasl_authentication_session.h"
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
                                       std::vector<Privilege>*) {}

    void redactForLogging(mutablebson::Document* cmdObj) override;

    virtual bool run(OperationContext* opCtx,
                     const std::string& db,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result);

    virtual void help(stringstream& help) const;
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual bool slaveOk() const {
        return true;
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
                                       std::vector<Privilege>*) {}

    virtual bool run(OperationContext* opCtx,
                     const std::string& db,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result);

    virtual void help(stringstream& help) const;
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual bool slaveOk() const {
        return true;
    }
    bool requiresAuth() const override {
        return false;
    }
};

CmdSaslStart cmdSaslStart;
CmdSaslContinue cmdSaslContinue;
Status buildResponse(const SaslAuthenticationSession* session,
                     const std::string& responsePayload,
                     BSONType responsePayloadType,
                     BSONObjBuilder* result) {
    result->appendIntOrLL(saslCommandConversationIdFieldName, session->getConversationId());
    result->appendBool(saslCommandDoneFieldName, session->isDone());

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

Status doSaslStep(const Client* client,
                  SaslAuthenticationSession* session,
                  const BSONObj& cmdObj,
                  BSONObjBuilder* result) {
    std::string payload;
    BSONType type = EOO;
    Status status = saslExtractPayload(cmdObj, &payload, &type);
    if (!status.isOK())
        return status;

    std::string responsePayload;
    // Passing in a payload and extracting a responsePayload
    status = session->step(payload, &responsePayload);

    if (!status.isOK()) {
        log() << session->getMechanism() << " authentication failed for "
              << session->getPrincipalId() << " on " << session->getAuthenticationDatabase()
              << " from client " << client->getRemote().toString() << " ; " << redact(status);

        sleepmillis(saslGlobalParams.authFailedDelay.load());
        // All the client needs to know is that authentication has failed.
        return AuthorizationManager::authenticationFailedStatus;
    }

    status = buildResponse(session, responsePayload, type, result);
    if (!status.isOK())
        return status;

    if (session->isDone()) {
        UserName userName(session->getPrincipalId(), session->getAuthenticationDatabase());
        status =
            session->getAuthorizationSession()->addAndAuthorizeUser(session->getOpCtxt(), userName);
        if (!status.isOK()) {
            return status;
        }

        if (!serverGlobalParams.quiet.load()) {
            log() << "Successfully authenticated as principal " << session->getPrincipalId()
                  << " on " << session->getAuthenticationDatabase();
        }
    }
    return Status::OK();
}

Status doSaslStart(const Client* client,
                   SaslAuthenticationSession* session,
                   const std::string& db,
                   const BSONObj& cmdObj,
                   BSONObjBuilder* result) {
    bool autoAuthorize = false;
    Status status = bsonExtractBooleanFieldWithDefault(
        cmdObj, saslCommandAutoAuthorizeFieldName, autoAuthorizeDefault, &autoAuthorize);
    if (!status.isOK())
        return status;

    std::string mechanism;
    status = extractMechanism(cmdObj, &mechanism);
    if (!status.isOK())
        return status;

    if (!sequenceContains(saslGlobalParams.authenticationMechanisms, mechanism) &&
        mechanism != "SCRAM-SHA-1") {
        // Always allow SCRAM-SHA-1 to pass to the first sasl step since we need to
        // handle internal user authentication, SERVER-16534
        result->append(saslCommandMechanismListFieldName,
                       saslGlobalParams.authenticationMechanisms);
        return Status(ErrorCodes::BadValue,
                      mongoutils::str::stream() << "Unsupported mechanism " << mechanism);
    }

    status = session->start(
        db, mechanism, saslGlobalParams.serviceName, saslGlobalParams.hostName, 1, autoAuthorize);
    if (!status.isOK())
        return status;

    return doSaslStep(client, session, cmdObj, result);
}

Status doSaslContinue(const Client* client,
                      SaslAuthenticationSession* session,
                      const BSONObj& cmdObj,
                      BSONObjBuilder* result) {
    int64_t conversationId = 0;
    Status status = extractConversationId(cmdObj, &conversationId);
    if (!status.isOK())
        return status;
    if (conversationId != session->getConversationId())
        return Status(ErrorCodes::ProtocolError, "sasl: Mismatched conversation id");

    return doSaslStep(client, session, cmdObj, result);
}

CmdSaslStart::CmdSaslStart() : BasicCommand(saslStartCommandName) {}
CmdSaslStart::~CmdSaslStart() {}

void CmdSaslStart::help(std::stringstream& os) const {
    os << "First step in a SASL authentication conversation.";
}

void CmdSaslStart::redactForLogging(mutablebson::Document* cmdObj) {
    mutablebson::Element element = mutablebson::findFirstChildNamed(cmdObj->root(), "payload");
    if (element.ok()) {
        element.setValueString("xxx").transitional_ignore();
    }
}

bool CmdSaslStart::run(OperationContext* opCtx,
                       const std::string& db,
                       const BSONObj& cmdObj,
                       BSONObjBuilder& result) {
    Client* client = Client::getCurrent();
    AuthenticationSession::set(client, std::unique_ptr<AuthenticationSession>());

    std::string mechanism;
    if (!extractMechanism(cmdObj, &mechanism).isOK()) {
        return false;
    }

    SaslAuthenticationSession* session =
        SaslAuthenticationSession::create(AuthorizationSession::get(client), db, mechanism);

    std::unique_ptr<AuthenticationSession> sessionGuard(session);

    session->setOpCtxt(opCtx);

    Status status = doSaslStart(client, session, db, cmdObj, &result);
    appendCommandStatus(result, status);

    if (session->isDone()) {
        audit::logAuthentication(client,
                                 session->getMechanism(),
                                 UserName(session->getPrincipalId(), db),
                                 status.code());
    } else {
        AuthenticationSession::swap(client, sessionGuard);
    }
    return status.isOK();
}

CmdSaslContinue::CmdSaslContinue() : BasicCommand(saslContinueCommandName) {}
CmdSaslContinue::~CmdSaslContinue() {}

void CmdSaslContinue::help(std::stringstream& os) const {
    os << "Subsequent steps in a SASL authentication conversation.";
}

bool CmdSaslContinue::run(OperationContext* opCtx,
                          const std::string& db,
                          const BSONObj& cmdObj,
                          BSONObjBuilder& result) {
    Client* client = Client::getCurrent();
    std::unique_ptr<AuthenticationSession> sessionGuard;
    AuthenticationSession::swap(client, sessionGuard);

    if (!sessionGuard || sessionGuard->getType() != AuthenticationSession::SESSION_TYPE_SASL) {
        return appendCommandStatus(
            result, Status(ErrorCodes::ProtocolError, "No SASL session state found"));
    }

    SaslAuthenticationSession* session =
        static_cast<SaslAuthenticationSession*>(sessionGuard.get());

    // Authenticating the __system@local user to the admin database on mongos is required
    // by the auth passthrough test suite.
    if (session->getAuthenticationDatabase() != db && !Command::testCommandsEnabled) {
        return appendCommandStatus(
            result,
            Status(ErrorCodes::ProtocolError,
                   "Attempt to switch database target during SASL authentication."));
    }

    session->setOpCtxt(opCtx);

    Status status = doSaslContinue(client, session, cmdObj, &result);
    appendCommandStatus(result, status);

    if (session->isDone()) {
        audit::logAuthentication(client,
                                 session->getMechanism(),
                                 UserName(session->getPrincipalId(), db),
                                 status.code());
    } else {
        AuthenticationSession::swap(client, sessionGuard);
    }

    return status.isOK();
}

// The CyrusSaslCommands Enterprise initializer is dependent on PreSaslCommands
MONGO_INITIALIZER_WITH_PREREQUISITES(PreSaslCommands, ("NativeSaslServerCore"))
(InitializerContext*) {
    if (!sequenceContains(saslGlobalParams.authenticationMechanisms, "MONGODB-CR"))
        CmdAuthenticate::disableAuthMechanism("MONGODB-CR");

    if (!sequenceContains(saslGlobalParams.authenticationMechanisms, "MONGODB-X509"))
        CmdAuthenticate::disableAuthMechanism("MONGODB-X509");

    // For backwards compatibility, in 3.0 we are letting MONGODB-CR imply general
    // challenge-response auth and hence SCRAM-SHA-1 is enabled by either specifying
    // SCRAM-SHA-1 or MONGODB-CR in the authenticationMechanism server parameter.
    if (!sequenceContains(saslGlobalParams.authenticationMechanisms, "SCRAM-SHA-1") &&
        sequenceContains(saslGlobalParams.authenticationMechanisms, "MONGODB-CR"))
        saslGlobalParams.authenticationMechanisms.push_back("SCRAM-SHA-1");

    return Status::OK();
}

}  // namespace
}  // namespace mongo
