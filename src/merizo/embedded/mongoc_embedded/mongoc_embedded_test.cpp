/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#define MERIZO_LOG_DEFAULT_COMPONENT ::merizo::logger::LogComponent::kDefault

#include "merizo/platform/basic.h"

#include "merizoc_embedded/merizoc_embedded.h"

#include <set>

#include <merizoc/merizoc.h>
#include <yaml-cpp/yaml.h>

#include "merizo/db/server_options.h"
#include "merizo/embedded/merizo_embedded/merizo_embedded.h"
#include "merizo/embedded/merizoc_embedded/merizoc_embedded_test_gen.h"
#include "merizo/stdx/memory.h"
#include "merizo/unittest/temp_dir.h"
#include "merizo/unittest/unittest.h"
#include "merizo/util/log.h"
#include "merizo/util/options_parser/environment.h"
#include "merizo/util/options_parser/option_section.h"
#include "merizo/util/options_parser/options_parser.h"
#include "merizo/util/quick_exit.h"
#include "merizo/util/signal_handlers_synchronous.h"

namespace moe = merizo::optionenvironment;

merizo_embedded_v1_lib* global_lib_handle;

namespace {

std::unique_ptr<merizo::unittest::TempDir> globalTempDir;

/**
 * WARNING: This function is an example lifted directly from the C driver
 * for use testing the connection to the c driver. It is written in C,
 * and should not be used for anything besides basic testing.
 */
bool insert_data(merizoc_collection_t* collection) {
    merizoc_bulk_operation_t* bulk;
    const int ndocs = 4;
    bson_t* docs[ndocs];

    bulk = merizoc_collection_create_bulk_operation(collection, true, NULL);

    docs[0] = BCON_NEW("x", BCON_DOUBLE(1.0), "tags", "[", "dog", "cat", "]");
    docs[1] = BCON_NEW("x", BCON_DOUBLE(2.0), "tags", "[", "cat", "]");
    docs[2] = BCON_NEW("x", BCON_DOUBLE(2.0), "tags", "[", "mouse", "cat", "dog", "]");
    docs[3] = BCON_NEW("x", BCON_DOUBLE(3.0), "tags", "[", "]");

    for (int i = 0; i < ndocs; i++) {
        merizoc_bulk_operation_insert(bulk, docs[i]);
        bson_destroy(docs[i]);
        docs[i] = NULL;
    }

    bson_error_t error;
    bool ret = merizoc_bulk_operation_execute(bulk, NULL, &error);

    if (!ret) {
        ::merizo::log() << "Error inserting data: " << error.message;
    }

    merizoc_bulk_operation_destroy(bulk);
    return ret;
}

/**
 * WARNING: This function is an example lifted directly from the C driver
 * for use testing the connection to the c driver. It is written in C,
 * and should not be used for anything besides basic testing.
 */
bool explain(merizoc_collection_t* collection) {

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
    res = merizoc_collection_command_simple(collection, command, NULL, &reply, &error);
    if (!res) {
        ::merizo::log() << "Error with explain: " << error.message;
        goto explain_cleanup;
    }


explain_cleanup:
    bson_destroy(&reply);
    bson_destroy(command);
    return res;
}

class MerizodbEmbeddedTransportLayerTest : public merizo::unittest::Test {
protected:
    void setUp() {
        if (!globalTempDir) {
            globalTempDir = std::make_unique<merizo::unittest::TempDir>("embedded_merizo");
        }

        YAML::Emitter yaml;
        yaml << YAML::BeginMap;

        yaml << YAML::Key << "storage";
        yaml << YAML::Value << YAML::BeginMap;
        yaml << YAML::Key << "dbPath";
        yaml << YAML::Value << globalTempDir->path();
        yaml << YAML::EndMap;  // storage

        yaml << YAML::EndMap;

        db_handle = merizo_embedded_v1_instance_create(global_lib_handle, yaml.c_str(), nullptr);

        cd_client = merizoc_embedded_v1_client_create(db_handle);
        merizoc_client_set_error_api(cd_client, 2);
        cd_db = merizoc_client_get_database(cd_client, "test");
        cd_collection = merizoc_database_get_collection(cd_db, (const char*)"things");
    }

    void tearDown() {
        merizoc_collection_drop(cd_collection, nullptr);
        if (cd_collection) {
            merizoc_collection_destroy(cd_collection);
        }

        if (cd_db) {
            merizoc_database_destroy(cd_db);
        }

        if (cd_client) {
            merizoc_client_destroy(cd_client);
        }

        merizo_embedded_v1_instance_destroy(db_handle, nullptr);
    }

    merizo_embedded_v1_instance* getDBHandle() {
        return db_handle;
    }

    merizoc_database_t* getDB() {
        return cd_db;
    }
    merizoc_client_t* getClient() {
        return cd_client;
    }
    merizoc_collection_t* getCollection() {
        return cd_collection;
    }


private:
    merizo_embedded_v1_instance* db_handle;
    merizoc_database_t* cd_db;
    merizoc_client_t* cd_client;
    merizoc_collection_t* cd_collection;
};

TEST_F(MerizodbEmbeddedTransportLayerTest, CreateAndDestroyDB) {
    // Test the setUp() and tearDown() test fixtures
}
TEST_F(MerizodbEmbeddedTransportLayerTest, InsertAndExplain) {
    auto client = getClient();
    auto collection = getCollection();
    ASSERT(client);


    ASSERT(insert_data(collection));

    ASSERT(explain(collection));
}
TEST_F(MerizodbEmbeddedTransportLayerTest, InsertAndCount) {
    auto client = getClient();
    auto collection = getCollection();
    ASSERT(client);
    ASSERT(collection);
    bson_error_t err;
    int64_t count;
    ASSERT(insert_data(collection));
    count = merizoc_collection_count(collection, MERIZOC_QUERY_NONE, nullptr, 0, 0, NULL, &err);
    ASSERT(count == 4);
}
TEST_F(MerizodbEmbeddedTransportLayerTest, InsertAndDelete) {
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
    ASSERT(merizoc_collection_insert(collection, MERIZOC_INSERT_NONE, doc, NULL, &err));
    count = merizoc_collection_count(collection, MERIZOC_QUERY_NONE, nullptr, 0, 0, NULL, &err);
    ASSERT(1 == count);
    bson_destroy(doc);
    doc = bson_new();
    BSON_APPEND_OID(doc, "_id", &oid);
    ASSERT(merizoc_collection_remove(collection, MERIZOC_REMOVE_SINGLE_REMOVE, doc, NULL, &err));
    ASSERT(0 == merizoc_collection_count(collection, MERIZOC_QUERY_NONE, nullptr, 0, 0, NULL, &err));
    bson_destroy(doc);
}

struct StatusDestroy {
    void operator()(merizo_embedded_v1_status* const ptr) {
        if (!ptr) {
            merizo_embedded_v1_status_destroy(ptr);
        }
    }
};
using StatusPtr = std::unique_ptr<merizo_embedded_v1_status, StatusDestroy>;
}  // namespace

// Define main function as an entry to these tests.
// These test functions cannot use the main() defined for unittests because they
// call runGlobalInitializers(). The embedded C API calls merizoDbMain() which
// calls runGlobalInitializers().
int main(int argc, char** argv, char** envp) {

    moe::OptionsParser parser;
    moe::Environment environment;
    moe::OptionSection options;
    std::map<std::string, std::string> env;

    auto ret = merizo::embedded::addMerizocEmbeddedTestOptions(&options);
    if (!ret.isOK()) {
        std::cerr << ret << std::endl;
        return EXIT_FAILURE;
    }

    std::vector<std::string> argVector(argv, argv + argc);
    ret = parser.run(options, argVector, env, &environment);
    if (!ret.isOK()) {
        std::cerr << options.helpString();
        return EXIT_FAILURE;
    }
    if (environment.count("tempPath")) {
        ::merizo::unittest::TempDir::setTempPath(environment["tempPath"].as<std::string>());
    }

    ::merizo::clearSignalMask();
    ::merizo::setupSynchronousSignalHandlers();
    ::merizo::serverGlobalParams.noUnixSocket = true;
    ::merizo::unittest::setupTestLogger();

    StatusPtr status(merizo_embedded_v1_status_create());
    merizoc_init();

    merizo_embedded_v1_init_params params;
    params.log_flags = MERIZO_EMBEDDED_V1_LOG_STDOUT;
    params.log_callback = nullptr;
    params.log_user_data = nullptr;

    global_lib_handle = merizo_embedded_v1_lib_init(&params, status.get());
    if (global_lib_handle == nullptr) {
        std::cerr << "Error: " << merizo_embedded_v1_status_get_explanation(status.get());
        return EXIT_FAILURE;
    }

    auto result = ::merizo::unittest::Suite::run(std::vector<std::string>(), "", 1);

    if (merizo_embedded_v1_lib_fini(global_lib_handle, status.get()) != MERIZO_EMBEDDED_V1_SUCCESS) {
        std::cerr << "Error: " << merizo_embedded_v1_status_get_explanation(status.get());
        return EXIT_FAILURE;
    }

    merizoc_cleanup();
    globalTempDir.reset();
    merizo::quickExit(result);
}
