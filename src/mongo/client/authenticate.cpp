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


#include "mongo/client/authenticate.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/client/internal_auth.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/sasl_command_constants.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/connection_health_metrics_parameter_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/str.h"

#include <string_view>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl


namespace mongo {
namespace auth {

using executor::RemoteCommandRequest;

using AuthRequest = StatusWith<RemoteCommandRequest>;

namespace {

const BSONObj kGetNonceCmd = BSON("getnonce" << 1);

}  // namespace

namespace {

//
// X-509
//

StatusWith<OpMsgRequest> createX509AuthCmd(const Credential& cred, std::string_view clientName) {
    if (clientName.empty()) {
        return {ErrorCodes::AuthenticationFailed,
                "Please enable SSL on the client-side to use the MONGODB-X509 authentication "
                "mechanism."};
    }

    const std::string& username =
        (cred.username && !cred.username->empty()) ? *cred.username : std::string{clientName};
    if (username != std::string{clientName}) {
        return {ErrorCodes::AuthenticationFailed,
                str::stream() << "Username \"" << username
                              << "\" does not match the provided client certificate user \""
                              << clientName << "\""};
    }

    return OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired /* db is not tenanted */,
        AuthDatabaseNameUtil::deserialize(cred.db.value_or("$external")),
        BSON("authenticate" << 1 << "mechanism" << kMechanismMongoX509 << "user" << username));
}

// Use the MONGODB-X509 protocol to authenticate as "username." The certificate details
// have already been communicated automatically as part of the connect call.
Future<void> authX509(RunCommandHook runCommand,
                      const HostAndPort& hostname,
                      const Credential& cred,
                      std::string_view clientName) {
    invariant(runCommand);

    auto swAuthRequest = createX509AuthCmd(cred, clientName);
    if (!swAuthRequest.isOK())
        return swAuthRequest.getStatus();

    auto mechCounter = authCounter.getEgressMechanismCounter(kMechanismMongoX509);
    mechCounter.incAuthenticateSent();

    auto argsBlock = std::make_tuple(
        hostname, cred, std::string(clientName), cred.db.value_or("$external"), mechCounter);
    auto sharedBlock = std::make_shared<decltype(argsBlock)>(std::move(argsBlock));

    auto metricsRecorder = std::make_shared<AuthMetricsRecorder>();

    // The runCommand hook checks whether the command returned { ok: 1.0 }, and we don't need to
    // extract anything from the command payload, so this is just turning a Future<BSONObj>
    // into a Future<void>
    return runCommand(swAuthRequest.getValue())
        .then([metricsRecorder, sharedBlock](const BSONObj& obj) {
            BSONObj metrics = metricsRecorder->captureEgress();
            auto [hostname, cred, clientName, targetDb, mechCounter] = *sharedBlock.get();
            mechCounter.incEgressAuthenticateSuccessful();
            if (gEnableDetailedConnectionHealthMetricLogLines.load()) {
                LOGV2(10748708,
                      "Authentication to remote host succeeded using MONGODB-X509",
                      "hostname"_attr = hostname,
                      "username"_attr = cred.username.value_or(""),
                      "subjectName"_attr = clientName,
                      "targetDatabase"_attr = targetDb,
                      "mechanism"_attr = kMechanismMongoX509,
                      "result"_attr = Status::OK().code(),
                      "metrics"_attr = metrics);
            }
        })
        .onError([metricsRecorder, sharedBlock](const Status& status) {
            BSONObj metrics = metricsRecorder->captureEgress();
            auto [hostname, cred, clientName, targetDb, _] = *sharedBlock.get();
            if (gEnableDetailedConnectionHealthMetricLogLines.load()) {
                LOGV2(10748707,
                      "Authentication to remote host failed using MONGODB-X509",
                      "hostname"_attr = hostname,
                      "username"_attr = cred.username.value_or(""),
                      "subjectName"_attr = clientName,
                      "targetDatabase"_attr = targetDb,
                      "mechanism"_attr = kMechanismMongoX509,
                      "error"_attr = redact(status),
                      "result"_attr = status.code(),
                      "metrics"_attr = metrics);
            }
            return status;
        })
        .ignoreValue();
}

class DefaultInternalAuthParametersProvider : public InternalAuthParametersProvider {
public:
    ~DefaultInternalAuthParametersProvider() override = default;

    boost::optional<Credential> get(size_t index, std::string_view mechanism) final {
        return getInternalAuthParams(index, mechanism);
    }
};

}  // namespace

std::shared_ptr<InternalAuthParametersProvider> createDefaultInternalAuthProvider() {
    return std::make_shared<DefaultInternalAuthParametersProvider>();
}


//
// General Auth
//

Future<void> authenticateClient(const Credential& credential,
                                const HostAndPort& hostname,
                                const std::string& clientName,
                                RunCommandHook runCommand) {
    auto errorHandler = [](Status status) {
        if (serverGlobalParams.transitionToAuth && !ErrorCodes::isNetworkError(status)) {
            // If auth failed in transitionToAuth, just pretend it succeeded.
            LOGV2(20108,
                  "Failed to authenticate in transitionToAuth, "
                  "falling back to no authentication");

            return Status::OK();
        }

        return status;
    };

#ifdef MONGO_CONFIG_SSL
    if (credential.mechanism == AuthMechanism::kMongoX509)
        return authX509(runCommand, hostname, credential, clientName).onError(errorHandler);
#endif

    if (saslClientAuthenticate != nullptr)
        return saslClientAuthenticate(runCommand, hostname, credential).onError(errorHandler);

    return Status(ErrorCodes::AuthenticationFailed,
                  str::stream() << toString(credential.mechanism)
                                << " mechanism support not compiled into client library.");
};

Future<std::string> negotiateSaslMechanism(RunCommandHook runCommand,
                                           const UserName& username,
                                           boost::optional<std::string> mechanismHint,
                                           StepDownBehavior stepDownBehavior) {
    if (mechanismHint && !mechanismHint->empty()) {
        return Future<std::string>::makeReady(*mechanismHint);
    }

    BSONObjBuilder builder;
    builder.append("hello", 1);
    builder.append("saslSupportedMechs", username.getUnambiguousName());
    if (stepDownBehavior == StepDownBehavior::kKeepConnectionOpen) {
        builder.append("hangUpOnStepDown", false);
    }
    const auto request = builder.obj();

    return runCommand(OpMsgRequestBuilder::create(
                          auth::ValidatedTenancyScope::kNotRequired /* admin is not per-tenant. */,
                          DatabaseName::kAdmin,
                          request))
        .then([](BSONObj reply) -> Future<std::string> {
            auto mechsArrayObj = reply.getField("saslSupportedMechs");
            if (mechsArrayObj.type() != BSONType::array) {
                return Status{ErrorCodes::BadValue, "Expected array of SASL mechanism names"};
            }

            auto obj = mechsArrayObj.Obj();
            std::vector<std::string> availableMechanisms;
            for (const auto& elem : obj) {
                if (elem.type() != BSONType::string) {
                    return Status{ErrorCodes::BadValue, "Expected array of SASL mechanism names"};
                }
                availableMechanisms.push_back(std::string{elem.checkAndGetStringData()});
                // The drivers spec says that if SHA-256 is available then it MUST be selected
                // as the SASL mech.
                if (availableMechanisms.back() == kMechanismScramSha256) {
                    return availableMechanisms.back();
                }
            }

            return availableMechanisms.empty() ? std::string{kInternalAuthFallbackMechanism}
                                               : availableMechanisms.front();
        });
}

Future<void> authenticateInternalClient(
    const std::string& clientSubjectName,
    const HostAndPort& remote,
    boost::optional<std::string> mechanismHint,
    StepDownBehavior stepDownBehavior,
    RunCommandHook runCommand,
    std::shared_ptr<InternalAuthParametersProvider> internalParamsProvider) {
    auto systemUser = internalSecurity.getUser();
    return negotiateSaslMechanism(
               runCommand, (*systemUser)->getName(), mechanismHint, stepDownBehavior)
        .then([runCommand, clientSubjectName, remote, internalParamsProvider](
                  std::string mechanism) -> Future<void> {
            auto cred = internalParamsProvider->get(0, mechanism);
            if (!cred) {
                return Status(ErrorCodes::BadValue,
                              "Missing authentication parameters for internal user auth");
            }
            return authenticateClient(*cred, remote, clientSubjectName, runCommand)
                .onError<ErrorCodes::AuthenticationFailed>(
                    [runCommand, clientSubjectName, remote, mechanism, internalParamsProvider](
                        Status status) -> Future<void> {
                        auto altCred = internalParamsProvider->get(1, mechanism);
                        if (!altCred)
                            return status;
                        return authenticateClient(*altCred, remote, clientSubjectName, runCommand);
                    });
        });
}

BSONObj buildAuthParams(const DatabaseName& dbname,
                        std::string_view username,
                        std::string_view passwordText,
                        std::string_view mechanism) {
    // Direct authentication expects no tenantId to be present.
    fassert(8032000, dbname.tenantId() == boost::none);
    // Because we assert above there is no TenantId, serialiazing the `dbname` will never include
    // a tenantId as part of the dbName regardless of the serialization context.
    SerializationContext sc = SerializationContext::stateDefault();
    return BSON(saslCommandMechanismFieldName << mechanism << saslCommandUserDBFieldName
                                              << DatabaseNameUtil::serialize(dbname, sc)
                                              << saslCommandUserFieldName << username
                                              << saslCommandPasswordFieldName << passwordText);
}

std::string_view getSaslCommandUserDBFieldName() {
    return saslCommandUserDBFieldName;
}

std::string_view getSaslCommandUserFieldName() {
    return saslCommandUserFieldName;
}

namespace {

StatusWith<std::shared_ptr<SaslClientSession>> _speculateSaslStart(
    BSONObjBuilder* helloRequestBuilder, const Credential& credential, const HostAndPort& host) {
    if (credential.mechanism == AuthMechanism::kSaslPlain) {
        return {ErrorCodes::BadValue, "PLAIN mechanism not supported with speculativeSaslStart"};
    }

    const auto mechStr = toString(credential.mechanism);
    const auto authDB = credential.db.value_or(std::string{saslDefaultDBName});
    std::shared_ptr<SaslClientSession> session(SaslClientSession::create(std::string{mechStr}));
    auto status = saslConfigureSession(session.get(), host, credential);
    if (!status.isOK()) {
        return status;
    }

    std::string payload;

    auto mechCounter = authCounter.getEgressMechanismCounter(mechStr);
    mechCounter.incAuthenticateSent();
    mechCounter.incSpeculativeAuthenticateSent();

    AuthMetricsRecorder metricsRecorder;
    status = session->step("", &payload);
    if (!status.isOK()) {
        auto metrics = metricsRecorder.captureEgress();
        if (gEnableDetailedConnectionHealthMetricLogLines.load()) {
            LOGV2(10748709,
                  "Speculative authentication to remote host failed",
                  "hostname"_attr = host,
                  "username"_attr = credential.username.value_or(""),
                  "targetDatabase"_attr = authDB,
                  "mechanism"_attr = mechStr,
                  "error"_attr = redact(status),
                  "result"_attr = status.code(),
                  "metrics"_attr = metrics);
        }
        return status;
    }
    mechCounter.incEgressAuthenticateSuccessful();
    mechCounter.incEgressSpeculativeAuthenticateSuccessful();
    auto metrics = metricsRecorder.captureEgress();
    if (gEnableDetailedConnectionHealthMetricLogLines.load()) {
        LOGV2(10748710,
              "Speculative authentication to remote host succeeded",
              "hostname"_attr = host,
              "username"_attr = credential.username.value_or(""),
              "targetDatabase"_attr = authDB,
              "mechanism"_attr = mechStr,
              "result"_attr = Status::OK().code(),
              "metrics"_attr = metrics);
    }

    BSONObjBuilder saslStart;
    saslStart.append("saslStart", 1);
    saslStart.append("mechanism", mechStr);
    saslStart.appendBinData("payload", int(payload.size()), BinDataGeneral, payload.c_str());
    saslStart.append("db", authDB);
    saslStart.append("options", BSON(saslCommandOptionSkipEmptyExchange << true));
    helloRequestBuilder->append(kSpeculativeAuthenticate, saslStart.obj());

    return session;
}

StatusWith<SpeculativeAuthType> _speculateAuth(
    BSONObjBuilder* helloRequestBuilder,
    const Credential& credential,
    const HostAndPort& host,
    std::shared_ptr<SaslClientSession>* saslClientSession) {
    if (credential.mechanism == AuthMechanism::kMongoX509) {
        // MONGODB-X509
        helloRequestBuilder->append(
            kSpeculativeAuthenticate,
            BSON(kAuthenticateCommand
                 << "1" << saslCommandMechanismFieldName << toString(credential.mechanism)
                 << saslCommandUserDBFieldName << credential.db.value_or("$external")));
        return SpeculativeAuthType::kAuthenticate;
    }

    // Proceed as if this is a SASL mech and we either have a password,
    // or we don't need one (e.g. MONGODB-AWS).
    // Failure is absolutely an option.
    auto swSaslClientSession = _speculateSaslStart(helloRequestBuilder, credential, host);
    if (!swSaslClientSession.isOK()) {
        return swSaslClientSession.getStatus();
    }

    // It's okay to fail, the non-speculative auth flow will try again.
    *saslClientSession = std::move(swSaslClientSession.getValue());
    return SpeculativeAuthType::kSaslStart;
}

}  // namespace

SpeculativeAuthType speculateAuth(BSONObjBuilder* helloRequestBuilder,
                                  const MongoURI& uri,
                                  std::shared_ptr<SaslClientSession>* saslClientSession) {
    auto mechStr = uri.getOption("authMechanism").get_value_or(std::string{kMechanismScramSha256});

    auto optParams = uri.makeAuthObjFromOptions(LATEST_WIRE_VERSION, {mechStr});
    if (!optParams) {
        return SpeculativeAuthType::kNone;
    }

    auto swCred = Credential::fromBSON(optParams.value());
    if (!swCred.isOK()) {
        return SpeculativeAuthType::kNone;
    }

    auto ret = _speculateAuth(
        helloRequestBuilder, swCred.getValue(), uri.getServers().front(), saslClientSession);
    if (!ret.isOK()) {
        // Ignore error, fallback on explicit auth.
        return SpeculativeAuthType::kNone;
    }

    return ret.getValue();
}

SpeculativeAuthType speculateInternalAuth(
    const HostAndPort& remoteHost,
    BSONObjBuilder* helloRequestBuilder,
    std::shared_ptr<SaslClientSession>* saslClientSession) try {
    auto cred = getInternalAuthParams(0, std::string{kMechanismScramSha256});
    if (!cred) {
        return SpeculativeAuthType::kNone;
    }

    auto ret = _speculateAuth(helloRequestBuilder, *cred, remoteHost, saslClientSession);
    if (!ret.isOK()) {
        return SpeculativeAuthType::kNone;
    }

    return ret.getValue();
} catch (...) {
    // Swallow any exception and fallback on explicit auth.
    return SpeculativeAuthType::kNone;
}


}  // namespace auth
}  // namespace mongo
