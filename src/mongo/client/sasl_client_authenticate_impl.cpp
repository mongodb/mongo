/*    Copyright 2012 10gen Inc.
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

/**
 * This module implements the client side of SASL authentication in MongoDB, in terms of the Cyrus
 * SASL library.  See <sasl/sasl.h> and http://cyrusimap.web.cmu.edu/ for relevant documentation.
 *
 * The primary entry point at runtime is saslClientAuthenticateImpl().
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include <cstdint>
#include <string>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/client/sasl_client_session.h"
#include "mongo/util/base64.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/password_digest.h"

namespace mongo {

using std::endl;

namespace {

// Default log level on the client for SASL log messages.
const int defaultSaslClientLogLevel = 4;

const char* const saslClientLogFieldName = "clientLogLevel";

int getSaslClientLogLevel(const BSONObj& saslParameters) {
    int saslLogLevel = defaultSaslClientLogLevel;
    BSONElement saslLogElement = saslParameters[saslClientLogFieldName];
    if (saslLogElement.trueValue())
        saslLogLevel = 1;
    if (saslLogElement.isNumber())
        saslLogLevel = saslLogElement.numberInt();
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

/**
 * Configures "session" to perform the client side of a SASL conversation over connection
 * "client".
 *
 * "saslParameters" is a BSON document providing the necessary configuration information.
 *
 * Returns Status::OK() on success.
 */
Status configureSession(SaslClientSession* session,
                        DBClientWithCommands* client,
                        const std::string& targetDatabase,
                        const BSONObj& saslParameters) {
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

    status = bsonExtractStringFieldWithDefault(saslParameters,
                                               saslCommandServiceHostnameFieldName,
                                               HostAndPort(client->getServerAddress()).host(),
                                               &value);
    if (!status.isOK())
        return status;
    session->setParameter(SaslClientSession::parameterServiceHostname, value);

    status = bsonExtractStringField(saslParameters, saslCommandUserFieldName, &value);
    if (!status.isOK())
        return status;
    session->setParameter(SaslClientSession::parameterUser, value);

    bool digestPasswordDefault = !(targetDatabase == "$external" && mechanism == "PLAIN") &&
        !(targetDatabase == "$external" && mechanism == "GSSAPI");
    bool digestPassword;
    status = bsonExtractBooleanFieldWithDefault(
        saslParameters, saslCommandDigestPasswordFieldName, digestPasswordDefault, &digestPassword);
    if (!status.isOK())
        return status;

    status = extractPassword(saslParameters, digestPassword, &value);
    if (status.isOK()) {
        session->setParameter(SaslClientSession::parameterPassword, value);
    } else if (!(status == ErrorCodes::NoSuchKey && targetDatabase == "$external")) {
        // $external users do not have passwords, hence NoSuchKey is expected
        return status;
    }

    return session->initialize();
}

/**
 * Driver for the client side of a sasl authentication session, conducted synchronously over
 * "client".
 */
Status saslClientAuthenticateImpl(DBClientWithCommands* client, const BSONObj& saslParameters) {
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

    std::unique_ptr<SaslClientSession> session(SaslClientSession::create(mechanism));
    status = configureSession(session.get(), client, targetDatabase, saslParameters);

    if (!status.isOK())
        return status;

    BSONObj saslFirstCommandPrefix =
        BSON(saslStartCommandName << 1 << saslCommandMechanismFieldName
                                  << session->getParameter(SaslClientSession::parameterMechanism));

    BSONObj saslFollowupCommandPrefix = BSON(saslContinueCommandName << 1);
    BSONObj saslCommandPrefix = saslFirstCommandPrefix;
    BSONObj inputObj = BSON(saslCommandPayloadFieldName << "");
    bool isServerDone = false;
    while (!session->isDone()) {
        std::string payload;
        BSONType type;

        status = saslExtractPayload(inputObj, &payload, &type);
        if (!status.isOK())
            return status;

        LOG(saslLogLevel) << "sasl client input: " << base64::encode(payload) << endl;

        std::string responsePayload;
        status = session->step(payload, &responsePayload);
        if (!status.isOK())
            return status;

        LOG(saslLogLevel) << "sasl client output: " << base64::encode(responsePayload) << endl;

        BSONObjBuilder commandBuilder;
        commandBuilder.appendElements(saslCommandPrefix);
        commandBuilder.appendBinData(saslCommandPayloadFieldName,
                                     int(responsePayload.size()),
                                     BinDataGeneral,
                                     responsePayload.c_str());
        BSONElement conversationId = inputObj[saslCommandConversationIdFieldName];
        if (!conversationId.eoo())
            commandBuilder.append(conversationId);

        // Server versions 2.3.2 and earlier may return "ok: 1" with a non-zero "code" field,
        // indicating a failure.  Subsequent versions should return "ok: 0" on failure with a
        // non-zero "code" field to indicate specific failure.  In all versions, ok: 1, code: >0
        // and ok: 0, code optional, indicate failure.
        bool ok = client->runCommand(targetDatabase, commandBuilder.obj(), inputObj);
        ErrorCodes::Error code =
            ErrorCodes::fromInt(inputObj[saslCommandCodeFieldName].numberInt());

        if (!ok || code != ErrorCodes::OK) {
            if (code == ErrorCodes::OK)
                code = ErrorCodes::UnknownError;

            return Status(code, inputObj[saslCommandErrmsgFieldName].str());
        }

        isServerDone = inputObj[saslCommandDoneFieldName].trueValue();
        saslCommandPrefix = saslFollowupCommandPrefix;
    }

    if (!isServerDone)
        return Status(ErrorCodes::ProtocolError, "Client finished before server.");
    return Status::OK();
}

MONGO_INITIALIZER(SaslClientAuthenticateFunction)(InitializerContext* context) {
    saslClientAuthenticate = saslClientAuthenticateImpl;
    return Status::OK();
}

}  // namespace
}  // namespace mongo
