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

#pragma once

#include <functional>
#include <string>

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/future.h"
#include "mongo/util/md5.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

class BSONObj;

namespace auth {

using RunCommandHook = std::function<Future<BSONObj>(OpMsgRequest request)>;

/* Hook for legacy MONGODB-CR support provided by shell client only */
using AuthMongoCRHandler = std::function<Future<void>(RunCommandHook, const BSONObj&)>;
extern AuthMongoCRHandler authMongoCR;

/**
 * Names for supported authentication mechanisms.
 */

constexpr auto kMechanismMongoCR = "MONGODB-CR"_sd;
constexpr auto kMechanismMongoX509 = "MONGODB-X509"_sd;
constexpr auto kMechanismSaslPlain = "PLAIN"_sd;
constexpr auto kMechanismGSSAPI = "GSSAPI"_sd;
constexpr auto kMechanismScramSha1 = "SCRAM-SHA-1"_sd;
constexpr auto kMechanismScramSha256 = "SCRAM-SHA-256"_sd;
constexpr auto kInternalAuthFallbackMechanism = kMechanismScramSha1;

/**
 * Authenticate a user.
 *
 * Pass the default hostname for this client in through "hostname." If SSL is enabled and
 * there is a stored client subject name, pass that through the "clientSubjectName" parameter.
 * Otherwise, "clientSubjectName" will be silently ignored, pass in any string.
 *
 * The "params" BSONObj should be initialized with some of the fields below.  Which fields
 * are required depends on the mechanism, which is mandatory.
 *
 *     "mechanism": The std::string name of the sasl mechanism to use.  Mandatory.
 *     "user": The std::string name of the user to authenticate.  Mandatory.
 *     "db": The database target of the auth command, which identifies the location
 *         of the credential information for the user.  May be "$external" if
 *         credential information is stored outside of the mongo cluster.  Mandatory.
 *     "pwd": The password data.
 *     "digestPassword": Boolean, set to true if the "pwd" is undigested (default).
 *     "serviceName": The GSSAPI service name to use.  Defaults to "mongodb".
 *     "serviceHostname": The GSSAPI hostname to use.  Defaults to the name of the remote
 *          host.
 *
 * Other fields in "params" are silently ignored. A "params" object can be constructed
 * using the buildAuthParams() method.
 *
 * This function will return a future that will be filled with the final result of the
 * authentication command on success or a Status on error.
 */
Future<void> authenticateClient(const BSONObj& params,
                                const HostAndPort& hostname,
                                const std::string& clientSubjectName,
                                RunCommandHook runCommand);

/**
 * Authenticate as the __system user. All parameters are the same as authenticateClient above,
 * but the __system user's credentials will be filled in automatically.
 *
 * The "mechanismHint" parameter will force authentication with a specific mechanism
 * (e.g. SCRAM-SHA-256). If it is boost::none, then an isMaster will be called to negotiate
 * a SASL mechanism with the server.
 *
 * Because this may retry during cluster keyfile rollover, this may call the RunCommandHook more
 * than once, but will only call the AuthCompletionHandler once.
 */
Future<void> authenticateInternalClient(const std::string& clientSubjectName,
                                        boost::optional<std::string> mechanismHint,
                                        RunCommandHook runCommand);

/**
 * Sets the keys used by authenticateInternalClient - these should be a vector of raw passwords,
 * they will be digested and prepped appropriately by authenticateInternalClient depending
 * on what mechanism is used.
 */
void setInternalAuthKeys(const std::vector<std::string>& keys);

/**
 * Sets the parameters for non-password based internal authentication.
 */
void setInternalUserAuthParams(BSONObj obj);

/**
 * Returns whether there are multiple keys that will be tried while authenticating an internal
 * client (used for logging a startup warning).
 */
bool hasMultipleInternalAuthKeys();

/**
 * Returns whether there are any internal auth data set.
 */
bool isInternalAuthSet();

/**
 * Build a BSONObject representing parameters to be passed to authenticateClient(). Takes
 * the following fields:
 *
 *     @dbname: The database target of the auth command.
 *     @username: The std::string name of the user to authenticate.
 *     @passwordText: The std::string representing the user's password.
 *     @digestPassword: Set to true if the password is undigested.
 */
BSONObj buildAuthParams(StringData dbname,
                        StringData username,
                        StringData passwordText,
                        bool digestPassword);

/**
 * Run an isMaster exchange to negotiate a SASL mechanism for authentication.
 */
Future<std::string> negotiateSaslMechanism(RunCommandHook runCommand,
                                           const UserName& username,
                                           boost::optional<std::string> mechanismHint);

/**
 * Return the field name for the database containing credential information.
 */
StringData getSaslCommandUserDBFieldName();

/**
 * Return the field name for the user to authenticate.
 */
StringData getSaslCommandUserFieldName();

}  // namespace auth
}  // namespace mongo
