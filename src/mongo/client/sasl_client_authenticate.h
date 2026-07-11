// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
