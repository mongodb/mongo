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


#include "mongo/client/cyrus_sasl_client_session.h"

#include "mongo/base/init.h"
#include "mongo/client/authenticate.h"
#include "mongo/client/native_sasl_client_session.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/allocator.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/signal_handlers_synchronous.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl

namespace mongo {
namespace {

void saslSetError(sasl_conn_t* conn, const std::string& msg) {
    sasl_seterror(conn, 0, "%s", msg.c_str());
}

SaslClientSession* createCyrusSaslClientSession(const std::string& mech) {
    if ((mech == auth::kMechanismScramSha1) || (mech == auth::kMechanismScramSha256) ||
        (mech == auth::kMechanismSaslPlain) || (mech == auth::kMechanismMongoAWS) ||
        (mech == auth::kMechanismMongoOIDC)) {
        return new NativeSaslClientSession();
    }
    return new CyrusSaslClientSession();
}

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

typedef int (*SaslCallbackFn)();

void* saslOurMalloc(SaslAllocSize sz) {
    return mongoMalloc(sz);
}

void* saslOurCalloc(SaslAllocSize count, SaslAllocSize size) {
    void* ptr = calloc(count, size);
    if (!ptr) {
        reportOutOfMemoryErrorAndExit();
    }
    return ptr;
}

void* saslOurRealloc(void* ptr, SaslAllocSize sz) {
    return mongoRealloc(ptr, sz);
}

/*
 * Mutex functions to be used by the SASL library, if the client doesn't initialize the library
 * for us.
 */

void* saslMutexAlloc(void) {
    return new stdx::mutex;
}

int saslMutexLock(void* mutex) {
    static_cast<stdx::mutex*>(mutex)->lock();
    return SASL_OK;
}

int saslMutexUnlock(void* mutex) {
    static_cast<stdx::mutex*>(mutex)->unlock();
    return SASL_OK;
}

void saslMutexFree(void* mutex) {
    delete static_cast<stdx::mutex*>(mutex);
}

/**
 * Configures the SASL library to use allocator and mutex functions we specify,
 * unless the client application has previously initialized the SASL library.
 */
MONGO_INITIALIZER(CyrusSaslAllocatorsAndMutexesClient)(InitializerContext*) {
    sasl_set_alloc(saslOurMalloc, saslOurCalloc, saslOurRealloc, free);

    sasl_set_mutex(saslMutexAlloc, saslMutexLock, saslMutexUnlock, saslMutexFree);
}

int saslClientLogSwallow(void* context, int priority, const char* message) {
    return SASL_OK;  // do nothing
}

/**
 * Implements the Cyrus SASL default_verifyfile_cb interface registered in the
 * Cyrus SASL library to verify, and then accept or reject, the loading of
 * plugin libraries from the target directory.
 *
 * On Windows environments, disable loading of plugin files.
 */
int saslClientVerifyPluginFile(void*, const char*, sasl_verify_type_t type) {

    if (type != SASL_VRFY_PLUGIN) {
        return SASL_OK;
    }

#ifdef _WIN32
    return SASL_CONTINUE;  // A non-SASL_OK response indicates to Cyrus SASL that it
                           // should not load a file. This effectively disables
                           // loading plugins from path on Windows.
#else
    return SASL_OK;
#endif
}

/**
 * Initializes the client half of the SASL library, but is effectively a no-op if the client
 * application has already done it.
 *
 * If a client wishes to override this initialization but keep the allocator and mutex
 * initialization, it should implement a MONGO_INITIALIZER_GENERAL with
 * CyrusSaslAllocatorsAndMutexesClient as a prerequisite and CyrusSaslClientContext as a
 * dependent.  If it wishes to override both, it should implement a MONGO_INITIALIZER_GENERAL
 * with CyrusSaslAllocatorsAndMutexesClient and CyrusSaslClientContext as dependents, or
 * initialize the library before calling mongo::runGlobalInitializersOrDie().
 */
MONGO_INITIALIZER_WITH_PREREQUISITES(CyrusSaslClientContext,
                                     ("NativeSaslClientContext",
                                      "CyrusSaslAllocatorsAndMutexesClient"))
(InitializerContext* context) {
    static sasl_callback_t saslClientGlobalCallbacks[] = {
        {SASL_CB_LOG, SaslCallbackFn(saslClientLogSwallow), nullptr /* context */},
        {SASL_CB_VERIFYFILE, SaslCallbackFn(saslClientVerifyPluginFile), nullptr /*context*/},
        {SASL_CB_LIST_END}};

    // If the client application has previously called sasl_client_init(), the callbacks passed
    // in here are ignored.
    //
    // TODO: Call sasl_client_done() at shutdown when we have a story for orderly shutdown.
    int result = sasl_client_init(saslClientGlobalCallbacks);
    if (result != SASL_OK) {
        uasserted(ErrorCodes::UnknownError,
                  str::stream() << "Could not initialize sasl client components ("
                                << sasl_errstring(result, nullptr, nullptr) << ")");
    }

    SaslClientSession::create = createCyrusSaslClientSession;
}

/**
 * Callback registered on the sasl_conn_t underlying a CyrusSaslClientSession to allow the Cyrus
 * SASL library to query for the authentication id and other simple string configuration parameters.
 *
 * Note that in Mongo, the authentication and authorization ids (authid and authzid) are always
 * the same.  These correspond to SASL_CB_AUTHNAME and SASL_CB_USER.
 */
int saslClientGetSimple(void* context, int id, const char** result, unsigned* resultLen) {
    try {
        CyrusSaslClientSession* session = static_cast<CyrusSaslClientSession*>(context);
        if (!session || !result)
            return SASL_BADPARAM;

        CyrusSaslClientSession::Parameter requiredParameterId;
        switch (id) {
            case SASL_CB_AUTHNAME:
            case SASL_CB_USER:
                requiredParameterId = CyrusSaslClientSession::parameterUser;
                break;
            default:
                return SASL_FAIL;
        }

        if (!session->hasParameter(requiredParameterId))
            return SASL_FAIL;
        StringData value = session->getParameter(requiredParameterId);
        *result = value.data();
        if (resultLen)
            *resultLen = static_cast<unsigned>(value.size());
        return SASL_OK;
    } catch (...) {
        return SASL_FAIL;
    }
}

/**
 * Callback registered on the sasl_conn_t underlying a CyrusSaslClientSession to allow
 * the Cyrus SASL library to query for the password data.
 */
int saslClientGetPassword(sasl_conn_t* conn, void* context, int id, sasl_secret_t** outSecret) {
    try {
        CyrusSaslClientSession* session = static_cast<CyrusSaslClientSession*>(context);
        if (!session || !outSecret)
            return SASL_BADPARAM;

        sasl_secret_t* secret = session->getPasswordAsSecret();
        if (secret == nullptr) {
            saslSetError(conn, "No password data provided");
            return SASL_FAIL;
        }

        *outSecret = secret;
        return SASL_OK;
    } catch (...) {
        StringBuilder sb;
        sb << "Caught unhandled exception in saslClientGetSimple: " << exceptionToStatus().reason();
        saslSetError(conn, sb.str());
        return SASL_FAIL;
    }
}
}  // namespace

CyrusSaslClientSession::CyrusSaslClientSession()
    : SaslClientSession(), _saslConnection(nullptr), _step(0), _success(false) {
    const sasl_callback_t callbackTemplate[maxCallbacks] = {
        {SASL_CB_AUTHNAME, SaslCallbackFn(saslClientGetSimple), this},
        {SASL_CB_USER, SaslCallbackFn(saslClientGetSimple), this},
        {SASL_CB_PASS, SaslCallbackFn(saslClientGetPassword), this},
        {SASL_CB_LIST_END}};
    std::copy(callbackTemplate, callbackTemplate + maxCallbacks, _callbacks);
}

CyrusSaslClientSession::~CyrusSaslClientSession() {
    sasl_dispose(&_saslConnection);
}

void CyrusSaslClientSession::setParameter(Parameter id, StringData value) {
    fassert(18665, id >= 0 && id < numParameters);
    if (id == parameterPassword) {
        // The parameterPassword is stored as a sasl_secret_t,  while other
        // parameters are stored directly.  This facilitates memory ownership management for
        // getPasswordAsSecret().
        _secret.reset(new char[sizeof(sasl_secret_t) + value.size() + 1]);
        sasl_secret_t* secret = static_cast<sasl_secret_t*>(static_cast<void*>(_secret.get()));
        secret->len = value.size();
        value.copy(static_cast<char*>(static_cast<void*>(&secret->data[0])), value.size());
    }
    SaslClientSession::setParameter(id, value);
}

sasl_secret_t* CyrusSaslClientSession::getPasswordAsSecret() {
    // See comment in setParameter() about the special storage of parameterPassword.
    return static_cast<sasl_secret_t*>(static_cast<void*>(_secret.get()));
}

Status CyrusSaslClientSession::initialize() {
    if (_saslConnection != nullptr)
        return Status(ErrorCodes::AlreadyInitialized,
                      "Cannot reinitialize CyrusSaslClientSession.");

    int result = sasl_client_new(std::string{getParameter(parameterServiceName)}.c_str(),
                                 std::string{getParameter(parameterServiceHostname)}.c_str(),
                                 nullptr,
                                 nullptr,
                                 _callbacks,
                                 0,
                                 &_saslConnection);

    if (SASL_OK != result) {
        return Status(ErrorCodes::UnknownError,
                      str::stream() << sasl_errstring(result, nullptr, nullptr));
    }

    return Status::OK();
}

Status CyrusSaslClientSession::step(StringData inputData, std::string* outputData) {
    const char* output = nullptr;
    unsigned outputSize = 0xFFFFFFFF;

    int result;
    if (_step == 0) {
        const char* actualMechanism;
        result = sasl_client_start(_saslConnection,
                                   std::string{getParameter(parameterMechanism)}.c_str(),
                                   nullptr,
                                   &output,
                                   &outputSize,
                                   &actualMechanism);
    } else {
        result = sasl_client_step(_saslConnection,
                                  inputData.data(),
                                  static_cast<unsigned>(inputData.size()),
                                  nullptr,
                                  &output,
                                  &outputSize);
    }
    ++_step;
    switch (result) {
        case SASL_OK:
            _success = true;
            [[fallthrough]];
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
