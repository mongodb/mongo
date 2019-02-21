
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
#include "mongo/db/auth/sasl_command_constants.h"
#include "mongo/db/server_options.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/log.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/password_digest.h"

namespace mongo {
namespace auth {

using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;

using AuthRequest = StatusWith<RemoteCommandRequest>;

const char* const kMechanismMongoCR = "MONGODB-CR";
const char* const kMechanismMongoX509 = "MONGODB-X509";
const char* const kMechanismSaslPlain = "PLAIN";
const char* const kMechanismGSSAPI = "GSSAPI";
const char* const kMechanismScramSha1 = "SCRAM-SHA-1";
const char* const kMechanismScramSha256 = "SCRAM-SHA-256";

namespace {

const char* const kUserSourceFieldName = "userSource";
const BSONObj kGetNonceCmd = BSON("getnonce" << 1);

bool isOk(const BSONObj& o) {
    return getStatusFromCommandResult(o).isOK();
}

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

void authMongoCRImpl(RunCommandHook cmd, const BSONObj& params, AuthCompletionHandler handler) {
    handler({ErrorCodes::AuthenticationFailed, "MONGODB-CR support was removed in MongoDB 3.8"});
}

//
// X-509
//

AuthRequest createX509AuthCmd(const BSONObj& params, StringData clientName) {
    if (clientName.empty()) {
        return {ErrorCodes::AuthenticationFailed,
                "Please enable SSL on the client-side to use the MONGODB-X509 authentication "
                "mechanism."};
    }
    auto db = extractDBField(params);
    if (!db.isOK())
        return std::move(db.getStatus());

    auto request = RemoteCommandRequest();
    request.dbname = db.getValue();

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

    request.cmdObj = BSON("authenticate" << 1 << "mechanism"
                                         << "MONGODB-X509"
                                         << "user"
                                         << username);
    return std::move(request);
}

// Use the MONGODB-X509 protocol to authenticate as "username." The certificate details
// have already been communicated automatically as part of the connect call.
void authX509(RunCommandHook runCommand,
              const BSONObj& params,
              StringData clientName,
              AuthCompletionHandler handler) {
    invariant(runCommand);
    invariant(handler);

    // Just 1 step: send authenticate command, receive response
    auto authRequest = createX509AuthCmd(params, clientName);
    if (!authRequest.isOK())
        return handler(std::move(authRequest.getStatus()));

    runCommand(authRequest.getValue(), handler);
}

//
// General Auth
//

bool isFailedAuthOk(const AuthResponse& response) {
    return (response.status == ErrorCodes::AuthenticationFailed &&
            serverGlobalParams.transitionToAuth);
}

void auth(RunCommandHook runCommand,
          const BSONObj& params,
          const HostAndPort& hostname,
          StringData clientName,
          AuthCompletionHandler handler) {
    std::string mechanism;
    auto authCompletionHandler = [handler](AuthResponse response) {
        if (isFailedAuthOk(response)) {
            // If auth failed in transitionToAuth, just pretend it succeeded.
            log() << "Failed to authenticate in transitionToAuth, falling back to no "
                     "authentication.";

            // We need to mock a successful AuthResponse.
            return handler(
                AuthResponse(RemoteCommandResponse(BSON("ok" << 1), BSONObj(), Milliseconds(0))));
        }

        // otherwise, call handler
        return handler(std::move(response));
    };
    auto response = bsonExtractStringField(params, saslCommandMechanismFieldName, &mechanism);
    if (!response.isOK())
        return handler(std::move(response));

    if (params.hasField(saslCommandUserDBFieldName) && params.hasField(kUserSourceFieldName)) {
        return handler({ErrorCodes::AuthenticationFailed,
                        "You cannot specify both 'db' and 'userSource'. Please use only 'db'."});
    }

    if (mechanism == kMechanismMongoCR)
        return authMongoCR(runCommand, params, authCompletionHandler);

#ifdef MONGO_CONFIG_SSL
    else if (mechanism == kMechanismMongoX509)
        return authX509(runCommand, params, clientName, authCompletionHandler);
#endif

    else if (saslClientAuthenticate != nullptr)
        return saslClientAuthenticate(runCommand, hostname, params, authCompletionHandler);

    return handler({ErrorCodes::AuthenticationFailed,
                    mechanism + " mechanism support not compiled into client library."});
};

void asyncAuth(RunCommandHook runCommand,
               const BSONObj& params,
               const HostAndPort& hostname,
               StringData clientName,
               AuthCompletionHandler handler) {
    auth(runCommand, params, hostname, clientName, std::move(handler));
}

}  // namespace

AuthMongoCRHandler authMongoCR = authMongoCRImpl;

void authenticateClient(const BSONObj& params,
                        const HostAndPort& hostname,
                        StringData clientName,
                        RunCommandHook runCommand,
                        AuthCompletionHandler handler) {
    if (handler) {
        // Run asynchronously
        return asyncAuth(std::move(runCommand), params, hostname, clientName, std::move(handler));
    } else {
        // Run synchronously through async framework
        // NOTE: this assumes that runCommand executes synchronously.
        asyncAuth(runCommand, params, hostname, clientName, [](AuthResponse response) {
            // DBClient expects us to throw in case of an auth error.
            uassertStatusOK(response.status);

            auto serverResponse = response.data;
            uassert(ErrorCodes::AuthenticationFailed,
                    serverResponse["errmsg"].str(),
                    isOk(serverResponse));
        });
    }
}

BSONObj buildAuthParams(StringData dbname,
                        StringData username,
                        StringData passwordText,
                        bool digestPassword) {
    return BSON(saslCommandMechanismFieldName << "SCRAM-SHA-1" << saslCommandUserDBFieldName
                                              << dbname
                                              << saslCommandUserFieldName
                                              << username
                                              << saslCommandPasswordFieldName
                                              << passwordText
                                              << saslCommandDigestPasswordFieldName
                                              << digestPassword);
}

StringData getSaslCommandUserDBFieldName() {
    return saslCommandUserDBFieldName;
}

StringData getSaslCommandUserFieldName() {
    return saslCommandUserFieldName;
}

}  // namespace auth
}  // namespace mongo
