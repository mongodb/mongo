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

#include "mongo/client/embedded/libmongodbcapi.h"

#include <exception>
#include <thread>
#include <unordered_map>
#include <vector>

#include "mongo/client/embedded/embedded.h"
#include "mongo/client/embedded/embedded_log_appender.h"
#include "mongo/db/client.h"
#include "mongo/db/dbmain.h"
#include "mongo/db/service_context.h"
#include "mongo/logger/logger.h"
#include "mongo/logger/message_event_utf8_encoder.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/message.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/shared_buffer.h"

struct libmongodbcapi_db {
    libmongodbcapi_db() = default;

    libmongodbcapi_db(const libmongodbcapi_db&) = delete;
    libmongodbcapi_db& operator=(const libmongodbcapi_db&) = delete;

    mongo::ServiceContext* serviceContext = nullptr;
    mongo::stdx::unordered_map<libmongodbcapi_client*, std::unique_ptr<libmongodbcapi_client>>
        open_clients;
    std::unique_ptr<mongo::transport::TransportLayerMock> transportLayer;
};
struct libmongodbcapi_client {
    libmongodbcapi_client(libmongodbcapi_db* db) : parent_db(db) {}

    libmongodbcapi_client(const libmongodbcapi_client&) = delete;
    libmongodbcapi_client& operator=(const libmongodbcapi_client&) = delete;

    void* client_handle = nullptr;
    std::vector<unsigned char> output;
    libmongodbcapi_db* parent_db = nullptr;
    mongo::ServiceContext::UniqueClient client;
    mongo::DbResponse response;
};

namespace mongo {
namespace {

bool libraryInitialized_ = false;
libmongodbcapi_db* global_db = nullptr;
mongo::logger::ComponentMessageLogDomain::AppenderHandle logCallbackHandle;
thread_local int last_error = LIBMONGODB_CAPI_SUCCESS;
thread_local int callEntryDepth = 0;

class ReentrancyGuard {
public:
    explicit ReentrancyGuard() {
        uassert(ErrorCodes::ReentrancyNotAllowed,
                str::stream() << "Reentry into libmongodbcapi is not allowed",
                callEntryDepth == 0);
        ++callEntryDepth;
    }

    ~ReentrancyGuard() {
        --callEntryDepth;
    }

    ReentrancyGuard(ReentrancyGuard const&) = delete;
    ReentrancyGuard& operator=(ReentrancyGuard const&) = delete;
};

int register_log_callback(libmongodbcapi_log_callback log_callback, void* log_user_data) {
    using namespace logger;

    logCallbackHandle = globalLogDomain()->attachAppender(
        std::make_unique<embedded::EmbeddedLogAppender<MessageEventEphemeral>>(
            log_callback, log_user_data, std::make_unique<MessageEventUnadornedEncoder>()));

    return LIBMONGODB_CAPI_SUCCESS;
}

int unregister_log_callback() {
    using namespace logger;

    globalLogDomain()->detachAppender(logCallbackHandle);
    logCallbackHandle.reset();

    return LIBMONGODB_CAPI_SUCCESS;
}

int init(libmongodbcapi_init_params const* params) noexcept try {
    using namespace logger;

    ReentrancyGuard guard;

    if (libraryInitialized_)
        return LIBMONGODB_CAPI_ERROR_LIBRARY_ALREADY_INITIALIZED;

    int result = LIBMONGODB_CAPI_SUCCESS;
    if (params) {
        // The standard console log appender may or may not be installed here, depending if this is
        // the first time we initialize the library or not. Make sure we handle both cases.
        if (params->log_flags & LIBMONGODB_CAPI_LOG_STDOUT) {
            if (!globalLogManager()->isDefaultConsoleAppenderAttached())
                globalLogManager()->reattachDefaultConsoleAppender();
        } else {
            if (globalLogManager()->isDefaultConsoleAppenderAttached())
                globalLogManager()->detachDefaultConsoleAppender();
        }

        if ((params->log_flags & LIBMONGODB_CAPI_LOG_CALLBACK) && params->log_callback) {
            result = register_log_callback(params->log_callback, params->log_user_data);
            if (result != LIBMONGODB_CAPI_SUCCESS)
                return result;
        }
    }

    libraryInitialized_ = true;
    return result;
} catch (const std::exception&) {
    return LIBMONGODB_CAPI_ERROR_UNKNOWN;
}

int fini() noexcept try {
    ReentrancyGuard guard;

    if (!libraryInitialized_)
        return LIBMONGODB_CAPI_ERROR_LIBRARY_NOT_INITIALIZED;

    if (global_db)
        return LIBMONGODB_CAPI_ERROR_DB_OPEN;

    int result = LIBMONGODB_CAPI_SUCCESS;
    if (logCallbackHandle) {
        result = unregister_log_callback();
        if (result != LIBMONGODB_CAPI_SUCCESS)
            return result;
    }

    libraryInitialized_ = false;

    return result;
} catch (const std::exception&) {
    return LIBMONGODB_CAPI_ERROR_UNKNOWN;
}

libmongodbcapi_db* db_new(const char* yaml_config) noexcept try {
    ReentrancyGuard guard;

    last_error = LIBMONGODB_CAPI_SUCCESS;
    if (!libraryInitialized_)
        throw std::runtime_error("libmongodbcapi_init not called");
    if (global_db) {
        throw std::runtime_error("DB already exists");
    }
    global_db = new libmongodbcapi_db;

    global_db->serviceContext = embedded::initialize(yaml_config);
    if (!global_db->serviceContext) {
        delete global_db;
        global_db = nullptr;
        throw std::runtime_error("Initialization failed");
    }

    // creating mock transport layer to be able to create sessions
    global_db->transportLayer = stdx::make_unique<transport::TransportLayerMock>();

    return global_db;
} catch (const std::exception&) {
    last_error = LIBMONGODB_CAPI_ERROR_UNKNOWN;
    return nullptr;
}

int db_destroy(libmongodbcapi_db* db) noexcept {
    if (!db->open_clients.empty()) {
        last_error = LIBMONGODB_CAPI_ERROR_UNKNOWN;
        return last_error;
    }

    embedded::shutdown(global_db->serviceContext);

    delete db;
    invariant(!db || db == global_db);
    if (db) {
        global_db = nullptr;
    }
    last_error = LIBMONGODB_CAPI_SUCCESS;
    return last_error;
}

int db_pump(libmongodbcapi_db* db) noexcept try {
    ReentrancyGuard guard;

    return LIBMONGODB_CAPI_SUCCESS;
} catch (const std::exception&) {
    return LIBMONGODB_CAPI_ERROR_UNKNOWN;
}

libmongodbcapi_client* client_new(libmongodbcapi_db* db) noexcept try {
    ReentrancyGuard guard;

    auto new_client = stdx::make_unique<libmongodbcapi_client>(db);
    libmongodbcapi_client* rv = new_client.get();
    db->open_clients.insert(std::make_pair(rv, std::move(new_client)));

    auto session = global_db->transportLayer->createSession();
    rv->client = global_db->serviceContext->makeClient("embedded", std::move(session));

    last_error = LIBMONGODB_CAPI_SUCCESS;
    return rv;
} catch (const std::exception&) {
    last_error = LIBMONGODB_CAPI_ERROR_UNKNOWN;
    return nullptr;
}

void client_destroy(libmongodbcapi_client* client) noexcept {
    last_error = LIBMONGODB_CAPI_SUCCESS;
    if (!client) {
        return;
    }
    client->parent_db->open_clients.erase(client);
}

int client_wire_protocol_rpc(libmongodbcapi_client* client,
                             const void* input,
                             size_t input_size,
                             void** output,
                             size_t* output_size) noexcept try {
    ReentrancyGuard reentry_guard;

    mongo::Client::setCurrent(std::move(client->client));
    const auto guard = mongo::MakeGuard([&] { client->client = mongo::Client::releaseCurrent(); });

    auto opCtx = cc().makeOperationContext();
    auto sep = client->parent_db->serviceContext->getServiceEntryPoint();

    auto sb = SharedBuffer::allocate(input_size);
    memcpy(sb.get(), input, input_size);

    Message msg(std::move(sb));

    client->response = sep->handleRequest(opCtx.get(), msg);
    *output_size = client->response.response.size();
    *output = (void*)client->response.response.buf();

    return LIBMONGODB_CAPI_SUCCESS;
} catch (const std::exception&) {
    return LIBMONGODB_CAPI_ERROR_UNKNOWN;
}

int get_last_capi_error() noexcept {
    return last_error;
}
}  // namespace
}  // namespace mongo

extern "C" {
int libmongodbcapi_init(const libmongodbcapi_init_params* params) {
    return mongo::init(params);
}

int libmongodbcapi_fini() {
    return mongo::fini();
}

libmongodbcapi_db* libmongodbcapi_db_new(const char* yaml_config) {
    return mongo::db_new(yaml_config);
}

int libmongodbcapi_db_destroy(libmongodbcapi_db* db) {
    return mongo::db_destroy(db);
}

int libmongodbcapi_db_pump(libmongodbcapi_db* p) {
    return mongo::db_pump(p);
}

libmongodbcapi_client* libmongodbcapi_db_client_new(libmongodbcapi_db* db) {
    return mongo::client_new(db);
}

void libmongodbcapi_db_client_destroy(libmongodbcapi_client* client) {
    return mongo::client_destroy(client);
}

int libmongodbcapi_db_client_wire_protocol_rpc(libmongodbcapi_client* client,
                                               const void* input,
                                               size_t input_size,
                                               void** output,
                                               size_t* output_size) {
    return mongo::client_wire_protocol_rpc(client, input, input_size, output, output_size);
}

int libmongodbcapi_get_last_error() {
    return mongo::get_last_capi_error();
}
}
