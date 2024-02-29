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


#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/client/authenticate.h"
#include "mongo/client/internal_auth.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/sasl_command_constants.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/server_options.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_component.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl


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
        return db.getStatus();

    std::string username;
    auto response = bsonExtractStringFieldWithDefault(
        params, saslCommandUserFieldName, clientName.toString(), &username);
    if (!response.isOK()) {
        return response;
    }
    if (username != clientName.toString()) {
        StringBuilder message;
        message << "Username \"";
        message << params[saslCommandUserFieldName].valueStringData();
        message << "\" does not match the provided client certificate user \"";
        message << clientName.toString() << "\"";
        return {ErrorCodes::AuthenticationFailed, message.str()};
    }

    return OpMsgRequestBuilder::createWithValidatedTenancyScope(
        AuthDatabaseNameUtil::deserialize(db.getValue()),
        auth::ValidatedTenancyScope::kNotRequired /* db is not tenanted */,
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

class DefaultInternalAuthParametersProvider : public InternalAuthParametersProvider {
public:
    ~DefaultInternalAuthParametersProvider() = default;

    BSONObj get(size_t index, StringData mechanism) final {
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

Future<void> authenticateClient(const BSONObj& params,
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

    return runCommand(OpMsgRequestBuilder::createWithValidatedTenancyScope(
                          DatabaseName::kAdmin,
                          auth::ValidatedTenancyScope::kNotRequired /* admin is not per-tenant. */,
                          request))
        .then([](BSONObj reply) -> Future<std::string> {
            auto mechsArrayObj = reply.getField("saslSupportedMechs");
            if (mechsArrayObj.type() != Array) {
                return Status{ErrorCodes::BadValue, "Expected array of SASL mechanism names"};
            }

            auto obj = mechsArrayObj.Obj();
            std::vector<std::string> availableMechanisms;
            for (const auto& elem : obj) {
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
            auto params = internalParamsProvider->get(0, mechanism);
            if (params.isEmpty()) {
                return Status(ErrorCodes::BadValue,
                              "Missing authentication parameters for internal user auth");
            }
            return authenticateClient(params, remote, clientSubjectName, runCommand)
                .onError<ErrorCodes::AuthenticationFailed>(
                    [runCommand, clientSubjectName, remote, mechanism, internalParamsProvider](
                        Status status) -> Future<void> {
                        auto altCreds = internalParamsProvider->get(1, mechanism);
                        if (!altCreds.isEmpty()) {
                            return authenticateClient(
                                altCreds, remote, clientSubjectName, runCommand);
                        }
                        return status;
                    });
        });
}

BSONObj buildAuthParams(const DatabaseName& dbname,
                        StringData username,
                        StringData passwordText,
                        StringData mechanism) {
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

StringData getSaslCommandUserDBFieldName() {
    return saslCommandUserDBFieldName;
}

StringData getSaslCommandUserFieldName() {
    return saslCommandUserFieldName;
}

namespace {

StatusWith<std::shared_ptr<SaslClientSession>> _speculateSaslStart(
    BSONObjBuilder* helloRequestBuilder,
    const std::string& mechanism,
    const HostAndPort& host,
    StringData authDB,
    BSONObj params) {
    if (mechanism == kMechanismSaslPlain) {
        return {ErrorCodes::BadValue, "PLAIN mechanism not supported with speculativeSaslStart"};
    }

    std::shared_ptr<SaslClientSession> session(SaslClientSession::create(mechanism));
    auto status = saslConfigureSession(session.get(), host, authDB, params);
    if (!status.isOK()) {
        return status;
    }

    std::string payload;
    status = session->step("", &payload);
    if (!status.isOK()) {
        return status;
    }

    BSONObjBuilder saslStart;
    saslStart.append("saslStart", 1);
    saslStart.append("mechanism", mechanism);
    saslStart.appendBinData("payload", int(payload.size()), BinDataGeneral, payload.c_str());
    saslStart.append("db", authDB);
    helloRequestBuilder->append(kSpeculativeAuthenticate, saslStart.obj());

    return session;
}

StatusWith<SpeculativeAuthType> _speculateAuth(
    BSONObjBuilder* helloRequestBuilder,
    const std::string& mechanism,
    const HostAndPort& host,
    StringData authDB,
    BSONObj params,
    std::shared_ptr<SaslClientSession>* saslClientSession) {
    if (mechanism == kMechanismMongoX509) {
        // MONGODB-X509
        helloRequestBuilder->append(kSpeculativeAuthenticate,
                                    BSON(kAuthenticateCommand
                                         << "1" << saslCommandMechanismFieldName << mechanism
                                         << saslCommandUserDBFieldName << "$external"));
        return SpeculativeAuthType::kAuthenticate;
    }

    // Proceed as if this is a SASL mech and we either have a password,
    // or we don't need one (e.g. MONGODB-AWS).
    // Failure is absolutely an option.
    auto swSaslClientSession =
        _speculateSaslStart(helloRequestBuilder, mechanism, host, authDB, params);
    if (!swSaslClientSession.isOK()) {
        return swSaslClientSession.getStatus();
    }

    // It's okay to fail, the non-speculative auth flow will try again.
    *saslClientSession = std::move(swSaslClientSession.getValue());
    return SpeculativeAuthType::kSaslStart;
}

std::string getBSONString(BSONObj container, StringData field) {
    auto elem = container[field];
    uassert(ErrorCodes::BadValue,
            str::stream() << "Field '" << field << "' must be of type string",
            elem.type() == String);
    return elem.String();
}
}  // namespace

SpeculativeAuthType speculateAuth(BSONObjBuilder* helloRequestBuilder,
                                  const MongoURI& uri,
                                  std::shared_ptr<SaslClientSession>* saslClientSession) {
    auto mechanism = uri.getOption("authMechanism").get_value_or(kMechanismScramSha256.toString());

    auto optParams = uri.makeAuthObjFromOptions(LATEST_WIRE_VERSION, {mechanism});
    if (!optParams) {
        return SpeculativeAuthType::kNone;
    }

    auto params = std::move(optParams.value());

    auto ret = _speculateAuth(helloRequestBuilder,
                              mechanism,
                              uri.getServers().front(),
                              uri.getAuthenticationDatabase(),
                              params,
                              saslClientSession);
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
    auto params = getInternalAuthParams(0, kMechanismScramSha256.toString());
    if (params.isEmpty()) {
        return SpeculativeAuthType::kNone;
    }

    auto mechanism = getBSONString(params, saslCommandMechanismFieldName);
    auto authDB = getBSONString(params, saslCommandUserDBFieldName);

    auto ret = _speculateAuth(
        helloRequestBuilder, mechanism, remoteHost, authDB, params, saslClientSession);
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
