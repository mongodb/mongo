/*    Copyright 2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
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

namespace {

const char* const kUserSourceFieldName = "userSource";
const BSONObj kGetNonceCmd = BSON("getnonce" << 1);

bool isOk(const BSONObj& o) {
    return getStatusFromCommandResult(o).isOK();
}

BSONObj getFallbackAuthParams(const BSONObj& params) {
    if (params["fallbackParams"].type() != Object) {
        return BSONObj();
    }
    return params["fallbackParams"].Obj();
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

AuthRequest createMongoCRGetNonceCmd(const BSONObj& params) {
    auto db = extractDBField(params);
    if (!db.isOK())
        return std::move(db.getStatus());

    auto request = RemoteCommandRequest();
    request.cmdObj = kGetNonceCmd;
    request.dbname = db.getValue();

    return std::move(request);
}

AuthRequest createMongoCRAuthenticateCmd(const BSONObj& params, StringData nonce) {
    std::string username;
    auto response = bsonExtractStringField(params, saslCommandUserFieldName, &username);
    if (!response.isOK())
        return response;

    std::string password;
    response = bsonExtractStringField(params, saslCommandPasswordFieldName, &password);
    if (!response.isOK())
        return response;

    bool shouldDigest;
    response = bsonExtractBooleanFieldWithDefault(
        params, saslCommandDigestPasswordFieldName, true, &shouldDigest);
    if (!response.isOK())
        return response;

    std::string digested = password;
    if (shouldDigest)
        digested = createPasswordDigest(username, password);

    auto db = extractDBField(params);
    if (!db.isOK())
        return std::move(db.getStatus());

    auto request = RemoteCommandRequest();
    request.dbname = db.getValue();

    BSONObjBuilder b;
    {
        b << "authenticate" << 1 << "nonce" << nonce << "user" << username;
        md5digest d;
        {
            md5_state_t st;
            md5_init(&st);
            md5_append(&st, reinterpret_cast<const md5_byte_t*>(nonce.rawData()), nonce.size());
            md5_append(&st, reinterpret_cast<const md5_byte_t*>(username.c_str()), username.size());
            md5_append(&st, reinterpret_cast<const md5_byte_t*>(digested.c_str()), digested.size());
            md5_finish(&st, d);
        }
        b << "key" << digestToString(d);
        request.cmdObj = b.obj();
    }
    return std::move(request);
}

void authMongoCR(RunCommandHook runCommand, const BSONObj& params, AuthCompletionHandler handler) {
    invariant(runCommand);
    invariant(handler);

    // Step 1: send getnonce command, receive nonce
    auto nonceRequest = createMongoCRGetNonceCmd(params);
    if (!nonceRequest.isOK())
        return handler(std::move(nonceRequest.getStatus()));

    runCommand(nonceRequest.getValue(), [runCommand, params, handler](AuthResponse response) {
        if (!response.isOK())
            return handler(std::move(response));

        // Ensure response was valid
        std::string nonce;
        BSONObj nonceResponse = response.getValue().data;
        auto valid = bsonExtractStringField(nonceResponse, "nonce", &nonce);
        if (!valid.isOK())
            return handler({ErrorCodes::AuthenticationFailed,
                            "Invalid nonce response: " + nonceResponse.toString()});

        // Step 2: send authenticate command, receive response
        auto authRequest = createMongoCRAuthenticateCmd(params, nonce);
        if (!authRequest.isOK())
            return handler(std::move(authRequest.getStatus()));

        runCommand(authRequest.getValue(), handler);
    });
}

//
// X-509
//

AuthRequest createX509AuthCmd(const BSONObj& params, StringData clientName) {
    auto db = extractDBField(params);
    if (!db.isOK())
        return std::move(db.getStatus());

    auto request = RemoteCommandRequest();
    request.dbname = db.getValue();

    std::string username;
    auto response = bsonExtractStringField(params, saslCommandUserFieldName, &username);
    if (!response.isOK())
        return response;

    if (clientName.toString() == "") {
        return {ErrorCodes::AuthenticationFailed,
                "Please enable SSL on the client-side to use the MONGODB-X509 authentication "
                "mechanism."};
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
    return (response == ErrorCodes::AuthenticationFailed && serverGlobalParams.transitionToAuth);
}

void auth(RunCommandHook runCommand,
          const BSONObj& params,
          StringData hostname,
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

bool needsFallback(const AuthResponse& response) {
    // TODO: BadValue is sometimes returned for auth failures with unsupported mechanisms
    // in 2.6 servers. We should investigate removing this BadValue check eventually.
    return (response == ErrorCodes::BadValue || response == ErrorCodes::CommandNotFound);
}

void asyncAuth(RunCommandHook runCommand,
               const BSONObj& params,
               StringData hostname,
               StringData clientName,
               AuthCompletionHandler handler) {
    auth(runCommand,
         params,
         hostname,
         clientName,
         [runCommand, params, hostname, clientName, handler](AuthResponse response) {
             // If auth failed, try again with fallback params when appropriate
             if (needsFallback(response)) {
                 return auth(runCommand,
                             std::move(getFallbackAuthParams(params)),
                             hostname,
                             clientName,
                             handler);
             }

             // otherwise, call handler
             return handler(std::move(response));
         });
}

}  // namespace

void authenticateClient(const BSONObj& params,
                        StringData hostname,
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
            uassertStatusOK(response);

            auto serverResponse = response.getValue().data;
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
