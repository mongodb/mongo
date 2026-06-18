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

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/credential.h"
#include "mongo/client/internal_auth.h"
#include "mongo/client/sasl_client_session.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/database_name.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class BSONObj;
class MongoURI;

namespace MONGO_MOD_PUBLIC auth {

using RunCommandHook = std::function<Future<BSONObj>(OpMsgRequest request)>;

constexpr auto kSaslSupportedMechanisms = "saslSupportedMechs"_sd;
constexpr auto kSpeculativeAuthenticate = "speculativeAuthenticate"_sd;
constexpr auto kClusterAuthenticate = "clusterAuthenticate"_sd;
constexpr auto kAuthenticateCommand = "authenticate"_sd;

/**
 * On replication step down, should the current connection be killed or left open.
 */
enum class StepDownBehavior { kKillConnection, kKeepConnectionOpen };

/**
 * Provider of SASL credentials for internal authentication purposes.
 */
class InternalAuthParametersProvider {
public:
    virtual ~InternalAuthParametersProvider() = default;

    /**
     * Get the credential for a given SASL mechanism.
     *
     * If there are multiple entries for a mechanism, supports retrieval by index. Used when
     * rotating the security key. Returns boost::none if no credential is available at that index.
     */
    virtual boost::optional<Credential> get(size_t index, std::string_view mechanism) = 0;
};

std::shared_ptr<InternalAuthParametersProvider> createDefaultInternalAuthProvider();

/**
 * Authenticate a user.
 *
 * Pass the default hostname for this client in through "hostname." If SSL is enabled and
 * there is a stored client subject name, pass that through the "clientSubjectName" parameter.
 * Otherwise, "clientSubjectName" will be silently ignored, pass in any string.
 *
 * The "credential" struct must have "mechanism" set. Other fields are mechanism-dependent:
 *   - "username": required for SCRAM, PLAIN, GSSAPI; omitted for X.509, AWS, OIDC.
 *   - "db": auth-source database; uses mechanism default ($external or admin) when absent.
 *   - "password": required for SCRAM and PLAIN; absent for other mechanisms.
 *   - "mechanismProperties": mechanism-specific options such as serviceName, serviceHostname,
 *       awsIamSessionToken, oidcAccessToken, digestPassword.
 *
 * This function will return a future that will be filled with the final result of the
 * authentication command on success or a Status on error.
 */
Future<void> authenticateClient(const Credential& credential,
                                const HostAndPort& hostname,
                                const std::string& clientSubjectName,
                                RunCommandHook runCommand);

/**
 * Authenticate as the __system user. All parameters are the same as authenticateClient above,
 * but the __system user's credentials will be filled in automatically.
 *
 * The "mechanismHint" parameter will force authentication with a specific mechanism
 * (e.g. SCRAM-SHA-256). If it is boost::none, then a "hello" will be called to negotiate
 * a SASL mechanism with the server.
 *
 * The "stepDownBehavior" parameter controls whether replication will kill the connection on
 * stepdown.
 *
 * Because this may retry during cluster keyfile rollover, this may call the RunCommandHook more
 * than once, but will only call the AuthCompletionHandler once.
 */
Future<void> authenticateInternalClient(
    const std::string& clientSubjectName,
    const HostAndPort& remote,
    boost::optional<std::string> mechanismHint,
    StepDownBehavior stepDownBehavior,
    RunCommandHook runCommand,
    std::shared_ptr<InternalAuthParametersProvider> internalParamsProvider);

/**
 * Build a BSONObject representing parameters to be passed to authenticateClient(). Takes
 * the following fields:
 *
 *     @dbname: The database target of the auth command.
 *     @username: The std::string name of the user to authenticate.
 *     @passwordText: The std::string representing the user's password.
 *     @mechanism: The std::string authentication mechanism to be used
 */
BSONObj buildAuthParams(const DatabaseName& dbname,
                        std::string_view username,
                        std::string_view passwordText,
                        std::string_view mechanism);

/**
 * Run a "hello" exchange to negotiate a SASL mechanism for authentication.
 */
Future<std::string> negotiateSaslMechanism(RunCommandHook runCommand,
                                           const UserName& username,
                                           boost::optional<std::string> mechanismHint,
                                           StepDownBehavior stepDownBehavior);

/**
 * Return the field name for the database containing credential information.
 */
std::string_view getSaslCommandUserDBFieldName();

/**
 * Return the field name for the user to authenticate.
 */
std::string_view getSaslCommandUserFieldName();

/**
 * Which type of speculative authentication was performed (if any).
 */
enum class SpeculativeAuthType {
    kNone,
    kAuthenticate,
    kSaslStart,
};

/**
 * Constructs a "speculativeAuthenticate" or "speculativeSaslStart" payload for an "hello" request
 * based on a given URI.
 */
SpeculativeAuthType speculateAuth(BSONObjBuilder* helloRequestBuilder,
                                  const MongoURI& uri,
                                  std::shared_ptr<SaslClientSession>* saslClientSession);

/**
 * Constructs a "speculativeAuthenticate" or "speculativeSaslStart" payload for an "hello" request
 * using internal (intracluster) authentication.
 */
SpeculativeAuthType speculateInternalAuth(const HostAndPort& remoteHost,
                                          BSONObjBuilder* helloRequestBuilder,
                                          std::shared_ptr<SaslClientSession>* saslClientSession);


}  // namespace MONGO_MOD_PUBLIC auth
}  // namespace mongo
