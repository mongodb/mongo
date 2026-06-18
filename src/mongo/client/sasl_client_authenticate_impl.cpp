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

/**
 * This module implements the client side of SASL authentication in MongoDB, in terms of the Cyrus
 * SASL library.  See <sasl/sasl.h> and http://cyrusimap.web.cmu.edu/ for relevant documentation.
 *
 * The primary entry point at runtime is saslClientAuthenticateImpl().
 */


#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/authenticate.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/client/sasl_client_session.h"
#include "mongo/db/auth/authentication_metrics.h"
#include "mongo/db/auth/sasl_command_constants.h"
#include "mongo/db/connection_health_metrics_parameter_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/stats/counters.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/base64.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/password_digest.h"
#include "mongo/util/str.h"

#include <functional>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

constexpr auto saslClientLogFieldName = "clientLogLevel"sv;

int getSaslClientLogLevel(const auth::Credential& credential) {
    int saslLogLevel = kSaslClientLogLevelDefault;
    BSONElement saslLogElement = credential.mechanismProperties[saslClientLogFieldName];

    if (saslLogElement.trueValue()) {
        saslLogLevel = 1;
    }

    if (saslLogElement.isNumber()) {
        saslLogLevel = saslLogElement.numberInt();
    }

    return saslLogLevel;
}

}  // namespace

Status saslConfigureSession(SaslClientSession* session,
                            const HostAndPort& hostname,
                            const auth::Credential& credential) {
    // SERVER-59876 Ensure hostname is never empty. If it is empty, the client-side SCRAM cache will
    // not be used which creates performance problems.
    dassert(!hostname.empty());

    const auto mechStr = toString(credential.mechanism);
    session->setParameter(SaslClientSession::parameterMechanism, mechStr);

    const auto& props = credential.mechanismProperties;
    std::string value;
    Status status = bsonExtractStringFieldWithDefault(
        props, saslCommandServiceNameFieldName, saslDefaultServiceName, &value);
    if (!status.isOK())
        return status;
    session->setParameter(SaslClientSession::parameterServiceName, value);

    status = bsonExtractStringFieldWithDefault(
        props, saslCommandServiceHostnameFieldName, hostname.host(), &value);
    if (!status.isOK())
        return status;
    session->setParameter(SaslClientSession::parameterServiceHostname, value);
    session->setParameter(SaslClientSession::parameterServiceHostAndPort, hostname.toString());

    const auto targetDatabase = credential.db.value_or(std::string{saslDefaultDBName});
    if (credential.username) {
        session->setParameter(SaslClientSession::parameterUser, *credential.username);
    } else if ((targetDatabase != DatabaseName::kExternal.db(omitTenant)) ||
               (credential.mechanism != auth::AuthMechanism::kMongoAWS &&
                credential.mechanism != auth::AuthMechanism::kMongoOIDC)) {
        return Status(ErrorCodes::AuthenticationFailed,
                      str::stream() << "Username required for mechanism " << mechStr);
    }

    if (credential.password) {
        const bool digestPasswordDefault =
            (credential.mechanism == auth::AuthMechanism::kScramSha1);
        bool digestPassword = digestPasswordDefault;
        status = bsonExtractBooleanFieldWithDefault(
            props, saslCommandDigestPasswordFieldName, digestPasswordDefault, &digestPassword);
        if (!status.isOK())
            return status;

        std::string processedPassword = *credential.password;
        if (digestPassword && credential.username) {
            processedPassword =
                mongo::createPasswordDigest(*credential.username, *credential.password);
        }
        session->setParameter(SaslClientSession::parameterPassword, processedPassword);
    } else if (targetDatabase != DatabaseName::kExternal.db(omitTenant)) {
        return Status(ErrorCodes::AuthenticationFailed, "Password required");
    }

    status = bsonExtractStringField(props, saslCommandIamSessionToken, &value);
    if (status.isOK()) {
        session->setParameter(SaslClientSession::parameterAWSSessionToken, value);
    }

    status = bsonExtractStringField(props, saslCommandOIDCAccessToken, &value);
    if (status.isOK()) {
        session->setParameter(SaslClientSession::parameterOIDCAccessToken, value);
    }

    return session->initialize();
}

Future<void> asyncSaslConversation(auth::RunCommandHook runCommand,
                                   const std::shared_ptr<SaslClientSession>& session,
                                   const BSONObj& saslCommandPrefix,
                                   const BSONObj& inputObj,
                                   std::string targetDatabase,
                                   int saslLogLevel) {
    // Extract payload from previous step
    std::string payload;
    BSONType type;
    auto status = saslExtractPayload(inputObj, &payload, &type);
    if (!status.isOK())
        return status;

    LOGV2_DEBUG(20197, saslLogLevel, "sasl client input", "payload"_attr = base64::encode(payload));

    // Create new payload for our response
    std::string responsePayload;
    status = [&] {
        ScopedCallbackTimer st([&](Duration<std::micro> elapsed) {
            BSONObjBuilder bob;

            auto currentStepOpt = session->currentStep();
            if (currentStepOpt) {
                bob << "step" << static_cast<std::int32_t>(*currentStepOpt);
            }

            auto totalStepOpt = session->totalSteps();
            if (totalStepOpt) {
                bob << "step_total" << static_cast<std::int32_t>(*totalStepOpt);
            }

            bob << "duration_micros" << elapsed.count();

            session->metrics()->appendMetric(bob.obj());
        });

        return session->step(payload, &responsePayload);
    }();

    if (!status.isOK())
        return status;

    LOGV2_DEBUG(20198,
                saslLogLevel,
                "sasl client output",
                "payload"_attr = base64::encode(responsePayload));

    // Handle a done from the server which comes before the client is complete.
    const bool serverDone = inputObj[saslCommandDoneFieldName].trueValue();
    if (serverDone && responsePayload.empty() && session->isSuccess()) {
        return Status::OK();
    }

    // Build command using our new payload and conversationId
    BSONObjBuilder commandBuilder;
    commandBuilder.appendElements(saslCommandPrefix);
    commandBuilder.appendBinData(saslCommandPayloadFieldName,
                                 int(responsePayload.size()),
                                 BinDataGeneral,
                                 responsePayload.c_str());
    BSONElement conversationId = inputObj[saslCommandConversationIdFieldName];
    if (!conversationId.eoo())
        commandBuilder.append(conversationId);

    // Asynchronously continue the conversation
    const auto dbName = DatabaseNameUtil::deserialize(
        boost::none, targetDatabase, SerializationContext::stateDefault());
    return runCommand(OpMsgRequestBuilder::create(
                          auth::ValidatedTenancyScope::kNotRequired, dbName, commandBuilder.obj()))
        .then([runCommand, session, targetDatabase, saslLogLevel](
                  BSONObj serverResponse) -> Future<void> {
            auto status = getStatusFromCommandResult(serverResponse);
            if (!status.isOK()) {
                return status;
            }

            // Exit if we have finished
            if (session->isSuccess()) {
                bool isServerDone = serverResponse[saslCommandDoneFieldName].trueValue();
                if (!isServerDone) {
                    return Status(ErrorCodes::ProtocolError, "Client finished before server.");
                }
                return Status::OK();
            }

            static const BSONObj saslFollowupCommandPrefix = BSON(saslContinueCommandName << 1);
            return asyncSaslConversation(runCommand,
                                         session,
                                         saslFollowupCommandPrefix,
                                         serverResponse,
                                         targetDatabase,
                                         saslLogLevel);
        });
}

namespace {
/**
 * Driver for the client side of a sasl authentication session, conducted synchronously over
 * "client".
 */
Future<void> saslClientAuthenticateImpl(auth::RunCommandHook runCommand,
                                        const HostAndPort& hostname,
                                        const auth::Credential& credential) {
    if (credential.mechanism == auth::AuthMechanism::kMongoDbCr)
        return Status{ErrorCodes::AuthenticationFailed,
                      "MONGODB-CR is deprecated and no longer supported. Use SCRAM for "
                      "password-based authentication instead."};

    int saslLogLevel = getSaslClientLogLevel(credential);
    const auto targetDatabase = credential.db.value_or(std::string{saslDefaultDBName});
    const auto mechStr = toString(credential.mechanism);

    // NOTE: this must be a shared_ptr so that we can capture it in a lambda later on.
    // Come C++14, we should be able to do this in a nicer way.
    std::shared_ptr<SaslClientSession> session(SaslClientSession::create(std::string{mechStr}));

    auto status = saslConfigureSession(session.get(), hostname, credential);
    if (!status.isOK())
        return status;

    auto mechanismName = session->getParameter(SaslClientSession::parameterMechanism);
    BSONObj saslFirstCommandPrefix =
        BSON(saslStartCommandName << 1 << saslCommandMechanismFieldName << mechanismName
                                  << "options" << BSON(saslCommandOptionSkipEmptyExchange << true));

    BSONObj inputObj = BSON(saslCommandPayloadFieldName << "");

    auto mechCounter = authCounter.getEgressMechanismCounter(mechStr);
    mechCounter.incAuthenticateSent();

    const auto username = credential.username.value_or("");
    auto argsBlock = std::make_tuple(hostname, username, targetDatabase, mechStr, mechCounter);
    auto sharedBlock = std::make_shared<decltype(argsBlock)>(std::move(argsBlock));

    session->metrics()->restart();

    return asyncSaslConversation(
               runCommand, session, saslFirstCommandPrefix, inputObj, targetDatabase, saslLogLevel)
        .onError([session, sharedBlock](Status status) {
            BSONObj metrics = session->metrics()->captureEgress();
            auto [hostname, username, targetDatabase, mechanism, _] = *sharedBlock.get();
            if (gEnableDetailedConnectionHealthMetricLogLines.load()) {
                LOGV2(10748700,
                      "Authentication to remote host failed using SASL",
                      "hostname"_attr = hostname,
                      "username"_attr = username,
                      "targetDatabase"_attr = targetDatabase,
                      "mechanism"_attr = mechanism,
                      "error"_attr = redact(status),
                      "result"_attr = status.code(),
                      "metrics"_attr = metrics);
            }
            return status;
        })
        .then([session, sharedBlock]() {
            BSONObj metrics = session->metrics()->captureEgress();
            auto [hostname, username, targetDatabase, mechanism, mechCounter] = *sharedBlock.get();
            mechCounter.incEgressAuthenticateSuccessful();
            if (gEnableDetailedConnectionHealthMetricLogLines.load()) {
                LOGV2(10748701,
                      "Authentication to remote host succeeded using SASL",
                      "hostname"_attr = hostname,
                      "username"_attr = username,
                      "targetDatabase"_attr = targetDatabase,
                      "mechanism"_attr = mechanism,
                      "result"_attr = Status::OK().code(),
                      "metrics"_attr = metrics);
            }
        });
}

MONGO_INITIALIZER(SaslClientAuthenticateFunction)(InitializerContext* context) {
    saslClientAuthenticate = saslClientAuthenticateImpl;
}

}  // namespace
}  // namespace mongo
