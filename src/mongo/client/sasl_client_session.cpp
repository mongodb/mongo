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

#include "mongo/client/sasl_client_session.h"

#include "mongo/base/init.h"
#include "mongo/util/allocator.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

    /*
     * Allocator functions to be used by the SASL library, if the client
     * doesn't initialize the library for us.
     */

// Version 2.1.26 is the first version to use size_t in the allocator signatures
#if (SASL_VERSION_FULL >= ((2 << 16) | (1 << 8) | 26))
    typedef size_t SaslAllocSize;
#else
    typedef unsigned long SaslAllocSize;
#endif

    typedef int(*SaslCallbackFn)();

    void* saslOurMalloc(SaslAllocSize sz) {
        return ourmalloc(sz);
    }

    void* saslOurCalloc(SaslAllocSize count, SaslAllocSize size) {
        void* ptr = calloc(count, size);
        if (!ptr) printStackAndExit(0);
        return ptr;
    }

    void* saslOurRealloc(void* ptr, SaslAllocSize sz) {
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

    int saslClientLogSwallow(void *context, int priority, const char *message) {
        return SASL_OK;  // do nothing
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

        static sasl_callback_t saslClientGlobalCallbacks[] = 
            { { SASL_CB_LOG, SaslCallbackFn(saslClientLogSwallow), NULL /* context */ },
              { SASL_CB_LIST_END } };

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

    /**
     * Callback registered on the sasl_conn_t underlying a SaslClientSession to allow the Cyrus SASL
     * library to query for the authentication id and other simple string configuration parameters.
     *
     * Note that in Mongo, the authentication and authorization ids (authid and authzid) are always
     * the same.  These correspond to SASL_CB_AUTHNAME and SASL_CB_USER.
     */
    int saslClientGetSimple(void* context,
                            int id,
                            const char** result,
                            unsigned* resultLen) throw () {
        SaslClientSession* session = static_cast<SaslClientSession*>(context);
        if (!session || !result)
            return SASL_BADPARAM;

        SaslClientSession::Parameter requiredParameterId;
        switch (id) {
        case SASL_CB_AUTHNAME:
        case SASL_CB_USER:
            requiredParameterId = SaslClientSession::parameterUser;
            break;
        default:
            return SASL_FAIL;
        }

        if (!session->hasParameter(requiredParameterId))
            return SASL_FAIL;
        StringData value = session->getParameter(requiredParameterId);
        *result = value.rawData();
        if (resultLen)
            *resultLen = static_cast<unsigned>(value.size());
        return SASL_OK;
    }

    /**
     * Callback registered on the sasl_conn_t underlying a SaslClientSession to allow the Cyrus SASL
     * library to query for the password data.
     */
    int saslClientGetPassword(sasl_conn_t* conn,
                              void* context,
                              int id,
                              sasl_secret_t** outSecret) throw () {

        SaslClientSession* session = static_cast<SaslClientSession*>(context);
        if (!session || !outSecret)
            return SASL_BADPARAM;

        sasl_secret_t* secret = session->getPasswordAsSecret();
        if (secret == NULL) {
            sasl_seterror(conn, 0, "No password data provided");
            return SASL_FAIL;
        }

        *outSecret = secret;
        return SASL_OK;
    }

}  // namespace

    SaslClientSession::SaslClientSession() :
        _saslConnection(NULL),
        _step(0),
        _done(false) {

        const sasl_callback_t callbackTemplate[maxCallbacks] = {
            { SASL_CB_AUTHNAME, SaslCallbackFn(saslClientGetSimple), this },
            { SASL_CB_USER, SaslCallbackFn(saslClientGetSimple), this },
            { SASL_CB_PASS, SaslCallbackFn(saslClientGetPassword), this },
            { SASL_CB_LIST_END }
        };
        std::copy(callbackTemplate, callbackTemplate + maxCallbacks, _callbacks);
    }

    SaslClientSession::~SaslClientSession() {
        sasl_dispose(&_saslConnection);
    }

    void SaslClientSession::setParameter(Parameter id, const StringData& value) {
        fassert(16807, id >= 0 && id < numParameters);
        DataBuffer& buffer = _parameters[id];
        if (id == parameterPassword) {
            // The parameterPassword is stored as a sasl_secret_t inside its DataBuffer, while other
            // parameters are stored directly.  This facilitates memory ownership management for
            // getPasswordAsSecret().
            buffer.size = sizeof(sasl_secret_t) + value.size();
            buffer.data.reset(new char[buffer.size + 1]);
            sasl_secret_t* secret =
                static_cast<sasl_secret_t*>(static_cast<void*>(buffer.data.get()));
            secret->len = value.size();
            value.copyTo(static_cast<char*>(static_cast<void*>(&secret->data[0])), false);
        }
        else {
            buffer.size = value.size();
            buffer.data.reset(new char[buffer.size + 1]);
            // Note that we append a terminal NUL to buffer.data, so it may be treated as a C-style
            // string.  This is required for parameterServiceName, parameterServiceHostname,
            // parameterMechanism and parameterUser.
            value.copyTo(buffer.data.get(), true);
        }
    }

    bool SaslClientSession::hasParameter(Parameter id) {
        if (id < 0 || id >= numParameters)
            return false;
        return _parameters[id].data;
    }

    StringData SaslClientSession::getParameter(Parameter id) {
        if (!hasParameter(id))
            return StringData();

        if (id == parameterPassword) {
            // See comment in setParameter() about the special storage of parameterPassword.
            sasl_secret_t* secret = getPasswordAsSecret();
            return StringData(static_cast<char*>(static_cast<void*>(secret->data)), secret->len);
        }
        else {
            DataBuffer& buffer = _parameters[id];
            return StringData(buffer.data.get(), buffer.size);
        }
    }

    sasl_secret_t* SaslClientSession::getPasswordAsSecret() {
        // See comment in setParameter() about the special storage of parameterPassword.
        return static_cast<sasl_secret_t*>(
                static_cast<void*>(_parameters[parameterPassword].data.get()));
    }

    Status SaslClientSession::initialize() {
        if (_saslConnection != NULL)
            return Status(ErrorCodes::AlreadyInitialized, "Cannot reinitialize SaslClientSession.");

        int result = sasl_client_new(_parameters[parameterServiceName].data.get(),
                                     _parameters[parameterServiceHostname].data.get(),
                                     NULL,
                                     NULL,
                                     _callbacks,
                                     0,
                                     &_saslConnection);

        if (SASL_OK != result) {
            return Status(ErrorCodes::UnknownError,
                          mongoutils::str::stream() << sasl_errstring(result, NULL, NULL));
        }

        return Status::OK();
    }

    Status SaslClientSession::step(const StringData& inputData, std::string* outputData) {
        const char* output = NULL;
        unsigned outputSize = 0xFFFFFFFF;

        int result;
        if (_step == 0) {
            const char* actualMechanism;
            result = sasl_client_start(_saslConnection,
                                       getParameter(parameterMechanism).toString().c_str(),
                                       NULL,
                                       &output,
                                       &outputSize,
                                       &actualMechanism);
        }
        else {
            result = sasl_client_step(_saslConnection,
                                      inputData.rawData(),
                                      static_cast<unsigned>(inputData.size()),
                                      NULL,
                                      &output,
                                      &outputSize);
        }
        ++_step;
        switch (result) {
        case SASL_OK:
            _done = true;
            // Fall through
        case SASL_CONTINUE:
            *outputData = std::string(output, outputSize);
            return Status::OK();
        case SASL_NOMECH:
            return Status(ErrorCodes::BadValue, sasl_errdetail(_saslConnection));
        case SASL_BADAUTH:
            return Status(ErrorCodes::AuthenticationFailed, sasl_errdetail(_saslConnection));
        default:
            return Status(ErrorCodes::ProtocolError, sasl_errdetail(_saslConnection));
        }
    }

}  // namespace mongo
