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


#include "mongo/platform/basic.h"

#include "mongoc_embedded/mongoc_embedded.h"

#include <memory>
#include <set>

#include <mongoc/mongoc.h>
#include <yaml-cpp/yaml.h>

#include "mongo/base/initializer.h"
#include "mongo/db/server_options.h"
#include "mongo/embedded/mongo_embedded/mongo_embedded.h"
#include "mongo/embedded/mongoc_embedded/mongoc_embedded_test_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/signal_handlers_synchronous.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace moe = mongo::optionenvironment;

mongo_embedded_v1_lib* global_lib_handle;

namespace mongo {
namespace {

std::unique_ptr<mongo::unittest::TempDir> globalTempDir;

/**
 * WARNING: This function is an example lifted directly from the C driver
 * for use testing the connection to the c driver. It is written in C,
 * and should not be used for anything besides basic testing.
 */
bool insert_data(mongoc_collection_t* collection) {
    mongoc_bulk_operation_t* bulk;
    const int ndocs = 4;
    bson_t* docs[ndocs];

    bulk = mongoc_collection_create_bulk_operation(collection, true, NULL);

    docs[0] = BCON_NEW("x", BCON_DOUBLE(1.0), "tags", "[", "dog", "cat", "]");
    docs[1] = BCON_NEW("x", BCON_DOUBLE(2.0), "tags", "[", "cat", "]");
    docs[2] = BCON_NEW("x", BCON_DOUBLE(2.0), "tags", "[", "mouse", "cat", "dog", "]");
    docs[3] = BCON_NEW("x", BCON_DOUBLE(3.0), "tags", "[", "]");

    for (int i = 0; i < ndocs; i++) {
        mongoc_bulk_operation_insert(bulk, docs[i]);
        bson_destroy(docs[i]);
        docs[i] = NULL;
    }

    bson_error_t error;
    bool ret = mongoc_bulk_operation_execute(bulk, NULL, &error);

    if (!ret) {
        LOGV2(22556, "Error inserting data: {error_message}", "error_message"_attr = error.message);
    }

    mongoc_bulk_operation_destroy(bulk);
    return ret;
}

/**
 * WARNING: This function is an example lifted directly from the C driver
 * for use testing the connection to the c driver. It is written in C,
 * and should not be used for anything besides basic testing.
 */
bool explain(mongoc_collection_t* collection) {

    bson_t* command;
    bson_t reply;
    bson_error_t error;
    bool res;

    command = BCON_NEW("explain",
                       "{",
                       "find",
                       BCON_UTF8((const char*)"things"),
                       "filter",
                       "{",
                       "x",
                       BCON_INT32(1),
                       "}",
                       "}");
    res = mongoc_collection_command_simple(collection, command, NULL, &reply, &error);
    if (!res) {
        LOGV2(22557, "Error with explain: {error_message}", "error_message"_attr = error.message);
        goto explain_cleanup;
    }


explain_cleanup:
    bson_destroy(&reply);
    bson_destroy(command);
    return res;
}

class MongodbEmbeddedTransportLayerTest : public mongo::unittest::Test {
protected:
    void setUp() {
        if (!globalTempDir) {
            globalTempDir = std::make_unique<mongo::unittest::TempDir>("embedded_mongo");
        }

        YAML::Emitter yaml;
        yaml << YAML::BeginMap;

        yaml << YAML::Key << "storage";
        yaml << YAML::Value << YAML::BeginMap;
        yaml << YAML::Key << "dbPath";
        yaml << YAML::Value << globalTempDir->path();
        yaml << YAML::EndMap;  // storage

        yaml << YAML::EndMap;

        db_handle = mongo_embedded_v1_instance_create(global_lib_handle, yaml.c_str(), nullptr);

        cd_client = mongoc_embedded_v1_client_create(db_handle);
        mongoc_client_set_error_api(cd_client, 2);
        cd_db = mongoc_client_get_database(cd_client, "test");
        cd_collection = mongoc_database_get_collection(cd_db, (const char*)"things");
    }

    void tearDown() {
        mongoc_collection_drop(cd_collection, nullptr);
        if (cd_collection) {
            mongoc_collection_destroy(cd_collection);
        }

        if (cd_db) {
            mongoc_database_destroy(cd_db);
        }

        if (cd_client) {
            mongoc_client_destroy(cd_client);
        }

        mongo_embedded_v1_instance_destroy(db_handle, nullptr);
    }

    mongo_embedded_v1_instance* getDBHandle() {
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
    mongo_embedded_v1_instance* db_handle;
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


    ASSERT(insert_data(collection));

    ASSERT(explain(collection));
}
TEST_F(MongodbEmbeddedTransportLayerTest, InsertAndCount) {
    auto client = getClient();
    auto collection = getCollection();
    ASSERT(client);
    ASSERT(collection);
    bson_error_t err;
    int64_t count;
    ASSERT(insert_data(collection));
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

struct StatusDestroy {
    void operator()(mongo_embedded_v1_status* const ptr) {
        if (!ptr) {
            mongo_embedded_v1_status_destroy(ptr);
        }
    }
};
using StatusPtr = std::unique_ptr<mongo_embedded_v1_status, StatusDestroy>;
}  // namespace
}  // namespace mongo

// Define main function as an entry to these tests.
// These test functions cannot use the main() defined for unittests because they
// call runGlobalInitializers(). The embedded C API calls mongoDbMain() which
// calls runGlobalInitializers().
int main(int argc, char** argv) {

    moe::OptionsParser parser;
    moe::Environment environment;
    moe::OptionSection options;

    auto ret = mongo::embedded::addMongocEmbeddedTestOptions(&options);
    if (!ret.isOK()) {
        std::cerr << ret << std::endl;
        return static_cast<int>(mongo::ExitCode::fail);
    }

    ret = parser.run(options, std::vector<std::string>(argv, argv + argc), &environment);
    if (!ret.isOK()) {
        std::cerr << options.helpString();
        return static_cast<int>(mongo::ExitCode::fail);
    }
    if (environment.count("tempPath")) {
        ::mongo::unittest::TempDir::setTempPath(environment["tempPath"].as<std::string>());
    }

    ::mongo::clearSignalMask();
    ::mongo::setupSynchronousSignalHandlers();
    ::mongo::serverGlobalParams.noUnixSocket = true;

    // See comment by the same code block in mongo_embedded_test.cpp
    ret = mongo::runGlobalInitializers(std::vector<std::string>{});
    if (!ret.isOK()) {
        std::cerr << "Global initilization failed";
        return static_cast<int>(mongo::ExitCode::fail);
    }

    ret = mongo::runGlobalDeinitializers();
    if (!ret.isOK()) {
        std::cerr << "Global deinitilization failed";
        return static_cast<int>(mongo::ExitCode::fail);
    }

    mongo::StatusPtr status(mongo_embedded_v1_status_create());
    mongoc_init();

    mongo_embedded_v1_init_params params;
    params.log_flags = MONGO_EMBEDDED_V1_LOG_STDOUT;
    params.log_callback = nullptr;
    params.log_user_data = nullptr;

    global_lib_handle = mongo_embedded_v1_lib_init(&params, status.get());
    if (global_lib_handle == nullptr) {
        std::cerr << "Error: " << mongo_embedded_v1_status_get_explanation(status.get());
        return static_cast<int>(mongo::ExitCode::fail);
    }

    auto result = ::mongo::unittest::Suite::run(std::vector<std::string>(), "", "", 1);

    if (mongo_embedded_v1_lib_fini(global_lib_handle, status.get()) != MONGO_EMBEDDED_V1_SUCCESS) {
        std::cerr << "Error: " << mongo_embedded_v1_status_get_explanation(status.get());
        return static_cast<int>(mongo::ExitCode::fail);
    }

    mongoc_cleanup();
    mongo::globalTempDir.reset();
    mongo::quickExit(result);
}
