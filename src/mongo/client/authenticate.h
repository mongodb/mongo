// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
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

namespace [[MONGO_MOD_PUBLIC]] auth {
using namespace std::literals::string_view_literals;

using RunCommandHook = std::function<Future<BSONObj>(OpMsgRequest request)>;

constexpr auto kSaslSupportedMechanisms = "saslSupportedMechs"sv;
constexpr auto kSpeculativeAuthenticate = "speculativeAuthenticate"sv;
constexpr auto kClusterAuthenticate = "clusterAuthenticate"sv;
constexpr auto kAuthenticateCommand = "authenticate"sv;

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


}  // namespace auth
}  // namespace mongo
