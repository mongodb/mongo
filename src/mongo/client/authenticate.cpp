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

#include "mongo/client/authenticate.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/json.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/config.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/sasl_command_constants.h"
#include "mongo/db/server_options.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/log.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/password_digest.h"

namespace mongo {
namespace auth {

using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;

using AuthRequest = StatusWith<RemoteCommandRequest>;

namespace {

const char* const kUserSourceFieldName = "userSource";
const BSONObj kGetNonceCmd = BSON("getnonce" << 1);

StatusWith<std::string> extractDBField(const BSONObj& params) {
    std::string db;
    if (params.hasField(kUserSourceFieldName)) {
        if (!bsonExtractStringField(params, kUserSourceFieldName, &db).isOK()) {
            return {ErrorCodes::AuthenticationFailed, "userSource field must contain a string"};
        }
    } else {
        if (!bsonExtractStringField(params, saslCommandUserDBFieldName, &db).isOK()) {
            return {ErrorCodes::AuthenticationFailed, "db field must contain a string"};
        }
    }

    return std::move(db);
}

//
// MONGODB-CR
//

Future<void> authMongoCRImpl(RunCommandHook cmd, const BSONObj& params) {
    return Status(ErrorCodes::AuthenticationFailed,
                  "MONGODB-CR support was removed in MongoDB 4.0");
}

//
// X-509
//

StatusWith<OpMsgRequest> createX509AuthCmd(const BSONObj& params, StringData clientName) {
    if (clientName.empty()) {
        return {ErrorCodes::AuthenticationFailed,
                "Please enable SSL on the client-side to use the MONGODB-X509 authentication "
                "mechanism."};
    }
    auto db = extractDBField(params);
    if (!db.isOK())
        return std::move(db.getStatus());

    std::string username;
    auto response = bsonExtractStringFieldWithDefault(
        params, saslCommandUserFieldName, clientName.toString(), &username);
    if (!response.isOK()) {
        return response;
    }
    if (username != clientName.toString()) {
        StringBuilder message;
        message << "Username \"";
        message << params[saslCommandUserFieldName].valuestr();
        message << "\" does not match the provided client certificate user \"";
        message << clientName.toString() << "\"";
        return {ErrorCodes::AuthenticationFailed, message.str()};
    }

    return OpMsgRequest::fromDBAndBody(db.getValue(),
                                       BSON("authenticate" << 1 << "mechanism"
                                                           << "MONGODB-X509"
                                                           << "user" << username));
}

// Use the MONGODB-X509 protocol to authenticate as "username." The certificate details
// have already been communicated automatically as part of the connect call.
Future<void> authX509(RunCommandHook runCommand, const BSONObj& params, StringData clientName) {
    invariant(runCommand);

    // Just 1 step: send authenticate command, receive response
    auto authRequest = createX509AuthCmd(params, clientName);
    if (!authRequest.isOK())
        return authRequest.getStatus();

    // The runCommand hook checks whether the command returned { ok: 1.0 }, and we don't need to
    // extract anything from the command payload, so this is just turning a Future<BSONObj>
    // into a Future<void>
    return runCommand(authRequest.getValue()).ignoreValue();
}

}  // namespace
//
// General Auth
//

Future<void> authenticateClient(const BSONObj& params,
                                const HostAndPort& hostname,
                                const std::string& clientName,
                                RunCommandHook runCommand) {
    auto errorHandler = [](Status status) {
        if (serverGlobalParams.transitionToAuth && !ErrorCodes::isNetworkError(status)) {
            // If auth failed in transitionToAuth, just pretend it succeeded.
            log() << "Failed to authenticate in transitionToAuth, falling back to no "
                     "authentication.";

            return Status::OK();
        }

        return status;
    };

    std::string mechanism;
    auto response = bsonExtractStringField(params, saslCommandMechanismFieldName, &mechanism);
    if (!response.isOK())
        return response;

    if (params.hasField(saslCommandUserDBFieldName) && params.hasField(kUserSourceFieldName)) {
        return Status(ErrorCodes::AuthenticationFailed,
                      "You cannot specify both 'db' and 'userSource'. Please use only 'db'.");
    }

    if (mechanism == kMechanismMongoCR)
        return authMongoCR(runCommand, params).onError(errorHandler);

#ifdef MONGO_CONFIG_SSL
    else if (mechanism == kMechanismMongoX509)
        return authX509(runCommand, params, clientName).onError(errorHandler);
#endif

    else if (saslClientAuthenticate != nullptr)
        return saslClientAuthenticate(runCommand, hostname, params).onError(errorHandler);

    return Status(ErrorCodes::AuthenticationFailed,
                  mechanism + " mechanism support not compiled into client library.");
};

AuthMongoCRHandler authMongoCR = authMongoCRImpl;

static stdx::mutex internalAuthKeysMutex;
static bool internalAuthSet = false;
static std::vector<std::string> internalAuthKeys;
static BSONObj internalAuthParams;

void setInternalAuthKeys(const std::vector<std::string>& keys) {
    stdx::lock_guard<stdx::mutex> lk(internalAuthKeysMutex);

    internalAuthKeys = keys;
    fassert(50996, internalAuthKeys.size() > 0);
    internalAuthSet = true;
}

void setInternalUserAuthParams(BSONObj obj) {
    stdx::lock_guard<stdx::mutex> lk(internalAuthKeysMutex);
    internalAuthParams = obj.getOwned();
    internalAuthKeys.clear();
    internalAuthSet = true;
}

bool hasMultipleInternalAuthKeys() {
    stdx::lock_guard<stdx::mutex> lk(internalAuthKeysMutex);
    return internalAuthSet && internalAuthKeys.size() > 1;
}

bool isInternalAuthSet() {
    stdx::lock_guard<stdx::mutex> lk(internalAuthKeysMutex);
    return internalAuthSet;
}

BSONObj getInternalAuthParams(size_t idx, const std::string& mechanism) {
    stdx::lock_guard<stdx::mutex> lk(internalAuthKeysMutex);
    if (!internalAuthSet) {
        return BSONObj();
    }

    // If we've set a specific BSONObj as the internal auth pararms, return it if the index
    // is zero (there are no alternate credentials if we've set a BSONObj explicitly).
    if (!internalAuthParams.isEmpty()) {
        return idx == 0 ? internalAuthParams : BSONObj();
    }

    // If the index is larger than the number of keys we know about then return an empty
    // BSONObj.
    if (idx + 1 > internalAuthKeys.size()) {
        return BSONObj();
    }

    auto password = internalAuthKeys.at(idx);
    if (mechanism == kMechanismScramSha1) {
        password = mongo::createPasswordDigest(
            internalSecurity.user->getName().getUser().toString(), password);
    }

    return BSON(saslCommandMechanismFieldName
                << mechanism << saslCommandUserDBFieldName
                << internalSecurity.user->getName().getDB() << saslCommandUserFieldName
                << internalSecurity.user->getName().getUser() << saslCommandPasswordFieldName
                << password << saslCommandDigestPasswordFieldName << false);
}

Future<std::string> negotiateSaslMechanism(RunCommandHook runCommand,
                                           const UserName& username,
                                           boost::optional<std::string> mechanismHint) {
    if (mechanismHint && !mechanismHint->empty()) {
        return Future<std::string>::makeReady(*mechanismHint);
    }

    const auto request =
        BSON("ismaster" << 1 << "saslSupportedMechs" << username.getUnambiguousName());
    return runCommand(OpMsgRequest::fromDBAndBody("admin"_sd, std::move(request)))
        .then([](BSONObj reply) -> Future<std::string> {
            auto mechsArrayObj = reply.getField("saslSupportedMechs");
            if (mechsArrayObj.type() != Array) {
                return Status{ErrorCodes::BadValue, "Expected array of SASL mechanism names"};
            }

            auto obj = mechsArrayObj.Obj();
            std::vector<std::string> availableMechanisms;
            for (const auto elem : obj) {
                if (elem.type() != String) {
                    return Status{ErrorCodes::BadValue, "Expected array of SASL mechanism names"};
                }
                availableMechanisms.push_back(elem.checkAndGetStringData().toString());
                // The drivers spec says that if SHA-256 is available then it MUST be selected
                // as the SASL mech.
                if (availableMechanisms.back() == kMechanismScramSha256) {
                    return availableMechanisms.back();
                }
            }

            return availableMechanisms.empty() ? kInternalAuthFallbackMechanism.toString()
                                               : availableMechanisms.front();
        });
}

Future<void> authenticateInternalClient(const std::string& clientSubjectName,
                                        boost::optional<std::string> mechanismHint,
                                        RunCommandHook runCommand) {
    return negotiateSaslMechanism(runCommand, internalSecurity.user->getName(), mechanismHint)
        .then([runCommand, clientSubjectName](std::string mechanism) -> Future<void> {
            auto params = getInternalAuthParams(0, mechanism);
            if (params.isEmpty()) {
                return Status(ErrorCodes::BadValue,
                              "Missing authentication parameters for internal user auth");
            }
            return authenticateClient(params, HostAndPort(), clientSubjectName, runCommand)
                .onError<ErrorCodes::AuthenticationFailed>(
                    [runCommand, clientSubjectName, mechanism](Status status) -> Future<void> {
                        auto altCreds = getInternalAuthParams(1, mechanism);
                        if (!altCreds.isEmpty()) {
                            return authenticateClient(
                                altCreds, HostAndPort(), clientSubjectName, runCommand);
                        }
                        return status;
                    });
        });
}

BSONObj buildAuthParams(StringData dbname,
                        StringData username,
                        StringData passwordText,
                        bool digestPassword) {
    return BSON(saslCommandMechanismFieldName
                << "SCRAM-SHA-1" << saslCommandUserDBFieldName << dbname << saslCommandUserFieldName
                << username << saslCommandPasswordFieldName << passwordText
                << saslCommandDigestPasswordFieldName << digestPassword);
}

StringData getSaslCommandUserDBFieldName() {
    return saslCommandUserDBFieldName;
}

StringData getSaslCommandUserFieldName() {
    return saslCommandUserFieldName;
}

}  // namespace auth
}  // namespace mongo
