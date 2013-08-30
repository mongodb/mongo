/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/**
 * This module implements the client side of SASL authentication in MongoDB, in terms of the Cyrus
 * SASL library.  See <sasl/sasl.h> and http://cyrusimap.web.cmu.edu/ for relevant documentation.
 *
 * The primary entry point at runtime is saslClientAuthenticateImpl().
 */

#include <boost/scoped_ptr.hpp>
#include <string>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/auth_helpers.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/client/sasl_client_session.h"
#include "mongo/platform/cstdint.h"
#include "mongo/util/base64.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
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
     * auth::createPasswordDigest(), this method takes care of that.
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
        Status status = bsonExtractStringField(saslParameters,
                                               saslCommandPasswordFieldName,
                                               &rawPassword);
        if (!status.isOK())
            return status;

        if (digestPassword) {
            std::string user;
            status = bsonExtractStringField(saslParameters,
                                            saslCommandUserFieldName,
                                            &user);
            if (!status.isOK())
                return status;

            *outPassword = auth::createPasswordDigest(user, rawPassword);
        }
        else {
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
        Status status = bsonExtractStringField(saslParameters,
                                               saslCommandMechanismFieldName,
                                               &mechanism);
        if (!status.isOK())
            return status;
        session->setParameter(SaslClientSession::parameterMechanism, mechanism);

        std::string value;
        status = bsonExtractStringFieldWithDefault(saslParameters,
                                                   saslCommandServiceNameFieldName,
                                                   saslDefaultServiceName,
                                                   &value);
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

        status = bsonExtractStringField(saslParameters,
                                        saslCommandUserFieldName,
                                        &value);
        if (!status.isOK())
            return status;
        session->setParameter(SaslClientSession::parameterUser, value);

        bool digestPasswordDefault =
            !(targetDatabase == "$external" && mechanism == "PLAIN") &&
            !(targetDatabase == "$external" && mechanism == "GSSAPI");
        bool digestPassword;
        status = bsonExtractBooleanFieldWithDefault(saslParameters,
                                                    saslCommandDigestPasswordFieldName,
                                                    digestPasswordDefault,
                                                    &digestPassword);
        if (!status.isOK())
            return status;

        status = extractPassword(saslParameters, digestPassword, &value);
        if (status.isOK()) {
            session->setParameter(SaslClientSession::parameterPassword, value);
        }
        else if (status != ErrorCodes::NoSuchKey) {
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
            Status status = bsonExtractStringFieldWithDefault(saslParameters,
                                                              saslCommandUserSourceFieldName,
                                                              saslDefaultDBName,
                                                              &targetDatabase);
            if (!status.isOK())
                return status;
        } catch (const DBException& ex) {
            return ex.toStatus();
        }

        SaslClientSession session;
        Status status = configureSession(&session, client, targetDatabase, saslParameters);
        if (!status.isOK())
            return status;

        BSONObj saslFirstCommandPrefix = BSON(
                saslStartCommandName << 1 <<
                saslCommandMechanismFieldName <<
                session.getParameter(SaslClientSession::parameterMechanism));

        BSONObj saslFollowupCommandPrefix = BSON(saslContinueCommandName << 1);
        BSONObj saslCommandPrefix = saslFirstCommandPrefix;
        BSONObj inputObj = BSON(saslCommandPayloadFieldName << "");
        bool isServerDone = false;
        while (!session.isDone()) {
            std::string payload;
            BSONType type;

            status = saslExtractPayload(inputObj, &payload, &type);
            if (!status.isOK())
                return status;

            LOG(saslLogLevel) << "sasl client input: " << base64::encode(payload) << endl;

            std::string responsePayload;
            status = session.step(payload, &responsePayload);
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
            ErrorCodes::Error code = ErrorCodes::fromInt(
                    inputObj[saslCommandCodeFieldName].numberInt());

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
