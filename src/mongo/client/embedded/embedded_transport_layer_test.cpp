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


#include "mongo/client/embedded/embedded_transport_layer.h"
#include "mongo/client/embedded/functions_for_test.h"
#include "mongo/client/embedded/libmongodbcapi.h"
#include <mongoc.h>
#include <set>

#include "mongo/stdx/memory.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/signal_handlers_synchronous.h"
namespace {

std::unique_ptr<mongo::unittest::TempDir> globalTempDir;

class MongodbEmbeddedTransportLayerTest : public mongo::unittest::Test {
protected:
    void setUp() {
        if (!globalTempDir) {
            globalTempDir = mongo::stdx::make_unique<mongo::unittest::TempDir>("embedded_mongo");
        }
        int argc = 4;
        const char* argv[] = {"mongo_embedded_transport_layer_test",
                              "--nounixsocket",
                              "--dbpath",
                              globalTempDir->path().c_str()};
        db_handle = libmongodbcapi_db_new(argc, argv, nullptr);

        mongoc_init();
        cd_client = embedded_mongoc_client_new(db_handle);
        mongoc_client_set_error_api(cd_client, 2);
        cd_db = mongoc_client_get_database(cd_client, "test");
        cd_collection = mongoc_database_get_collection(cd_db, (const char*)"things");
    }

    void tearDown() {
        mongoc_collection_drop(cd_collection, NULL);
        if (cd_collection) {
            mongoc_collection_destroy(cd_collection);
        }

        if (cd_db) {
            mongoc_database_destroy(cd_db);
        }

        if (cd_client) {
            mongoc_client_destroy(cd_client);
        }
        mongoc_cleanup();
        libmongodbcapi_db_destroy(db_handle);
    }

    libmongodbcapi_db* getDBHandle() {
        return db_handle;
    }

    mongoc_database_t* getDB() {
        return cd_db;
    }
    mongoc_client_t* getClient() {
        return cd_client;
    }
    mongoc_collection_t* getCollection() {
        return cd_collection;
    }


private:
    libmongodbcapi_db* db_handle;
    mongoc_database_t* cd_db;
    mongoc_client_t* cd_client;
    mongoc_collection_t* cd_collection;
};

TEST_F(MongodbEmbeddedTransportLayerTest, CreateAndDestroyDB) {
    // Test the setUp() and tearDown() test fixtures
}
TEST_F(MongodbEmbeddedTransportLayerTest, InsertAndExplain) {
    auto client = getClient();
    auto collection = getCollection();
    ASSERT(client);


    ASSERT(mongo::embeddedTest::insert_data(collection));

    ASSERT(mongo::embeddedTest::explain(collection));
}
TEST_F(MongodbEmbeddedTransportLayerTest, InsertAndCount) {
    auto client = getClient();
    auto collection = getCollection();
    ASSERT(client);
    ASSERT(collection);
    bson_error_t err;
    int64_t count;
    ASSERT(mongo::embeddedTest::insert_data(collection));
    count = mongoc_collection_count(collection, MONGOC_QUERY_NONE, nullptr, 0, 0, NULL, &err);
    ASSERT(count == 4);
}
TEST_F(MongodbEmbeddedTransportLayerTest, InsertAndDelete) {
    auto client = getClient();
    auto collection = getCollection();
    ASSERT(client);
    ASSERT(collection);
    bson_error_t err;
    bson_oid_t oid;
    int64_t count;
    // done with setup

    auto doc = bson_new();
    bson_oid_init(&oid, NULL);
    BSON_APPEND_OID(doc, "_id", &oid);
    BSON_APPEND_UTF8(doc, "hello", "world");
    ASSERT(mongoc_collection_insert(collection, MONGOC_INSERT_NONE, doc, NULL, &err));
    count = mongoc_collection_count(collection, MONGOC_QUERY_NONE, nullptr, 0, 0, NULL, &err);
    ASSERT(1 == count);
    bson_destroy(doc);
    doc = bson_new();
    BSON_APPEND_OID(doc, "_id", &oid);
    ASSERT(mongoc_collection_remove(collection, MONGOC_REMOVE_SINGLE_REMOVE, doc, NULL, &err));
    ASSERT(0 == mongoc_collection_count(collection, MONGOC_QUERY_NONE, nullptr, 0, 0, NULL, &err));
    bson_destroy(doc);
}
}  // namespace

// Define main function as an entry to these tests.
// These test functions cannot use the main() defined for unittests because they
// call runGlobalInitializers(). The embedded C API calls mongoDbMain() which
// calls runGlobalInitializers().
int main(int argc, char** argv, char** envp) {
    ::mongo::clearSignalMask();
    ::mongo::setupSynchronousSignalHandlers();
    auto result = ::mongo::unittest::Suite::run(std::vector<std::string>(), "", 1);
    globalTempDir.reset();
    mongo::quickExit(result);
}
