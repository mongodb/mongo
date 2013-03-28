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
#include <sasl/sasl.h>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/client/sasl_client_session.h"
#include "mongo/platform/cstdint.h"
#include "mongo/util/allocator.h"
#include "mongo/util/base64.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
namespace {

    // Default log level on the client for SASL log messages.
    const int defaultSaslClientLogLevel = 4;

    const char* const saslClientLogFieldName = "clientLogLevel";

    /*
     * Allocator functions to be used by the SASL library, if the client
     * doesn't initialize the library for us.
     */

    void* saslOurMalloc(unsigned long sz) {
        return ourmalloc(sz);
    }

    void* saslOurCalloc(unsigned long count, unsigned long size) {
        void* ptr = calloc(count, size);
        if (!ptr) printStackAndExit(0);
        return ptr;
    }

    void* saslOurRealloc(void* ptr, unsigned long sz) {
        return ourrealloc(ptr, sz);
    }

    /*
     * Mutex functions to be used by the SASL library, if the client doesn't initialize the library
     * for us.
     */

    void* saslMutexAlloc(void) {
        return new SimpleMutex("sasl");
    }

    int saslMutexLock(void* mutex) {
        static_cast<SimpleMutex*>(mutex)->lock();
        return SASL_OK;
    }

    int saslMutexUnlock(void* mutex) {
        static_cast<SimpleMutex*>(mutex)->unlock();
        return SASL_OK;
    }

    void saslMutexFree(void* mutex) {
        delete static_cast<SimpleMutex*>(mutex);
    }

    /**
     * Configures the SASL library to use allocator and mutex functions we specify,
     * unless the client application has previously initialized the SASL library.
     */
    MONGO_INITIALIZER(CyrusSaslAllocatorsAndMutexes)(InitializerContext*) {
        sasl_set_alloc(saslOurMalloc,
                       saslOurCalloc,
                       saslOurRealloc,
                       free);

        sasl_set_mutex(saslMutexAlloc,
                       saslMutexLock,
                       saslMutexUnlock,
                       saslMutexFree);
        return Status::OK();
    }

    /**
     * Initializes the client half of the SASL library, but is effectively a no-op if the client
     * application has already done it.
     *
     * If a client wishes to override this initialization but keep the allocator and mutex
     * initialization, it should implement a MONGO_INITIALIZER_GENERAL with
     * CyrusSaslAllocatorsAndMutexes as a prerequisite and SaslClientContext as a dependent.  If it
     * wishes to override both, it should implement a MONGO_INITIALIZER_GENERAL with
     * CyrusSaslAllocatorsAndMutexes and SaslClientContext as dependents, or initialize the library
     * before calling mongo::runGlobalInitializersOrDie().
     */
    MONGO_INITIALIZER_WITH_PREREQUISITES(SaslClientContext, ("CyrusSaslAllocatorsAndMutexes"))(
            InitializerContext* context) {

        static sasl_callback_t saslClientGlobalCallbacks[] = { { SASL_CB_LIST_END } };

        // If the client application has previously called sasl_client_init(), the callbacks passed
        // in here are ignored.
        //
        // TODO: Call sasl_client_done() at shutdown when we have a story for orderly shutdown.
        int result = sasl_client_init(saslClientGlobalCallbacks);
        if (result != SASL_OK) {
            return Status(ErrorCodes::UnknownError,
                          mongoutils::str::stream() <<
                          "Could not initialize sasl client components (" <<
                          sasl_errstring(result, NULL, NULL) <<
                          ")");
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

    /**
     * Gets the password data from "saslParameters" and stores it to "outPassword".
     *
     * If "saslParameters" indicates that the password needs to be "digested" via
     * DBClientWithCommands::createPasswordDigest(), this method takes care of that.
     * On success, the value of "*outPassword" is always the correct value to set
     * as the password on the SaslClientSession.
     *
     * Returns Status::OK() on success, and ErrorCodes::NoSuchKey if the password data is not
     * present in "saslParameters".  Other ErrorCodes returned indicate other errors.
     */
    Status extractPassword(DBClientWithCommands* client,
                           const BSONObj& saslParameters,
                           std::string* outPassword) {

        std::string rawPassword;
        Status status = bsonExtractStringField(saslParameters,
                                               saslCommandPasswordFieldName,
                                               &rawPassword);
        if (!status.isOK())
            return status;

        bool digest;
        status = bsonExtractBooleanFieldWithDefault(saslParameters,
                                                    saslCommandDigestPasswordFieldName,
                                                    true,
                                                    &digest);
        if (!status.isOK())
            return status;

        if (digest) {
            std::string user;
            status = bsonExtractStringField(saslParameters,
                                            saslCommandPrincipalFieldName,
                                            &user);
            if (!status.isOK())
                return status;

            *outPassword = client->createPasswordDigest(user, rawPassword);
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
                            const BSONObj& saslParameters) {

        std::string value;
        Status status = bsonExtractStringField(saslParameters,
                                               saslCommandMechanismFieldName,
                                               &value);
        if (!status.isOK())
            return status;
        session->setParameter(SaslClientSession::parameterMechanism, value);

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
                                        saslCommandPrincipalFieldName,
                                        &value);
        if (!status.isOK())
            return status;
        session->setParameter(SaslClientSession::parameterUser, value);

        status = extractPassword(client, saslParameters, &value);
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

        SaslClientSession session;
        Status status = configureSession(&session, client, saslParameters);
        if (!status.isOK())
            return status;

        std::string targetDatabase;
        try {
            status = bsonExtractStringFieldWithDefault(saslParameters,
                                                       saslCommandPrincipalSourceFieldName,
                                                       saslDefaultDBName,
                                                       &targetDatabase);
        } catch (const DBException& ex) {
            return ex.toStatus();
        }
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
