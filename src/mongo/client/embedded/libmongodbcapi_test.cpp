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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include "mongo/client/embedded/libmongodbcapi.h"

#include <set>

#include "mongo/unittest/unittest.h"

namespace {

class MongodbCAPITest : public mongo::unittest::Test {
protected:
    void setUp() {
        db = libmongodbcapi_db_new(0, nullptr, nullptr);
        ASSERT(db != nullptr);
    }

    void tearDown() {
        libmongodbcapi_db_destroy(db);
        ASSERT_EQUALS(libmongodbcapi_get_last_error(), LIBMONGODB_CAPI_ERROR_SUCCESS);
    }

    libmongodbcapi_db* getDB() {
        return db;
    }

    libmongodbcapi_client* createClient() {
        libmongodbcapi_client* client = libmongodbcapi_db_client_new(db);
        ASSERT(client != nullptr);
        ASSERT_EQUALS(libmongodbcapi_get_last_error(), LIBMONGODB_CAPI_ERROR_SUCCESS);
        return client;
    }

    void destroyClient(libmongodbcapi_client* client) {
        ASSERT(client != nullptr);
        libmongodbcapi_db_client_destroy(client);
        ASSERT_EQUALS(libmongodbcapi_get_last_error(), LIBMONGODB_CAPI_ERROR_SUCCESS);
    }

private:
    libmongodbcapi_db* db;
};

TEST_F(MongodbCAPITest, CreateAndDestroyDB) {
    // Test the setUp() and tearDown() test fixtures
}

TEST_F(MongodbCAPITest, CreateAndDestroyDBAndClient) {
    libmongodbcapi_client* client = createClient();
    destroyClient(client);
}

// This test is to make sure that destroying the db will destroy all of its clients
// This test will only fail under ASAN
TEST_F(MongodbCAPITest, DoNotDestroyClient) {
    createClient();
}

TEST_F(MongodbCAPITest, CreateMultipleClients) {
    const int numClients = 10;
    std::set<libmongodbcapi_client*> clients;
    for (int i = 0; i < numClients; i++) {
        clients.insert(createClient());
    }

    // ensure that each client is unique by making sure that the set size equals the number of
    // clients instantiated
    ASSERT_EQUALS(static_cast<int>(clients.size()), numClients);

    for (libmongodbcapi_client* client : clients) {
        destroyClient(client);
    }
}

TEST_F(MongodbCAPITest, DBPump) {
    libmongodbcapi_db* db = getDB();
    int err = libmongodbcapi_db_pump(db);
    ASSERT_EQUALS(err, LIBMONGODB_CAPI_ERROR_SUCCESS);
}

TEST_F(MongodbCAPITest, SendMessages) {
    libmongodbcapi_client* client = createClient();

    void* output = nullptr;
    size_t output_size = 0;

    // create an arbitrary array of set size to pass into the method (does not have to be
    // null-terminated)
    const size_t inputSize1 = 100;
    const std::array<char, inputSize1> input1 = {"abcdefg"};
    int err = libmongodbcapi_db_client_wire_protocol_rpc(
        client, input1.data(), inputSize1, &output, &output_size);
    ASSERT_EQUALS(err, LIBMONGODB_CAPI_ERROR_SUCCESS);

    output = nullptr;
    output_size = 0;

    const size_t inputSize2 = 50;
    const std::array<char, inputSize1> input2 = {"123456"};
    err = libmongodbcapi_db_client_wire_protocol_rpc(
        client, input2.data(), inputSize2, &output, &output_size);
    ASSERT_EQUALS(err, LIBMONGODB_CAPI_ERROR_SUCCESS);

    destroyClient(client);
}

TEST_F(MongodbCAPITest, MultipleClientsMultipleMessages) {
    libmongodbcapi_client* client1 = createClient();
    libmongodbcapi_client* client2 = createClient();
    ASSERT(client1 != client2);

    void* output = nullptr;
    size_t output_size = 0;

    const size_t inputSize1 = 100;
    const std::array<char, inputSize1> input1 = {"abcdefg"};
    int err = libmongodbcapi_db_client_wire_protocol_rpc(
        client1, input1.data(), inputSize1, &output, &output_size);
    ASSERT_EQUALS(err, LIBMONGODB_CAPI_ERROR_SUCCESS);

    output = nullptr;
    output_size = 0;

    const size_t inputSize2 = 50;
    const std::array<char, inputSize1> input2 = {"123456"};
    err = libmongodbcapi_db_client_wire_protocol_rpc(
        client1, input2.data(), inputSize2, &output, &output_size);
    ASSERT_EQUALS(err, LIBMONGODB_CAPI_ERROR_SUCCESS);

    output = nullptr;
    output_size = 0;

    err = libmongodbcapi_db_client_wire_protocol_rpc(
        client2, input1.data(), inputSize1, &output, &output_size);
    ASSERT_EQUALS(err, LIBMONGODB_CAPI_ERROR_SUCCESS);

    output = nullptr;
    output_size = 0;

    err = libmongodbcapi_db_client_wire_protocol_rpc(
        client2, input2.data(), inputSize2, &output, &output_size);
    ASSERT_EQUALS(err, LIBMONGODB_CAPI_ERROR_SUCCESS);

    destroyClient(client1);
    destroyClient(client2);
}

// This test is temporary to make sure that only one database can be created
// This restriction may be relaxed at a later time
TEST_F(MongodbCAPITest, CreateMultipleDBs) {
    libmongodbcapi_db* db2 = libmongodbcapi_db_new(0, nullptr, nullptr);
    ASSERT(db2 == nullptr);
    ASSERT_EQUALS(libmongodbcapi_get_last_error(), LIBMONGODB_CAPI_ERROR_UNKNOWN);
}
}  // namespace
