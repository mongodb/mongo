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

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/client/authenticate.h"
#include "mongo/client/sasl_client_session.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {
class BSONObj;
class SaslClientSession;

/**
 * Attempts to authenticate "client" using the SASL protocol.
 *
 * Do not use directly in client code.  Use the auth::authenticateClient() method, instead.
 *
 * Test against NULL for availability.  Client driver must be compiled with SASL support _and_
 * client application must have successfully executed mongo::runGlobalInitializersOrDie() or its
 * ilk to make this functionality available.
 *
 * The "credential" struct must have "mechanism" set. Other fields are mechanism-dependent:
 *   - "username": required for SCRAM, PLAIN, GSSAPI; omitted for X.509, AWS, OIDC.
 *   - "db": auth-source database; uses mechanism default ($external or admin) when absent.
 *   - "password": required for SCRAM and PLAIN; absent for other mechanisms.
 *   - "mechanismProperties": mechanism-specific options such as serviceName, serviceHostname,
 *       awsIamSessionToken, oidcAccessToken, digestPassword.
 *
 * Returns an OK status on success, and ErrorCodes::AuthenticationFailed if authentication is
 * rejected.  Other failures, all of which are tantamount to authentication failure, may also be
 * returned.
 */
extern Future<void> (*saslClientAuthenticate)(auth::RunCommandHook runCommand,
                                              const HostAndPort& hostname,
                                              const auth::Credential& credential);

/**
 * Extracts the payload field from "cmdObj", and store it into "*payload".
 *
 * Sets "*type" to the BSONType of the payload field in cmdObj.
 *
 * If the type of the payload field is String, the contents base64 decodes and
 * stores into "*payload".  If the type is BinData, the contents are stored directly
 * into "*payload".  In all other cases, returns
 */
Status saslExtractPayload(const BSONObj& cmdObj, std::string* payload, BSONType* type);

// Default log level on the client for SASL log messages.
constexpr int kSaslClientLogLevelDefault = 4;

/**
 * Configures and initializes "session" to perform the client side of a SASL conversation.
 *
 * Reads mechanism, username, password, and mechanism-specific properties from "credential".
 *
 * Returns Status::OK() on success.
 */
Status saslConfigureSession(SaslClientSession* session,
                            const HostAndPort& hostname,
                            const auth::Credential& credential);

/**
 * Continue a previously started sasl session and proceed until completion.
 */
Future<void> asyncSaslConversation(auth::RunCommandHook runCommand,
                                   const std::shared_ptr<SaslClientSession>& session,
                                   const BSONObj& saslCommandPrefix,
                                   const BSONObj& inputObj,
                                   std::string targetDatabase,
                                   int saslLogLevel);
}  // namespace mongo
