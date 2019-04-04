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


#include "merizo_embedded/merizo_embedded.h"

#include <memory>
#include <set>
#include <yaml-cpp/yaml.h>

#include "merizo/bson/bsonobjbuilder.h"
#include "merizo/db/commands/test_commands_enabled.h"
#include "merizo/db/json.h"
#include "merizo/db/server_options.h"
#include "merizo/embedded/merizo_embedded/merizo_embedded_test_gen.h"
#include "merizo/rpc/message.h"
#include "merizo/rpc/op_msg.h"
#include "merizo/stdx/thread.h"
#include "merizo/unittest/temp_dir.h"
#include "merizo/unittest/unittest.h"
#include "merizo/util/options_parser/environment.h"
#include "merizo/util/options_parser/option_section.h"
#include "merizo/util/options_parser/options_parser.h"
#include "merizo/util/quick_exit.h"
#include "merizo/util/shared_buffer.h"
#include "merizo/util/signal_handlers_synchronous.h"

namespace moe = merizo::optionenvironment;

merizo_embedded_v1_lib* global_lib_handle;

namespace {

std::unique_ptr<merizo::unittest::TempDir> globalTempDir;

struct StatusDestructor {
    void operator()(merizo_embedded_v1_status* const p) const noexcept {
        if (p)
            merizo_embedded_v1_status_destroy(p);
    }
};

using CapiStatusPtr = std::unique_ptr<merizo_embedded_v1_status, StatusDestructor>;

CapiStatusPtr makeStatusPtr() {
    return CapiStatusPtr{merizo_embedded_v1_status_create()};
}

struct ClientDestructor {
    void operator()(merizo_embedded_v1_client* const p) const noexcept {
        if (!p)
            return;

        auto status = makeStatusPtr();
        if (merizo_embedded_v1_client_destroy(p, status.get()) != MERIZO_EMBEDDED_V1_SUCCESS) {
            std::cerr << "libmerizodb_capi_client_destroy failed." << std::endl;
            if (status) {
                std::cerr << "Error code: " << merizo_embedded_v1_status_get_error(status.get())
                          << std::endl;
                std::cerr << "Error message: "
                          << merizo_embedded_v1_status_get_explanation(status.get()) << std::endl;
            }
        }
    }
};

using MerizoDBCAPIClientPtr = std::unique_ptr<merizo_embedded_v1_client, ClientDestructor>;

class MerizodbCAPITest : public merizo::unittest::Test {
protected:
    void setUp() {
        status = merizo_embedded_v1_status_create();
        ASSERT(status != nullptr);

        if (!globalTempDir) {
            globalTempDir = std::make_unique<merizo::unittest::TempDir>("embedded_merizo");
        }

        merizo_embedded_v1_init_params params;
        params.log_flags = MERIZO_EMBEDDED_V1_LOG_STDOUT;
        params.log_callback = nullptr;
        params.log_user_data = nullptr;

        YAML::Emitter yaml;
        yaml << YAML::BeginMap;

        yaml << YAML::Key << "storage";
        yaml << YAML::Value << YAML::BeginMap;
        yaml << YAML::Key << "dbPath";
        yaml << YAML::Value << globalTempDir->path();
        yaml << YAML::EndMap;  // storage

        yaml << YAML::EndMap;

        params.yaml_config = yaml.c_str();

        lib = merizo_embedded_v1_lib_init(&params, status);
        ASSERT(lib != nullptr) << merizo_embedded_v1_status_get_explanation(status);

        db = merizo_embedded_v1_instance_create(lib, yaml.c_str(), status);
        ASSERT(db != nullptr) << merizo_embedded_v1_status_get_explanation(status);
    }

    void tearDown() {
        ASSERT_EQUALS(merizo_embedded_v1_instance_destroy(db, status), MERIZO_EMBEDDED_V1_SUCCESS)
            << merizo_embedded_v1_status_get_explanation(status);
        ASSERT_EQUALS(merizo_embedded_v1_lib_fini(lib, status), MERIZO_EMBEDDED_V1_SUCCESS)
            << merizo_embedded_v1_status_get_explanation(status);
        merizo_embedded_v1_status_destroy(status);
    }

    merizo_embedded_v1_instance* getDB() const {
        return db;
    }

    MerizoDBCAPIClientPtr createClient() const {
        MerizoDBCAPIClientPtr client(merizo_embedded_v1_client_create(db, status));
        ASSERT(client.get() != nullptr) << merizo_embedded_v1_status_get_explanation(status);
        return client;
    }

    merizo::Message messageFromBuffer(void* data, size_t dataLen) {
        auto sb = merizo::SharedBuffer::allocate(dataLen);
        memcpy(sb.get(), data, dataLen);
        merizo::Message msg(std::move(sb));
        return msg;
    }

    merizo::BSONObj performRpc(MerizoDBCAPIClientPtr& client, merizo::OpMsgRequest request) {
        auto inputMessage = request.serialize();

        // declare the output size and pointer
        void* output;
        size_t outputSize;

        // call the wire protocol
        int err = merizo_embedded_v1_client_invoke(
            client.get(), inputMessage.buf(), inputMessage.size(), &output, &outputSize, status);
        ASSERT_EQUALS(err, MERIZO_EMBEDDED_V1_SUCCESS);

        // convert the shared buffer to a merizo::message and ensure that it is valid
        auto outputMessage = messageFromBuffer(output, outputSize);
        ASSERT(outputMessage.size() > 0);
        ASSERT(outputMessage.operation() == inputMessage.operation());

        // convert the message into an OpMessage to examine its BSON
        auto outputOpMsg = merizo::OpMsg::parseOwned(outputMessage);
        ASSERT(outputOpMsg.body.valid(merizo::BSONVersion::kLatest));
        return outputOpMsg.body;
    }


protected:
    merizo_embedded_v1_lib* lib;
    merizo_embedded_v1_instance* db;
    merizo_embedded_v1_status* status;
};

TEST_F(MerizodbCAPITest, CreateAndDestroyDB) {
    // Test the setUp() and tearDown() test fixtures
}

TEST_F(MerizodbCAPITest, CreateAndDestroyDBAndClient) {
    auto client = createClient();
}

// This test is to make sure that destroying the db will fail if there's remaining clients left.
TEST_F(MerizodbCAPITest, DoNotDestroyClient) {
    auto client = createClient();
    ASSERT(merizo_embedded_v1_instance_destroy(getDB(), nullptr) != MERIZO_EMBEDDED_V1_SUCCESS);
}

TEST_F(MerizodbCAPITest, CreateMultipleClients) {
    const int numClients = 10;
    std::set<MerizoDBCAPIClientPtr> clients;
    for (int i = 0; i < numClients; i++) {
        clients.insert(createClient());
    }

    // ensure that each client is unique by making sure that the set size equals the number of
    // clients instantiated
    ASSERT_EQUALS(static_cast<int>(clients.size()), numClients);
}

TEST_F(MerizodbCAPITest, IsMaster) {
    // create the client object
    auto client = createClient();

    // craft the isMaster message
    merizo::BSONObj inputObj = merizo::fromjson("{isMaster: 1}");
    auto inputOpMsg = merizo::OpMsgRequest::fromDBAndBody("admin", inputObj);
    auto output = performRpc(client, inputOpMsg);
    ASSERT(output.getBoolField("ismaster"));
}

TEST_F(MerizodbCAPITest, CreateIndex) {
    // create the client object
    auto client = createClient();

    // craft the createIndexes message
    merizo::BSONObj inputObj = merizo::fromjson(
        R"raw_delimiter({
            createIndexes: 'items',
            indexes: 
            [
                {
                    key: {
                        task: 1
                    },
                    name: 'task_1'
                }
            ]
        })raw_delimiter");
    auto inputOpMsg = merizo::OpMsgRequest::fromDBAndBody("index_db", inputObj);
    auto output = performRpc(client, inputOpMsg);

    ASSERT(output.hasField("ok")) << output;
    ASSERT(output.getField("ok").numberDouble() == 1.0) << output;
    ASSERT(output.getIntField("numIndexesAfter") == output.getIntField("numIndexesBefore") + 1)
        << output;
}

TEST_F(MerizodbCAPITest, CreateBackgroundIndex) {
    // create the client object
    auto client = createClient();

    // craft the createIndexes message
    merizo::BSONObj inputObj = merizo::fromjson(
        R"raw_delimiter({
            createIndexes: 'items',
            indexes: 
            [
                {
                    key: {
                        task: 1
                    },
                    name: 'task_1',
                    background: true
                }
            ]
        })raw_delimiter");
    auto inputOpMsg = merizo::OpMsgRequest::fromDBAndBody("background_index_db", inputObj);
    auto output = performRpc(client, inputOpMsg);

    ASSERT(output.hasField("ok")) << output;
    ASSERT(output.getField("ok").numberDouble() != 1.0) << output;
}

TEST_F(MerizodbCAPITest, CreateTTLIndex) {
    // create the client object
    auto client = createClient();

    // craft the createIndexes message
    merizo::BSONObj inputObj = merizo::fromjson(
        R"raw_delimiter({
            createIndexes: 'items',
            indexes: 
            [
                {
                    key: {
                        task: 1
                    },
                    name: 'task_ttl',
                    expireAfterSeconds: 36000
                }
            ]
        })raw_delimiter");
    auto inputOpMsg = merizo::OpMsgRequest::fromDBAndBody("ttl_index_db", inputObj);
    auto output = performRpc(client, inputOpMsg);

    ASSERT(output.hasField("ok")) << output;
    ASSERT(output.getField("ok").numberDouble() != 1.0) << output;
}

TEST_F(MerizodbCAPITest, TrimMemory) {
    // create the client object
    auto client = createClient();

    // craft the isMaster message
    merizo::BSONObj inputObj = merizo::fromjson("{trimMemory: 'aggressive'}");
    auto inputOpMsg = merizo::OpMsgRequest::fromDBAndBody("admin", inputObj);
    performRpc(client, inputOpMsg);
}

TEST_F(MerizodbCAPITest, BatteryLevel) {
    // create the client object
    auto client = createClient();

    // craft the isMaster message
    merizo::BSONObj inputObj = merizo::fromjson("{setBatteryLevel: 'low'}");
    auto inputOpMsg = merizo::OpMsgRequest::fromDBAndBody("admin", inputObj);
    performRpc(client, inputOpMsg);
}


TEST_F(MerizodbCAPITest, InsertDocument) {
    auto client = createClient();

    merizo::BSONObj insertObj = merizo::fromjson(
        "{insert: 'collection_name', documents: [{firstName: 'Merizo', lastName: 'DB', age: 10}]}");
    auto insertOpMsg = merizo::OpMsgRequest::fromDBAndBody("db_name", insertObj);
    auto outputBSON = performRpc(client, insertOpMsg);
    ASSERT(outputBSON.hasField("n"));
    ASSERT(outputBSON.getIntField("n") == 1);
    ASSERT(outputBSON.hasField("ok"));
    ASSERT(outputBSON.getField("ok").numberDouble() == 1.0);
}

TEST_F(MerizodbCAPITest, InsertMultipleDocuments) {
    auto client = createClient();

    merizo::BSONObj insertObj = merizo::fromjson(
        "{insert: 'collection_name', documents: [{firstName: 'doc1FirstName', lastName: "
        "'doc1LastName', age: 30}, {firstName: 'doc2FirstName', lastName: 'doc2LastName', age: "
        "20}]}");
    auto insertOpMsg = merizo::OpMsgRequest::fromDBAndBody("db_name", insertObj);
    auto outputBSON = performRpc(client, insertOpMsg);
    ASSERT(outputBSON.hasField("n"));
    ASSERT(outputBSON.getIntField("n") == 2);
    ASSERT(outputBSON.hasField("ok"));
    ASSERT(outputBSON.getField("ok").numberDouble() == 1.0);
}

TEST_F(MerizodbCAPITest, KillOp) {
    auto client = createClient();

    merizo::stdx::thread killOpThread([this]() {
        auto client = createClient();

        merizo::BSONObj currentOpObj = merizo::fromjson("{currentOp: 1}");
        auto currentOpMsg = merizo::OpMsgRequest::fromDBAndBody("admin", currentOpObj);
        merizo::BSONObj outputBSON;

        // Wait for the sleep command to start in the main test thread.
        int opid = -1;
        do {
            outputBSON = performRpc(client, currentOpMsg);
            auto inprog = outputBSON.getObjectField("inprog");

            // See if we find the sleep command among the running commands
            for (const auto& elt : inprog) {
                auto inprogObj = inprog.getObjectField(elt.fieldNameStringData());
                std::string ns = inprogObj.getStringField("ns");
                if (ns == "admin.$cmd") {
                    opid = inprogObj.getIntField("opid");
                    break;
                }
            }
        } while (opid == -1);

        // Sleep command found, kill it.
        std::stringstream ss;
        ss << "{'killOp': 1, 'op': " << opid << "}";
        merizo::BSONObj killOpObj = merizo::fromjson(ss.str());
        auto killOpMsg = merizo::OpMsgRequest::fromDBAndBody("admin", killOpObj);
        outputBSON = performRpc(client, killOpMsg);

        ASSERT(outputBSON.hasField("ok"));
        ASSERT(outputBSON.getField("ok").numberDouble() == 1.0);
    });

    merizo::BSONObj sleepObj = merizo::fromjson("{'sleep': {'secs': 1000}}");
    auto sleepOpMsg = merizo::OpMsgRequest::fromDBAndBody("admin", sleepObj);
    auto outputBSON = performRpc(client, sleepOpMsg);

    ASSERT(outputBSON.hasField("ok"));
    ASSERT(outputBSON.getField("ok").numberDouble() != 1.0);
    ASSERT(outputBSON.getIntField("code") == merizo::ErrorCodes::Interrupted);

    killOpThread.join();
}

TEST_F(MerizodbCAPITest, ReadDB) {
    auto client = createClient();

    merizo::BSONObj findObj = merizo::fromjson("{find: 'collection_name', limit: 2}");
    auto findMsg = merizo::OpMsgRequest::fromDBAndBody("db_name", findObj);
    auto outputBSON = performRpc(client, findMsg);


    ASSERT(outputBSON.valid(merizo::BSONVersion::kLatest));
    ASSERT(outputBSON.hasField("cursor"));
    ASSERT(outputBSON.getField("cursor").embeddedObject().hasField("firstBatch"));
    merizo::BSONObj arrObj =
        outputBSON.getField("cursor").embeddedObject().getField("firstBatch").embeddedObject();
    ASSERT(arrObj.couldBeArray());

    merizo::BSONObjIterator i(arrObj);
    int index = 0;
    while (i.moreWithEOO()) {
        merizo::BSONElement e = i.next();
        if (e.eoo())
            break;
        index++;
    }
    ASSERT(index == 2);
}

TEST_F(MerizodbCAPITest, InsertAndRead) {
    auto client = createClient();

    merizo::BSONObj insertObj = merizo::fromjson(
        "{insert: 'collection_name', documents: [{firstName: 'Merizo', lastName: 'DB', age: 10}]}");
    auto insertOpMsg = merizo::OpMsgRequest::fromDBAndBody("db_name", insertObj);
    auto outputBSON1 = performRpc(client, insertOpMsg);
    ASSERT(outputBSON1.valid(merizo::BSONVersion::kLatest));
    ASSERT(outputBSON1.hasField("n"));
    ASSERT(outputBSON1.getIntField("n") == 1);
    ASSERT(outputBSON1.hasField("ok"));
    ASSERT(outputBSON1.getField("ok").numberDouble() == 1.0);

    merizo::BSONObj findObj = merizo::fromjson("{find: 'collection_name', limit: 1}");
    auto findMsg = merizo::OpMsgRequest::fromDBAndBody("db_name", findObj);
    auto outputBSON2 = performRpc(client, findMsg);
    ASSERT(outputBSON2.valid(merizo::BSONVersion::kLatest));
    ASSERT(outputBSON2.hasField("cursor"));
    ASSERT(outputBSON2.getField("cursor").embeddedObject().hasField("firstBatch"));
    merizo::BSONObj arrObj =
        outputBSON2.getField("cursor").embeddedObject().getField("firstBatch").embeddedObject();
    ASSERT(arrObj.couldBeArray());

    merizo::BSONObjIterator i(arrObj);
    int index = 0;
    while (i.moreWithEOO()) {
        merizo::BSONElement e = i.next();
        if (e.eoo())
            break;
        index++;
    }
    ASSERT(index == 1);
}

TEST_F(MerizodbCAPITest, InsertAndReadDifferentClients) {
    auto client1 = createClient();
    auto client2 = createClient();

    merizo::BSONObj insertObj = merizo::fromjson(
        "{insert: 'collection_name', documents: [{firstName: 'Merizo', lastName: 'DB', age: 10}]}");
    auto insertOpMsg = merizo::OpMsgRequest::fromDBAndBody("db_name", insertObj);
    auto outputBSON1 = performRpc(client1, insertOpMsg);
    ASSERT(outputBSON1.valid(merizo::BSONVersion::kLatest));
    ASSERT(outputBSON1.hasField("n"));
    ASSERT(outputBSON1.getIntField("n") == 1);
    ASSERT(outputBSON1.hasField("ok"));
    ASSERT(outputBSON1.getField("ok").numberDouble() == 1.0);

    merizo::BSONObj findObj = merizo::fromjson("{find: 'collection_name', limit: 1}");
    auto findMsg = merizo::OpMsgRequest::fromDBAndBody("db_name", findObj);
    auto outputBSON2 = performRpc(client2, findMsg);
    ASSERT(outputBSON2.valid(merizo::BSONVersion::kLatest));
    ASSERT(outputBSON2.hasField("cursor"));
    ASSERT(outputBSON2.getField("cursor").embeddedObject().hasField("firstBatch"));
    merizo::BSONObj arrObj =
        outputBSON2.getField("cursor").embeddedObject().getField("firstBatch").embeddedObject();
    ASSERT(arrObj.couldBeArray());

    merizo::BSONObjIterator i(arrObj);
    int index = 0;
    while (i.moreWithEOO()) {
        merizo::BSONElement e = i.next();
        if (e.eoo())
            break;
        index++;
    }
    ASSERT(index == 1);
}

TEST_F(MerizodbCAPITest, InsertAndDelete) {
    auto client = createClient();
    merizo::BSONObj insertObj = merizo::fromjson(
        "{insert: 'collection_name', documents: [{firstName: 'toDelete', lastName: 'notImportant', "
        "age: 10}]}");
    auto insertOpMsg = merizo::OpMsgRequest::fromDBAndBody("db_name", insertObj);
    auto outputBSON1 = performRpc(client, insertOpMsg);
    ASSERT(outputBSON1.valid(merizo::BSONVersion::kLatest));
    ASSERT(outputBSON1.hasField("n"));
    ASSERT(outputBSON1.getIntField("n") == 1);
    ASSERT(outputBSON1.hasField("ok"));
    ASSERT(outputBSON1.getField("ok").numberDouble() == 1.0);


    // Delete
    merizo::BSONObj deleteObj = merizo::fromjson(
        "{delete: 'collection_name', deletes:   [{q: {firstName: 'toDelete', age: 10}, limit: "
        "1}]}");
    auto deleteOpMsg = merizo::OpMsgRequest::fromDBAndBody("db_name", deleteObj);
    auto outputBSON2 = performRpc(client, deleteOpMsg);
    ASSERT(outputBSON2.valid(merizo::BSONVersion::kLatest));
    ASSERT(outputBSON2.hasField("n"));
    ASSERT(outputBSON2.getIntField("n") == 1);
    ASSERT(outputBSON2.hasField("ok"));
    ASSERT(outputBSON2.getField("ok").numberDouble() == 1.0);
}


TEST_F(MerizodbCAPITest, InsertAndUpdate) {
    auto client = createClient();

    merizo::BSONObj insertObj = merizo::fromjson(
        "{insert: 'collection_name', documents: [{firstName: 'toUpdate', lastName: 'notImportant', "
        "age: 10}]}");
    auto insertOpMsg = merizo::OpMsgRequest::fromDBAndBody("db_name", insertObj);
    auto outputBSON1 = performRpc(client, insertOpMsg);
    ASSERT(outputBSON1.valid(merizo::BSONVersion::kLatest));
    ASSERT(outputBSON1.hasField("n"));
    ASSERT(outputBSON1.getIntField("n") == 1);
    ASSERT(outputBSON1.hasField("ok"));
    ASSERT(outputBSON1.getField("ok").numberDouble() == 1.0);


    // Update
    merizo::BSONObj updateObj = merizo::fromjson(
        "{update: 'collection_name', updates: [ {q: {firstName: 'toUpdate', age: 10}, u: {'$inc': "
        "{age: 5}}}]}");
    auto updateOpMsg = merizo::OpMsgRequest::fromDBAndBody("db_name", updateObj);
    auto outputBSON2 = performRpc(client, updateOpMsg);
    ASSERT(outputBSON2.valid(merizo::BSONVersion::kLatest));
    ASSERT(outputBSON2.hasField("ok"));
    ASSERT(outputBSON2.getField("ok").numberDouble() == 1.0);
    ASSERT(outputBSON2.hasField("nModified"));
    ASSERT(outputBSON2.getIntField("nModified") == 1);
}

TEST_F(MerizodbCAPITest, RunListCommands) {
    auto client = createClient();

    std::vector<std::string> whitelist = {
        "_hashBSONElement",
        "aggregate",
        "buildInfo",
        "collMod",
        "collStats",
        "configureFailPoint",
        "count",
        "create",
        "createIndexes",
        "currentOp",
        "dataSize",
        "dbStats",
        "delete",
        "distinct",
        "drop",
        "dropDatabase",
        "dropIndexes",
        "echo",
        "endSessions",
        "explain",
        "find",
        "findAndModify",
        "getLastError",
        "getMore",
        "getParameter",
        "httpClientRequest",
        "insert",
        "isMaster",
        "killCursors",
        "killOp",
        "killSessions",
        "killAllSessions",
        "killAllSessionsByPattern",
        "listCollections",
        "listCommands",
        "listDatabases",
        "listIndexes",
        "lockInfo",
        "ping",
        "planCacheClear",
        "planCacheClearFilters",
        "planCacheListFilters",
        "planCacheListPlans",
        "planCacheListQueryShapes",
        "planCacheSetFilter",
        "reIndex",
        "refreshLogicalSessionCacheNow",
        "refreshSessions",
        "renameCollection",
        "repairCursor",
        "repairDatabase",
        "resetError",
        "serverStatus",
        "setBatteryLevel",
        "setParameter",
        "sleep",
        "startSession",
        "trimMemory",
        "twoPhaseCreateIndexes",
        "update",
        "validate",
    };
    std::sort(whitelist.begin(), whitelist.end());

    merizo::BSONObj listCommandsObj = merizo::fromjson("{ listCommands: 1 }");
    auto listCommandsOpMsg = merizo::OpMsgRequest::fromDBAndBody("db_name", listCommandsObj);
    auto output = performRpc(client, listCommandsOpMsg);
    auto commandsBSON = output["commands"];
    std::vector<std::string> commands;
    for (const auto& element : commandsBSON.Obj()) {
        commands.push_back(element.fieldNameStringData().toString());
    }
    std::sort(commands.begin(), commands.end());

    std::vector<std::string> missing;
    std::vector<std::string> unsupported;
    std::set_difference(whitelist.begin(),
                        whitelist.end(),
                        commands.begin(),
                        commands.end(),
                        std::back_inserter(missing));
    std::set_difference(commands.begin(),
                        commands.end(),
                        whitelist.begin(),
                        whitelist.end(),
                        std::back_inserter(unsupported));

    if (!missing.empty()) {
        std::cout << "\nMissing commands from the embedded binary:\n";
    }
    for (auto&& cmd : missing) {
        std::cout << cmd << "\n";
    }
    if (!unsupported.empty()) {
        std::cout << "\nUnsupported commands in the embedded binary:\n";
    }
    for (auto&& cmd : unsupported) {
        std::cout << cmd << "\n";
    }

    ASSERT(missing.empty());
    ASSERT(unsupported.empty());
}

// This test is temporary to make sure that only one database can be created
// This restriction may be relaxed at a later time
TEST_F(MerizodbCAPITest, CreateMultipleDBs) {
    auto status = makeStatusPtr();
    ASSERT(status.get());
    merizo_embedded_v1_instance* db2 = merizo_embedded_v1_instance_create(lib, nullptr, status.get());
    ASSERT(db2 == nullptr);
    ASSERT_EQUALS(merizo_embedded_v1_status_get_error(status.get()),
                  MERIZO_EMBEDDED_V1_ERROR_DB_MAX_OPEN);
}
}  // namespace

// Define main function as an entry to these tests.
// These test functions cannot use the main() defined for unittests because they
// call runGlobalInitializers(). The embedded C API calls merizoDbMain() which
// calls runGlobalInitializers().
int main(const int argc, const char* const* const argv) {
    moe::Environment environment;
    moe::OptionSection options;

    auto ret = merizo::embedded::addMerizoEmbeddedTestOptions(&options);
    if (!ret.isOK()) {
        std::cerr << ret << std::endl;
        return EXIT_FAILURE;
    }

    std::map<std::string, std::string> env;
    ret = moe::OptionsParser().run(
        options, std::vector<std::string>(argv, argv + argc), env, &environment);
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

    // Allocate an error descriptor for use in non-configured tests
    const auto status = makeStatusPtr();

    merizo::setTestCommandsEnabled(true);

    // Check so we can initialize the library without providing init params
    merizo_embedded_v1_lib* lib = merizo_embedded_v1_lib_init(nullptr, status.get());
    if (lib == nullptr) {
        std::cerr << "merizo_embedded_v1_init() failed with "
                  << merizo_embedded_v1_status_get_error(status.get()) << ": "
                  << merizo_embedded_v1_status_get_explanation(status.get()) << std::endl;
        return EXIT_FAILURE;
    }

    if (merizo_embedded_v1_lib_fini(lib, status.get()) != MERIZO_EMBEDDED_V1_SUCCESS) {
        std::cerr << "merizo_embedded_v1_fini() failed with "
                  << merizo_embedded_v1_status_get_error(status.get()) << ": "
                  << merizo_embedded_v1_status_get_explanation(status.get()) << std::endl;
        return EXIT_FAILURE;
    }

    // Initialize the library with a log callback and test so we receive at least one callback
    // during the lifetime of the test
    merizo_embedded_v1_init_params params{};

    bool receivedCallback = false;
    params.log_flags = MERIZO_EMBEDDED_V1_LOG_STDOUT | MERIZO_EMBEDDED_V1_LOG_CALLBACK;
    params.log_callback = [](void* user_data,
                             const char* message,
                             const char* component,
                             const char* context,
                             int severety) {
        ASSERT(message);
        ASSERT(component);
        *reinterpret_cast<bool*>(user_data) = true;
    };
    params.log_user_data = &receivedCallback;

    lib = merizo_embedded_v1_lib_init(&params, nullptr);
    if (lib == nullptr) {
        std::cerr << "merizo_embedded_v1_init() failed with "
                  << merizo_embedded_v1_status_get_error(status.get()) << ": "
                  << merizo_embedded_v1_status_get_explanation(status.get()) << std::endl;
    }

    if (merizo_embedded_v1_lib_fini(lib, nullptr) != MERIZO_EMBEDDED_V1_SUCCESS) {
        std::cerr << "merizo_embedded_v1_fini() failed with "
                  << merizo_embedded_v1_status_get_error(status.get()) << ": "
                  << merizo_embedded_v1_status_get_explanation(status.get()) << std::endl;
    }

    if (!receivedCallback) {
        std::cerr << "Did not get a log callback." << std::endl;
    }

    const auto result = ::merizo::unittest::Suite::run(std::vector<std::string>(), "", 1);

    globalTempDir.reset();
    merizo::quickExit(result);
}
