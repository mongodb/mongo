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

#include "mongo/client/sasl_client_authenticate.h"

#include <string>

#include "mongo/base/string_data.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/util/base64.h"
#include "mongo/util/gsasl_session.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

    using namespace mongoutils;

    const char* const saslStartCommandName = "saslStart";
    const char* const saslContinueCommandName = "saslContinue";
    const char* const saslCommandAutoAuthorizeFieldName = "autoAuthorize";
    const char* const saslCommandCodeFieldName = "code";
    const char* const saslCommandConversationIdFieldName = "conversationId";
    const char* const saslCommandDoneFieldName = "done";
    const char* const saslCommandErrmsgFieldName = "errmsg";
    const char* const saslCommandMechanismFieldName = "mechanism";
    const char* const saslCommandMechanismListFieldName = "supportedMechanisms";
    const char* const saslCommandPasswordFieldName = "pwd";
    const char* const saslCommandPayloadFieldName = "payload";
    const char* const saslCommandPrincipalFieldName = "user";
    const char* const saslCommandPrincipalSourceFieldName = "userSource";
    const char* const saslCommandServiceHostnameFieldName = "serviceHostname";
    const char* const saslCommandServiceNameFieldName = "serviceName";
    const char* const saslDefaultDBName = "$sasl";
    const char* const saslDefaultServiceName = "mongodb";

    const char* const saslClientLogFieldName = "clientLogLevel";

namespace {
    // Default log level on the client for SASL log messages.
    const int defaultSaslClientLogLevel = 4;
}  // namespace

    Status saslExtractPayload(const BSONObj& cmdObj, std::string* payload, BSONType* type) {
        BSONElement payloadElement;
        Status status = bsonExtractField(cmdObj, saslCommandPayloadFieldName, &payloadElement);
        if (!status.isOK())
            return status;

        *type = payloadElement.type();
        if (payloadElement.type() == BinData) {
            const char* payloadData;
            int payloadLen;
            payloadData = payloadElement.binData(payloadLen);
            if (payloadLen < 0)
                return Status(ErrorCodes::InvalidLength, "Negative payload length");
            *payload = std::string(payloadData, payloadData + payloadLen);
        }
        else if (payloadElement.type() == String) {
            try {
                *payload = base64::decode(payloadElement.str());
            } catch (UserException& e) {
                return Status(ErrorCodes::FailedToParse, e.what());
            }
        }
        else {
            return Status(ErrorCodes::TypeMismatch,
                          (str::stream() << "Wrong type for field; expected BinData or String for "
                           << payloadElement));
        }

        return Status::OK();
    }

namespace {

    /**
     * Configure "*session" as a client gsasl session for authenticating on the connection
     * "*client", with the given "saslParameters".  "gsasl" and "sessionHook" are passed through
     * to GsaslSession::initializeClientSession, where they are documented.
     */
    Status configureSession(Gsasl* gsasl,
                            DBClientWithCommands* client,
                            const BSONObj& saslParameters,
                            void* sessionHook,
                            GsaslSession* session) {

        std::string mechanism;
        Status status = bsonExtractStringField(saslParameters,
                                               saslCommandMechanismFieldName,
                                               &mechanism);
        if (!status.isOK())
            return status;

        status = session->initializeClientSession(gsasl, mechanism, sessionHook);
        if (!status.isOK())
            return status;

        std::string service;
        status = bsonExtractStringFieldWithDefault(saslParameters,
                                                   saslCommandServiceNameFieldName,
                                                   saslDefaultServiceName,
                                                   &service);
        if (!status.isOK())
            return status;
        session->setProperty(GSASL_SERVICE, service);

        std::string hostname;
        status = bsonExtractStringFieldWithDefault(saslParameters,
                                                   saslCommandServiceHostnameFieldName,
                                                   HostAndPort(client->getServerAddress()).host(),
                                                   &hostname);
        if (!status.isOK())
            return status;
        session->setProperty(GSASL_HOSTNAME, hostname);

        BSONElement principalElement = saslParameters[saslCommandPrincipalFieldName];
        if (principalElement.type() == String) {
            session->setProperty(GSASL_AUTHID, principalElement.str());
        }
        else if (!principalElement.eoo()) {
            return Status(ErrorCodes::TypeMismatch,
                          str::stream() << "Expected string for " << principalElement);
        }

        BSONElement passwordElement = saslParameters[saslCommandPasswordFieldName];
        if (passwordElement.type() == String) {
            std::string passwordHash = client->createPasswordDigest(principalElement.str(),
                                                                    passwordElement.str());
            session->setProperty(GSASL_PASSWORD, passwordHash);
        }
        else if (!passwordElement.eoo()) {
            return Status(ErrorCodes::TypeMismatch,
                          str::stream() << "Expected string for " << passwordElement);
        }

        return Status::OK();
    }

    int getSaslClientLogLevel(const BSONObj& saslParameters) {
        int saslLogLevel = defaultSaslClientLogLevel;
        BSONElement saslLogElement = saslParameters[saslClientLogFieldName];
        if (saslLogElement.trueValue())
            saslLogLevel = 1;
        if (saslLogElement.isNumber())
            saslLogLevel = saslLogElement.numberInt();
        return saslLogLevel;
    }

}  // namespace

    Status saslClientAuthenticate(Gsasl *gsasl,
                                  DBClientWithCommands* client,
                                  const BSONObj& saslParameters,
                                  void* sessionHook) {

        GsaslSession session;

        int saslLogLevel = getSaslClientLogLevel(saslParameters);

        Status status = configureSession(gsasl, client, saslParameters, sessionHook, &session);
        if (!status.isOK())
            return status;

        std::string targetDatabase;
        status = bsonExtractStringFieldWithDefault(saslParameters,
                                                   saslCommandPrincipalSourceFieldName,
                                                   saslDefaultDBName,
                                                   &targetDatabase);
        if (!status.isOK())
            return status;

        BSONObj saslFirstCommandPrefix = BSON(
                saslStartCommandName << 1 <<
                saslCommandMechanismFieldName << session.getMechanism());

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

            if (!client->runCommand(targetDatabase, commandBuilder.obj(), inputObj)) {
                return Status(ErrorCodes::UnknownError,
                              inputObj[saslCommandErrmsgFieldName].str());
            }

            int statusCodeInt = inputObj[saslCommandCodeFieldName].Int();
            if (0 != statusCodeInt)
                return Status(ErrorCodes::fromInt(statusCodeInt),
                              inputObj[saslCommandErrmsgFieldName].str());

            isServerDone = inputObj[saslCommandDoneFieldName].trueValue();
            saslCommandPrefix = saslFollowupCommandPrefix;
        }

        if (!isServerDone)
            return Status(ErrorCodes::ProtocolError, "Client finished before server.");
        return Status::OK();
    }

}  // namespace mongo
