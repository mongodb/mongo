/*
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/auth/native_sasl_authentication_session.h"

#include <boost/range/size.hpp>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/db/auth/authz_session_external_state_mock.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/auth/sasl_plain_server_conversation.h"
#include "mongo/db/auth/sasl_scramsha1_server_conversation.h"
#include "mongo/db/commands.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::unique_ptr;

namespace {
SaslAuthenticationSession* createNativeSaslAuthenticationSession(AuthorizationSession* authzSession,
                                                                 const std::string& mechanism) {
    return new NativeSaslAuthenticationSession(authzSession);
}

MONGO_INITIALIZER(NativeSaslServerCore)(InitializerContext* context) {
    if (saslGlobalParams.hostName.empty())
        saslGlobalParams.hostName = getHostNameCached();
    if (saslGlobalParams.serviceName.empty())
        saslGlobalParams.serviceName = "mongodb";

    SaslAuthenticationSession::create = createNativeSaslAuthenticationSession;
    return Status::OK();
}

// PostSaslCommands is reversely dependent on CyrusSaslCommands having been run
MONGO_INITIALIZER_WITH_PREREQUISITES(PostSaslCommands, ("NativeSaslServerCore"))
(InitializerContext*) {
    AuthorizationManager authzManager(stdx::make_unique<AuthzManagerExternalStateMock>());
    std::unique_ptr<AuthorizationSession> authzSession = authzManager.makeAuthorizationSession();

    for (size_t i = 0; i < saslGlobalParams.authenticationMechanisms.size(); ++i) {
        const std::string& mechanism = saslGlobalParams.authenticationMechanisms[i];
        if (mechanism == "MONGODB-CR" || mechanism == "MONGODB-X509") {
            // Not a SASL mechanism; no need to smoke test built-in mechanisms.
            continue;
        }
        unique_ptr<SaslAuthenticationSession> session(
            SaslAuthenticationSession::create(authzSession.get(), mechanism));
        Status status = session->start(
            "test", mechanism, saslGlobalParams.serviceName, saslGlobalParams.hostName, 1, true);
        if (!status.isOK())
            return status;
    }

    return Status::OK();
}
}  // namespace

NativeSaslAuthenticationSession::NativeSaslAuthenticationSession(AuthorizationSession* authzSession)
    : SaslAuthenticationSession(authzSession), _mechanism("") {}

NativeSaslAuthenticationSession::~NativeSaslAuthenticationSession() {}

Status NativeSaslAuthenticationSession::start(StringData authenticationDatabase,
                                              StringData mechanism,
                                              StringData serviceName,
                                              StringData serviceHostname,
                                              int64_t conversationId,
                                              bool autoAuthorize) {
    fassert(18626, conversationId > 0);

    if (_conversationId != 0) {
        return Status(ErrorCodes::AlreadyInitialized,
                      "Cannot call start() twice on same NativeSaslAuthenticationSession.");
    }

    _authenticationDatabase = authenticationDatabase.toString();
    _mechanism = mechanism.toString();
    _serviceName = serviceName.toString();
    _serviceHostname = serviceHostname.toString();
    _conversationId = conversationId;
    _autoAuthorize = autoAuthorize;

    if (mechanism == "PLAIN") {
        _saslConversation.reset(new SaslPLAINServerConversation(this));
    } else if (mechanism == "SCRAM-SHA-1") {
        _saslConversation.reset(new SaslSCRAMSHA1ServerConversation(this));
    } else {
        return Status(ErrorCodes::BadValue,
                      mongoutils::str::stream() << "SASL mechanism " << mechanism
                                                << " is not supported");
    }

    return Status::OK();
}

Status NativeSaslAuthenticationSession::step(StringData inputData, std::string* outputData) {
    if (!_saslConversation) {
        return Status(ErrorCodes::BadValue,
                      mongoutils::str::stream()
                          << "The authentication session has not been properly initialized");
    }

    StatusWith<bool> status = _saslConversation->step(inputData, outputData);
    if (status.isOK()) {
        _done = status.getValue();
    } else {
        _done = true;
    }
    return status.getStatus();
}

std::string NativeSaslAuthenticationSession::getPrincipalId() const {
    return _saslConversation->getPrincipalId();
}

const char* NativeSaslAuthenticationSession::getMechanism() const {
    return _mechanism.c_str();
}

}  // namespace mongo
