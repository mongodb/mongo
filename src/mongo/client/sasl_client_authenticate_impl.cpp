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
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/authenticate.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/client/sasl_client_session.h"
#include "mongo/db/auth/sasl_command_constants.h"
#include "mongo/db/database_name.h"
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

#include <functional>
#include <memory>
#include <ostream>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {
namespace {

constexpr auto saslClientLogFieldName = "clientLogLevel"_sd;

int getSaslClientLogLevel(const BSONObj& saslParameters) {
    int saslLogLevel = kSaslClientLogLevelDefault;
    BSONElement saslLogElement = saslParameters[saslClientLogFieldName];

    if (saslLogElement.trueValue()) {
        saslLogLevel = 1;
    }

    if (saslLogElement.isNumber()) {
        saslLogLevel = saslLogElement.numberInt();
    }

    return saslLogLevel;
}

/**
 * Gets the password data from "saslParameters" and stores it to "outPassword".
 *
 * If "digestPassword" indicates that the password needs to be "digested" via
 * mongo::createPasswordDigest(), this method takes care of that.
 * On success, the value of "*outPassword" is always the correct value to set
 * as the password on the SaslClientSession.
 *
 * Returns Status::OK() on success, and ErrorCodes::NoSuchKey if the password data is not
 * present in "saslParameters".  Other ErrorCodes returned indicate other errors.
 */
Status extractPassword(const BSONObj& saslParameters,
                       bool digestPassword,
                       std::string* outPassword) {
    std::string rawPassword;
    Status status =
        bsonExtractStringField(saslParameters, saslCommandPasswordFieldName, &rawPassword);
    if (!status.isOK())
        return status;

    if (digestPassword) {
        std::string user;
        status = bsonExtractStringField(saslParameters, saslCommandUserFieldName, &user);
        if (!status.isOK())
            return status;

        *outPassword = mongo::createPasswordDigest(user, rawPassword);
    } else {
        *outPassword = rawPassword;
    }
    return Status::OK();
}
}  // namespace

Status saslConfigureSession(SaslClientSession* session,
                            const HostAndPort& hostname,
                            StringData targetDatabase,
                            const BSONObj& saslParameters) {
    // SERVER-59876 Ensure hostname is never empty. If it is empty, the client-side SCRAM cache will
    // not be used which creates performance problems.
    dassert(!hostname.empty());

    std::string mechanism;
    Status status =
        bsonExtractStringField(saslParameters, saslCommandMechanismFieldName, &mechanism);
    if (!status.isOK())
        return status;
    session->setParameter(SaslClientSession::parameterMechanism, mechanism);

    std::string value;
    status = bsonExtractStringFieldWithDefault(
        saslParameters, saslCommandServiceNameFieldName, saslDefaultServiceName, &value);
    if (!status.isOK())
        return status;
    session->setParameter(SaslClientSession::parameterServiceName, value);

    status = bsonExtractStringFieldWithDefault(
        saslParameters, saslCommandServiceHostnameFieldName, hostname.host(), &value);
    if (!status.isOK())
        return status;
    session->setParameter(SaslClientSession::parameterServiceHostname, value);
    session->setParameter(SaslClientSession::parameterServiceHostAndPort, hostname.toString());

    status = bsonExtractStringField(saslParameters, saslCommandUserFieldName, &value);
    if (status.isOK()) {
        session->setParameter(SaslClientSession::parameterUser, value);
    } else if ((targetDatabase != DatabaseName::kExternal.db(omitTenant)) ||
               ((mechanism != auth::kMechanismMongoAWS) &&
                (mechanism != auth::kMechanismMongoOIDC))) {
        return status;
    }

    const bool digestPasswordDefault = (mechanism == auth::kMechanismScramSha1);
    bool digestPassword;
    status = bsonExtractBooleanFieldWithDefault(
        saslParameters, saslCommandDigestPasswordFieldName, digestPasswordDefault, &digestPassword);
    if (!status.isOK())
        return status;

    status = extractPassword(saslParameters, digestPassword, &value);
    if (status.isOK()) {
        session->setParameter(SaslClientSession::parameterPassword, value);
    } else if (!(status == ErrorCodes::NoSuchKey &&
                 targetDatabase == DatabaseName::kExternal.db(omitTenant))) {
        // $external users do not have passwords, hence NoSuchKey is expected
        return status;
    }

    status = bsonExtractStringField(saslParameters, saslCommandIamSessionToken, &value);
    if (status.isOK()) {
        session->setParameter(SaslClientSession::parameterAWSSessionToken, value);
    }

    status = bsonExtractStringField(saslParameters, saslCommandOIDCAccessToken, &value);
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
    status = session->step(payload, &responsePayload);
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
    return runCommand(
               OpMsgRequestBuilder::create(
                   auth::ValidatedTenancyScope::kNotRequired /* TODO SERVER-86582 investigate */,
                   dbName,
                   commandBuilder.obj()))
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
                                        const BSONObj& saslParameters) {
    int saslLogLevel = getSaslClientLogLevel(saslParameters);
    std::string targetDatabase;
    try {
        Status status = bsonExtractStringFieldWithDefault(
            saslParameters, saslCommandUserDBFieldName, saslDefaultDBName, &targetDatabase);
        if (!status.isOK())
            return status;
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    std::string mechanism;
    Status status =
        bsonExtractStringField(saslParameters, saslCommandMechanismFieldName, &mechanism);
    if (!status.isOK()) {
        return status;
    }

    // NOTE: this must be a shared_ptr so that we can capture it in a lambda later on.
    // Come C++14, we should be able to do this in a nicer way.
    std::shared_ptr<SaslClientSession> session(SaslClientSession::create(mechanism));

    status = saslConfigureSession(session.get(), hostname, targetDatabase, saslParameters);
    if (!status.isOK())
        return status;

    auto mechanismName = session->getParameter(SaslClientSession::parameterMechanism);
    BSONObj saslFirstCommandPrefix =
        BSON(saslStartCommandName << 1 << saslCommandMechanismFieldName << mechanismName
                                  << "options" << BSON(saslCommandOptionSkipEmptyExchange << true));

    BSONObj inputObj = BSON(saslCommandPayloadFieldName << "");
    return asyncSaslConversation(
        runCommand, session, saslFirstCommandPrefix, inputObj, targetDatabase, saslLogLevel);
}

MONGO_INITIALIZER(SaslClientAuthenticateFunction)(InitializerContext* context) {
    saslClientAuthenticate = saslClientAuthenticateImpl;
}

}  // namespace
}  // namespace mongo
