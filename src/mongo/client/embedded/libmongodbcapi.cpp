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
#include <unordered_map>
#include <vector>

#include "mongo/stdx/memory.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"

struct libmongodbcapi_db {
    libmongodbcapi_db() = default;

    libmongodbcapi_db(const libmongodbcapi_db&) = delete;
    libmongodbcapi_db& operator=(const libmongodbcapi_db&) = delete;

    void* db_sc = nullptr;
    mongo::stdx::unordered_map<libmongodbcapi_client*, std::unique_ptr<libmongodbcapi_client>>
        open_clients;
};
struct libmongodbcapi_client {
    libmongodbcapi_client(libmongodbcapi_db* db) : parent_db(db) {}

    libmongodbcapi_client(const libmongodbcapi_client&) = delete;
    libmongodbcapi_client& operator=(const libmongodbcapi_client&) = delete;

    void* client_handle = nullptr;
    std::vector<unsigned char> output;
    libmongodbcapi_db* parent_db = nullptr;
};

namespace mongo {
namespace {

libmongodbcapi_db* global_db = nullptr;
thread_local int last_error = LIBMONGODB_CAPI_ERROR_SUCCESS;

libmongodbcapi_db* db_new(int argc, const char** argv, const char** envp) noexcept try {
    last_error = LIBMONGODB_CAPI_ERROR_SUCCESS;
    if (global_db) {
        throw std::runtime_error("DB already exists");
    }
    global_db = new libmongodbcapi_db;
    return global_db;
} catch (const std::exception& e) {
    last_error = LIBMONGODB_CAPI_ERROR_UNKNOWN;
    return nullptr;
}

void db_destroy(libmongodbcapi_db* db) noexcept {
    delete db;
    invariant(!db || db == global_db);
    if (db) {
        global_db = nullptr;
    }
}

int db_pump(libmongodbcapi_db* db) noexcept try {
    return LIBMONGODB_CAPI_ERROR_SUCCESS;
} catch (const std::exception& e) {
    return LIBMONGODB_CAPI_ERROR_UNKNOWN;
}

libmongodbcapi_client* client_new(libmongodbcapi_db* db) noexcept try {
    auto new_client = stdx::make_unique<libmongodbcapi_client>(db);
    libmongodbcapi_client* rv = new_client.get();
    db->open_clients.insert(std::make_pair(rv, std::move(new_client)));
    return rv;
} catch (const std::exception& e) {
    return nullptr;
}

void client_destroy(libmongodbcapi_client* client) noexcept {
    if (!client) {
        return;
    }
    client->parent_db->open_clients.erase(client);
}

int client_wire_protocol_rpc(libmongodbcapi_client* client,
                             const void* input,
                             size_t input_size,
                             void** output,
                             size_t* output_size) noexcept {
    return LIBMONGODB_CAPI_ERROR_SUCCESS;
}

int get_last_error() noexcept {
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

libmongodbcapi_client* libmongdbcapi_db_client_new(libmongodbcapi_db* db) {
    return mongo::client_new(db);
}

void libmongdbcapi_db_client_destroy(libmongodbcapi_client* client) {
    return mongo::client_destroy(client);
}

int libmongdbcapi_db_client_wire_protocol_rpc(libmongodbcapi_client* client,
                                              const void* input,
                                              size_t input_size,
                                              void** output,
                                              size_t* output_size) {
    return mongo::client_wire_protocol_rpc(client, input, input_size, output, output_size);
}
}
