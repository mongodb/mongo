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

#include <string>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/platform/cstdint.h"
#include "mongo/util/base64.h"
#include "mongo/util/gsasl_session.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/hostandport.h"

#include <gsasl.h>  // Must be included after "mongo/platform/cstdint.h" because of SERVER-8086.

namespace mongo {
namespace {

    // Default log level on the client for SASL log messages.
    const int defaultSaslClientLogLevel = 4;

    const char* const saslClientLogFieldName = "clientLogLevel";

    Gsasl* _gsaslLibraryContext = NULL;

    MONGO_INITIALIZER(SaslClientContext)(InitializerContext* context) {
        fassert(16710, _gsaslLibraryContext == NULL);

        if (!gsasl_check_version(GSASL_VERSION))
            return Status(ErrorCodes::UnknownError, "Incompatible gsasl library.");

        int rc = gsasl_init(&_gsaslLibraryContext);
        if (GSASL_OK != rc)
            return Status(ErrorCodes::UnknownError, gsasl_strerror(rc));
        return Status::OK();
    }

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
            bool digest;
            status = bsonExtractBooleanFieldWithDefault(saslParameters,
                                                        saslCommandDigestPasswordFieldName,
                                                        true,
                                                        &digest);
            if (!status.isOK())
                return status;

            std::string passwordHash;
            if (digest) {
                passwordHash = client->createPasswordDigest(principalElement.str(),
                                                            passwordElement.str());
            }
            else {
                passwordHash = passwordElement.str();
            }
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

    Status saslClientAuthenticateImpl(DBClientWithCommands* client,
                                      const BSONObj& saslParameters,
                                      void* sessionHook) {

        GsaslSession session;

        int saslLogLevel = getSaslClientLogLevel(saslParameters);

        Status status = configureSession(_gsaslLibraryContext,
                                         client,
                                         saslParameters,
                                         sessionHook,
                                         &session);
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
