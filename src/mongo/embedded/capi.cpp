/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/embedded/capi.h"

#include <cstring>
#include <exception>
#include <thread>
#include <unordered_map>
#include <vector>

#include "mongo/db/client.h"
#include "mongo/db/dbmain.h"
#include "mongo/db/service_context.h"
#include "mongo/embedded/embedded.h"
#include "mongo/embedded/embedded_log_appender.h"
#include "mongo/logger/logger.h"
#include "mongo/logger/message_event_utf8_encoder.h"
#include "mongo/rpc/message.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/shared_buffer.h"

struct mongo_embedded_v1_status {
    mongo_embedded_v1_status() noexcept = default;
    mongo_embedded_v1_status(const mongo_embedded_v1_error e, const int ec, std::string w)
        : error(e), exception_code(ec), what(std::move(w)) {}

    void clean() noexcept {
        error = MONGO_EMBEDDED_V1_SUCCESS;
    }

    mongo_embedded_v1_error error = MONGO_EMBEDDED_V1_SUCCESS;
    int exception_code = 0;
    std::string what;
};

namespace mongo {
namespace {
class MobileException : public std::exception {
public:
    explicit MobileException(const mongo_embedded_v1_error code, std::string m)
        : _mesg(std::move(m)), _code(code) {}

    mongo_embedded_v1_error mobileCode() const noexcept {
        return this->_code;
    }

    const char* what() const noexcept final {
        return this->_mesg.c_str();
    }

private:
    std::string _mesg;
    mongo_embedded_v1_error _code;
};

mongo_embedded_v1_status translateException() try { throw; } catch (const MobileException& ex) {
    return {ex.mobileCode(), mongo::ErrorCodes::InternalError, ex.what()};
} catch (const ExceptionFor<ErrorCodes::ReentrancyNotAllowed>& ex) {
    return {MONGO_EMBEDDED_V1_ERROR_REENTRANCY_NOT_ALLOWED, ex.code(), ex.what()};
} catch (const DBException& ex) {
    return {MONGO_EMBEDDED_V1_ERROR_EXCEPTION, ex.code(), ex.what()};
} catch (const std::bad_alloc& ex) {
    return {MONGO_EMBEDDED_V1_ERROR_ENOMEM, mongo::ErrorCodes::InternalError, ex.what()};
} catch (const std::exception& ex) {
    return {MONGO_EMBEDDED_V1_ERROR_UNKNOWN, mongo::ErrorCodes::InternalError, ex.what()};
} catch (...) {
    return {MONGO_EMBEDDED_V1_ERROR_UNKNOWN,
            mongo::ErrorCodes::InternalError,
            "Unknown error encountered in performing requested libmongodbcapi operation"};
}

std::nullptr_t handleException(mongo_embedded_v1_status& status) noexcept {
    try {
        status = translateException();
    } catch (...) {
        status.error = MONGO_EMBEDDED_V1_ERROR_IN_REPORTING_ERROR;

        try {
            status.exception_code = -1;

            status.what.clear();

            // Expected to be small enough to fit in the capacity that string always has.
            const char severeErrorMessage[] = "Severe Error";

            if (status.what.capacity() > sizeof(severeErrorMessage)) {
                status.what = severeErrorMessage;
            }
        } catch (...) /* Ignore any errors at this point. */
        {
        }
    }
    return nullptr;
}

}  // namespace
}  // namespace mongo

struct mongo_embedded_v1_lib {
    ~mongo_embedded_v1_lib() {
        invariant(this->databaseCount.load() == 0);

        if (this->logCallbackHandle) {
            using mongo::logger::globalLogDomain;
            globalLogDomain()->detachAppender(this->logCallbackHandle);
            this->logCallbackHandle.reset();
        }
    }

    mongo_embedded_v1_lib(const mongo_embedded_v1_lib&) = delete;
    void operator=(const mongo_embedded_v1_lib) = delete;

    mongo_embedded_v1_lib() = default;

    mongo::AtomicWord<int> databaseCount;

    mongo::logger::ComponentMessageLogDomain::AppenderHandle logCallbackHandle;

    std::unique_ptr<mongo_embedded_v1_instance> onlyDB;
};

namespace mongo {
namespace {
struct ServiceContextDestructor {
    void operator()(mongo::ServiceContext* const serviceContext) const noexcept {
        ::mongo::embedded::shutdown(serviceContext);
    }
};

using EmbeddedServiceContextPtr = std::unique_ptr<mongo::ServiceContext, ServiceContextDestructor>;
}  // namespace
}  // namespace mongo

struct mongo_embedded_v1_instance {
    ~mongo_embedded_v1_instance() {
        invariant(this->clientCount.load() == 0);
        this->parentLib->databaseCount.subtractAndFetch(1);
    }

    mongo_embedded_v1_instance(const mongo_embedded_v1_instance&) = delete;
    mongo_embedded_v1_instance& operator=(const mongo_embedded_v1_instance&) = delete;

    explicit mongo_embedded_v1_instance(mongo_embedded_v1_lib* const p,
                                        const char* const yaml_config)
        : parentLib(p),
          serviceContext(::mongo::embedded::initialize(yaml_config)),
          // creating mock transport layer to be able to create sessions
          transportLayer(std::make_unique<mongo::transport::TransportLayerMock>()) {
        if (!this->serviceContext) {
            throw ::mongo::MobileException{
                MONGO_EMBEDDED_V1_ERROR_DB_INITIALIZATION_FAILED,
                "The MongoDB Embedded Library Failed to initialize the Service Context"};
        }

        this->parentLib->databaseCount.addAndFetch(1);
    }

    mongo_embedded_v1_lib* parentLib;
    mongo::AtomicWord<int> clientCount;

    mongo::EmbeddedServiceContextPtr serviceContext;
    std::unique_ptr<mongo::transport::TransportLayerMock> transportLayer;
};

struct mongo_embedded_v1_client {
    ~mongo_embedded_v1_client() {
        this->parent_db->clientCount.subtractAndFetch(1);
    }

    explicit mongo_embedded_v1_client(mongo_embedded_v1_instance* const db)
        : parent_db(db),
          client(db->serviceContext->makeClient("embedded", db->transportLayer->createSession())) {
        this->parent_db->clientCount.addAndFetch(1);
    }

    mongo_embedded_v1_client(const mongo_embedded_v1_client&) = delete;
    mongo_embedded_v1_client& operator=(const mongo_embedded_v1_client&) = delete;

    mongo_embedded_v1_instance* const parent_db;
    mongo::ServiceContext::UniqueClient client;

    std::vector<unsigned char> output;
    mongo::DbResponse response;
};

namespace mongo {
namespace {

std::unique_ptr<mongo_embedded_v1_lib> library;

class ReentrancyGuard {
private:
    thread_local static bool inLibrary;

public:
    explicit ReentrancyGuard() {
        uassert(ErrorCodes::ReentrancyNotAllowed,
                str::stream() << "Reentry into libmongodbcapi is not allowed",
                !inLibrary);
        inLibrary = true;
    }

    ~ReentrancyGuard() {
        inLibrary = false;
    }

    ReentrancyGuard(ReentrancyGuard const&) = delete;
    ReentrancyGuard& operator=(ReentrancyGuard const&) = delete;
};

thread_local bool ReentrancyGuard::inLibrary = false;

void registerLogCallback(mongo_embedded_v1_lib* const lib,
                         const mongo_embedded_v1_log_callback logCallback,
                         void* const logUserData) {
    using logger::globalLogDomain;
    using logger::MessageEventEphemeral;
    using logger::MessageEventUnadornedEncoder;

    lib->logCallbackHandle = globalLogDomain()->attachAppender(
        std::make_unique<embedded::EmbeddedLogAppender<MessageEventEphemeral>>(
            logCallback, logUserData, std::make_unique<MessageEventUnadornedEncoder>()));
}

mongo_embedded_v1_lib* capi_lib_init(mongo_embedded_v1_init_params const* params,
                                     mongo_embedded_v1_status& status) try {
    if (library) {
        throw MobileException{
            MONGO_EMBEDDED_V1_ERROR_LIBRARY_ALREADY_INITIALIZED,
            "Cannot initialize the MongoDB Embedded Library when it is already initialized."};
    }

    auto lib = std::make_unique<mongo_embedded_v1_lib>();

    // TODO(adam.martin): Fold all of this log initialization into the ctor of lib.
    if (params) {
        using logger::globalLogManager;
        // The standard console log appender may or may not be installed here, depending if this is
        // the first time we initialize the library or not. Make sure we handle both cases.
        if (params->log_flags & MONGO_EMBEDDED_V1_LOG_STDOUT) {
            if (!globalLogManager()->isDefaultConsoleAppenderAttached())
                globalLogManager()->reattachDefaultConsoleAppender();
        } else {
            if (globalLogManager()->isDefaultConsoleAppenderAttached())
                globalLogManager()->detachDefaultConsoleAppender();
        }

        if ((params->log_flags & MONGO_EMBEDDED_V1_LOG_CALLBACK) && params->log_callback) {
            registerLogCallback(lib.get(), params->log_callback, params->log_user_data);
        }
    }

    library = std::move(lib);

    return library.get();
} catch (...) {
    // Make sure that no actual logger is attached if library cannot be initialized.  Also prevent
    // exception leaking failures here.
    []() noexcept {
        using logger::globalLogManager;
        if (globalLogManager()->isDefaultConsoleAppenderAttached())
            globalLogManager()->detachDefaultConsoleAppender();
    }
    ();
    throw;
}

void capi_lib_fini(mongo_embedded_v1_lib* const lib, mongo_embedded_v1_status& status) {
    if (!lib) {
        throw MobileException{
            MONGO_EMBEDDED_V1_ERROR_INVALID_LIB_HANDLE,
            "Cannot close a `NULL` pointer referencing a MongoDB Embedded Library Instance"};
    }

    if (!library) {
        throw MobileException{
            MONGO_EMBEDDED_V1_ERROR_LIBRARY_NOT_INITIALIZED,
            "Cannot close the MongoDB Embedded Library when it is not initialized"};
    }

    if (library.get() != lib) {
        throw MobileException{MONGO_EMBEDDED_V1_ERROR_INVALID_LIB_HANDLE,
                              "Invalid MongoDB Embedded Library handle."};
    }

    // This check is not possible to 100% guarantee.  It is a best effort.  The documentation of
    // this API says that the behavior of closing a `lib` with open handles is undefined, but may
    // provide diagnostic errors in some circumstances.
    if (lib->databaseCount.load() > 0) {
        throw MobileException{
            MONGO_EMBEDDED_V1_ERROR_HAS_DB_HANDLES_OPEN,
            "Cannot close the MongoDB Embedded Library when it has database handles still open."};
    }

    library = nullptr;
}

mongo_embedded_v1_instance* instance_new(mongo_embedded_v1_lib* const lib,
                                         const char* const yaml_config,
                                         mongo_embedded_v1_status& status) {
    if (!library) {
        throw MobileException{MONGO_EMBEDDED_V1_ERROR_LIBRARY_NOT_INITIALIZED,
                              "Cannot create a new database handle when the MongoDB Embedded "
                              "Library is not yet initialized."};
    }

    if (library.get() != lib) {
        throw MobileException{MONGO_EMBEDDED_V1_ERROR_INVALID_LIB_HANDLE,
                              "Cannot create a new database handle when the MongoDB Embedded "
                              "Library is not yet initialized."};
    }

    if (lib->onlyDB) {
        throw MobileException{MONGO_EMBEDDED_V1_ERROR_DB_MAX_OPEN,
                              "The maximum number of permitted database handles for the MongoDB "
                              "Embedded Library have been opened."};
    }

    lib->onlyDB = std::make_unique<mongo_embedded_v1_instance>(lib, yaml_config);

    return lib->onlyDB.get();
}

void instance_destroy(mongo_embedded_v1_instance* const db, mongo_embedded_v1_status& status) {
    if (!library) {
        throw MobileException{MONGO_EMBEDDED_V1_ERROR_LIBRARY_NOT_INITIALIZED,
                              "Cannot destroy a database handle when the MongoDB Embedded Library "
                              "is not yet initialized."};
    }

    if (!db) {
        throw MobileException{
            MONGO_EMBEDDED_V1_ERROR_INVALID_DB_HANDLE,
            "Cannot close a `NULL` pointer referencing a MongoDB Embedded Database"};
    }

    if (db != library->onlyDB.get()) {
        throw MobileException{
            MONGO_EMBEDDED_V1_ERROR_INVALID_DB_HANDLE,
            "Cannot close the specified MongoDB Embedded Database, as it is not a valid instance."};
    }

    if (db->clientCount.load() > 0) {
        throw MobileException{
            MONGO_EMBEDDED_V1_ERROR_DB_CLIENTS_OPEN,
            "Cannot close a MongoDB Embedded Database instance while it has open clients"};
    }

    library->onlyDB = nullptr;
}

mongo_embedded_v1_client* client_new(mongo_embedded_v1_instance* const db,
                                     mongo_embedded_v1_status& status) {
    if (!library) {
        throw MobileException{MONGO_EMBEDDED_V1_ERROR_LIBRARY_NOT_INITIALIZED,
                              "Cannot create a new client handle when the MongoDB Embedded Library "
                              "is not yet initialized."};
    }

    if (!db) {
        throw MobileException{MONGO_EMBEDDED_V1_ERROR_INVALID_DB_HANDLE,
                              "Cannot use a `NULL` pointer referencing a MongoDB Embedded Database "
                              "when creating a new client"};
    }

    if (db != library->onlyDB.get()) {
        throw MobileException{MONGO_EMBEDDED_V1_ERROR_INVALID_DB_HANDLE,
                              "The specified MongoDB Embedded Database instance cannot be used to "
                              "create a new client because it is invalid."};
    }

    return new mongo_embedded_v1_client(db);
}

void client_destroy(mongo_embedded_v1_client* const client, mongo_embedded_v1_status& status) {
    if (!library) {
        throw MobileException(MONGO_EMBEDDED_V1_ERROR_LIBRARY_NOT_INITIALIZED,
                              "Cannot destroy a database handle when the MongoDB Embedded Library "
                              "is not yet initialized.");
    }

    if (!client) {
        throw MobileException{
            MONGO_EMBEDDED_V1_ERROR_INVALID_CLIENT_HANDLE,
            "Cannot destroy a `NULL` pointer referencing a MongoDB Embedded Database Client"};
    }

    delete client;
}

class ClientGuard {
    ClientGuard(const ClientGuard&) = delete;
    void operator=(const ClientGuard&) = delete;

public:
    explicit ClientGuard(mongo_embedded_v1_client* const client) : _client(client) {
        mongo::Client::setCurrent(std::move(client->client));
    }

    ~ClientGuard() {
        _client->client = mongo::Client::releaseCurrent();
    }

private:
    mongo_embedded_v1_client* const _client;
};

void client_wire_protocol_rpc(mongo_embedded_v1_client* const client,
                              const void* input,
                              const size_t input_size,
                              void** const output,
                              size_t* const output_size,
                              mongo_embedded_v1_status& status) {
    ClientGuard clientGuard(client);

    auto opCtx = cc().makeOperationContext();
    auto sep = client->parent_db->serviceContext->getServiceEntryPoint();

    auto sb = SharedBuffer::allocate(input_size);
    memcpy(sb.get(), input, input_size);

    Message msg(std::move(sb));

    client->response = sep->handleRequest(opCtx.get(), msg);

    MsgData::View outMessage(client->response.response.buf());
    outMessage.setId(nextMessageId());
    outMessage.setResponseToMsgId(msg.header().getId());

    // The results of the computations used to fill out-parameters need to be captured and processed
    // before setting the output parameters themselves, in order to maintain the strong-guarantee
    // part of the contract of this function.
    auto outParams =
        std::make_tuple(client->response.response.size(), client->response.response.buf());

    // We force the output parameters to be set in a `noexcept` enabled way.  If the operation
    // itself
    // is safely noexcept, we just run it, otherwise we force a `noexcept` over it to catch errors.
    if (noexcept(std::tie(*output_size, *output) = std::move(outParams))) {
        std::tie(*output_size, *output) = std::move(outParams);
    } else {
        // Assigning primitives in a tied tuple should be noexcept, so we force it to be so, for
        // our purposes.  This facilitates a runtime check should something WEIRD happen.
        [ output, output_size, &outParams ]() noexcept {
            std::tie(*output_size, *output) = std::move(outParams);
        }
        ();
    }
}

int capi_status_get_error(const mongo_embedded_v1_status* const status) noexcept {
    invariant(status);
    return status->error;
}

const char* capi_status_get_what(const mongo_embedded_v1_status* const status) noexcept {
    invariant(status);
    return status->what.c_str();
}

int capi_status_get_code(const mongo_embedded_v1_status* const status) noexcept {
    invariant(status);
    return status->exception_code;
}

template <typename Function,
          typename ReturnType =
              decltype(std::declval<Function>()(*std::declval<mongo_embedded_v1_status*>()))>
struct enterCXXImpl;

template <typename Function>
struct enterCXXImpl<Function, void> {
    template <typename Callable>
    static int call(Callable&& function, mongo_embedded_v1_status& status) noexcept {
        try {
            ReentrancyGuard singleEntrant;
            function(status);
        } catch (...) {
            handleException(status);
        }
        return status.error;
    }
};


template <typename Function, typename Pointer>
struct enterCXXImpl<Function, Pointer*> {
    template <typename Callable>
    static Pointer* call(Callable&& function, mongo_embedded_v1_status& status) noexcept try {
        ReentrancyGuard singleEntrant;
        return function(status);
    } catch (...) {
        return handleException(status);
    }
};
}  // namespace
}  // namespace mongo

namespace {
struct StatusGuard {
private:
    mongo_embedded_v1_status* status;
    mongo_embedded_v1_status fallback;

public:
    explicit StatusGuard(mongo_embedded_v1_status* const statusPtr) noexcept : status(statusPtr) {
        if (status)
            status->clean();
    }

    mongo_embedded_v1_status& get() noexcept {
        return status ? *status : fallback;
    }

    const mongo_embedded_v1_status& get() const noexcept {
        return status ? *status : fallback;
    }

    operator mongo_embedded_v1_status&() & noexcept {
        return this->get();
    }
    operator mongo_embedded_v1_status&() && noexcept {
        return this->get();
    }
};

template <typename Callable>
auto enterCXX(mongo_embedded_v1_status* const statusPtr, Callable&& c) noexcept
    -> decltype(mongo::enterCXXImpl<Callable>::call(std::forward<Callable>(c), *statusPtr)) {
    StatusGuard status(statusPtr);
    return mongo::enterCXXImpl<Callable>::call(std::forward<Callable>(c), status);
}
}  // namespace

extern "C" {
mongo_embedded_v1_lib* mongo_embedded_v1_lib_init(const mongo_embedded_v1_init_params* const params,
                                                  mongo_embedded_v1_status* const statusPtr) {
    return enterCXX(statusPtr, [&](mongo_embedded_v1_status& status) {
        return mongo::capi_lib_init(params, status);
    });
}

int mongo_embedded_v1_lib_fini(mongo_embedded_v1_lib* const lib,
                               mongo_embedded_v1_status* const statusPtr) {
    return enterCXX(statusPtr, [&](mongo_embedded_v1_status& status) {
        return mongo::capi_lib_fini(lib, status);
    });
}

mongo_embedded_v1_instance* mongo_embedded_v1_instance_create(
    mongo_embedded_v1_lib* lib,
    const char* const yaml_config,
    mongo_embedded_v1_status* const statusPtr) {
    return enterCXX(statusPtr, [&](mongo_embedded_v1_status& status) {
        return mongo::instance_new(lib, yaml_config, status);
    });
}

int mongo_embedded_v1_instance_destroy(mongo_embedded_v1_instance* const db,
                                       mongo_embedded_v1_status* const statusPtr) {
    return enterCXX(statusPtr, [&](mongo_embedded_v1_status& status) {
        return mongo::instance_destroy(db, status);
    });
}

mongo_embedded_v1_client* mongo_embedded_v1_client_create(
    mongo_embedded_v1_instance* const db, mongo_embedded_v1_status* const statusPtr) {
    return enterCXX(
        statusPtr, [&](mongo_embedded_v1_status& status) { return mongo::client_new(db, status); });
}

int mongo_embedded_v1_client_destroy(mongo_embedded_v1_client* const client,
                                     mongo_embedded_v1_status* const statusPtr) {
    return enterCXX(statusPtr, [&](mongo_embedded_v1_status& status) {
        return mongo::client_destroy(client, status);
    });
}

int mongo_embedded_v1_client_invoke(mongo_embedded_v1_client* const client,
                                    const void* input,
                                    const size_t input_size,
                                    void** const output,
                                    size_t* const output_size,
                                    mongo_embedded_v1_status* const statusPtr) {
    return enterCXX(statusPtr, [&](mongo_embedded_v1_status& status) {
        return mongo::client_wire_protocol_rpc(
            client, input, input_size, output, output_size, status);
    });
}

int mongo_embedded_v1_status_get_error(const mongo_embedded_v1_status* const status) {
    return mongo::capi_status_get_error(status);
}

const char* mongo_embedded_v1_status_get_explanation(const mongo_embedded_v1_status* const status) {
    return mongo::capi_status_get_what(status);
}

int mongo_embedded_v1_status_get_code(const mongo_embedded_v1_status* const status) {
    return mongo::capi_status_get_code(status);
}

mongo_embedded_v1_status* mongo_embedded_v1_status_create(void) {
    return new mongo_embedded_v1_status;
}

void mongo_embedded_v1_status_destroy(mongo_embedded_v1_status* const status) {
    delete status;
}

}  // extern "C"
