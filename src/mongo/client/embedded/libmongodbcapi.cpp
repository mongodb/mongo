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

#include "mongo/db/client.h"
#include "mongo/db/dbmain.h"
#include "mongo/db/service_context.h"
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
    mongo::stdx::thread mongodThread;
    mongo::stdx::unordered_map<libmongodbcapi_client*, std::unique_ptr<libmongodbcapi_client>>
        open_clients;
    std::unique_ptr<mongo::transport::TransportLayerMock> transportLayer;

    std::vector<std::unique_ptr<char[]>> argvStorage;
    std::vector<char*> argvPointers;
    std::vector<std::unique_ptr<char[]>> envpStorage;
    std::vector<char*> envpPointers;
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

libmongodbcapi_db* global_db = nullptr;
thread_local int last_error = LIBMONGODB_CAPI_ERROR_SUCCESS;
bool run_setup = false;

libmongodbcapi_db* db_new(int argc, const char** argv, const char** envp) noexcept try {
    last_error = LIBMONGODB_CAPI_ERROR_SUCCESS;
    if (global_db) {
        throw std::runtime_error("DB already exists");
    }
    global_db = new libmongodbcapi_db;

    if (!run_setup) {
        // iterate over argv and copy them to argvStorage
        for (int i = 0; i < argc; i++) {
            // allocate space for the null terminator
            auto s = mongo::stdx::make_unique<char[]>(std::strlen(argv[i]) + 1);
            // copy the string + null terminator
            std::strncpy(s.get(), argv[i], std::strlen(argv[i]) + 1);
            global_db->argvPointers.push_back(s.get());
            global_db->argvStorage.push_back(std::move(s));
        }
        global_db->argvPointers.push_back(nullptr);

        // iterate over envp and copy them to envpStorage
        while (envp != nullptr && *envp != nullptr) {
            auto s = mongo::stdx::make_unique<char[]>(std::strlen(*envp) + 1);
            std::strncpy(s.get(), *envp, std::strlen(*envp) + 1);
            global_db->envpPointers.push_back(s.get());
            global_db->envpStorage.push_back(std::move(s));
            envp++;
        }
        global_db->envpPointers.push_back(nullptr);

        // call mongoDbMain() in a new thread because it currently does not terminate
        global_db->mongodThread = stdx::thread([=] {
            mongoDbMain(argc, global_db->argvPointers.data(), global_db->envpPointers.data());
        });
        global_db->mongodThread.detach();

        // wait until the global service context is not null
        global_db->serviceContext = waitAndGetGlobalServiceContext();

        // block until the global service context is initialized
        global_db->serviceContext->waitForStartupComplete();

        run_setup = true;
    } else {
        // wait until the global service context is not null
        global_db->serviceContext = waitAndGetGlobalServiceContext();
    }
    // creating mock transport layer
    global_db->transportLayer = stdx::make_unique<transport::TransportLayerMock>();

    return global_db;
} catch (const std::exception&) {
    last_error = LIBMONGODB_CAPI_ERROR_UNKNOWN;
    return nullptr;
}

void db_destroy(libmongodbcapi_db* db) noexcept {
    delete db;
    invariant(!db || db == global_db);
    if (db) {
        global_db = nullptr;
    }
    last_error = LIBMONGODB_CAPI_ERROR_SUCCESS;
}

int db_pump(libmongodbcapi_db* db) noexcept try {
    return LIBMONGODB_CAPI_ERROR_SUCCESS;
} catch (const std::exception&) {
    return LIBMONGODB_CAPI_ERROR_UNKNOWN;
}

libmongodbcapi_client* client_new(libmongodbcapi_db* db) noexcept try {
    auto new_client = stdx::make_unique<libmongodbcapi_client>(db);
    libmongodbcapi_client* rv = new_client.get();
    db->open_clients.insert(std::make_pair(rv, std::move(new_client)));

    auto session = global_db->transportLayer->createSession();
    rv->client = global_db->serviceContext->makeClient("embedded", std::move(session));

    last_error = LIBMONGODB_CAPI_ERROR_SUCCESS;
    return rv;
} catch (const std::exception&) {
    last_error = LIBMONGODB_CAPI_ERROR_UNKNOWN;
    return nullptr;
}

void client_destroy(libmongodbcapi_client* client) noexcept {
    last_error = LIBMONGODB_CAPI_ERROR_SUCCESS;
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

    return LIBMONGODB_CAPI_ERROR_SUCCESS;
} catch (const std::exception&) {
    return LIBMONGODB_CAPI_ERROR_UNKNOWN;
}

int get_last_capi_error() noexcept {
    return last_error;
}
}  // namespace
}  // namespace mongo

extern "C" {
libmongodbcapi_db* libmongodbcapi_db_new(int argc, const char** argv, const char** envp) {
    return mongo::db_new(argc, argv, envp);
}

void libmongodbcapi_db_destroy(libmongodbcapi_db* db) {
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
