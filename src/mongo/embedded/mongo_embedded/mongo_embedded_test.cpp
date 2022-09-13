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


#include "mongo_embedded/mongo_embedded.h"

#include <memory>
#include <set>
#include <yaml-cpp/yaml.h>

#include "mongo/base/initializer.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/json.h"
#include "mongo/db/server_options.h"
#include "mongo/embedded/mongo_embedded/mongo_embedded_test_gen.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/shared_buffer.h"
#include "mongo/util/signal_handlers_synchronous.h"
#include "mongo/util/text.h"

namespace moe = mongo::optionenvironment;

mongo_embedded_v1_lib* global_lib_handle;

namespace {


auto& getGlobalTempDir() {
    static std::unique_ptr<mongo::unittest::TempDir> globalTempDir;
    if (!globalTempDir) {
        globalTempDir = std::make_unique<mongo::unittest::TempDir>("embedded_mongo");
    }
    return globalTempDir;
}

struct StatusDestructor {
    void operator()(mongo_embedded_v1_status* const p) const noexcept {
        if (p)
            mongo_embedded_v1_status_destroy(p);
    }
};

using CapiStatusPtr = std::unique_ptr<mongo_embedded_v1_status, StatusDestructor>;

CapiStatusPtr makeStatusPtr() {
    return CapiStatusPtr{mongo_embedded_v1_status_create()};
}

struct ClientDestructor {
    void operator()(mongo_embedded_v1_client* const p) const noexcept {
        if (!p)
            return;

        auto status = makeStatusPtr();
        if (mongo_embedded_v1_client_destroy(p, status.get()) != MONGO_EMBEDDED_V1_SUCCESS) {
            std::cerr << "libmongodb_capi_client_destroy failed." << std::endl;
            if (status) {
                std::cerr << "Error code: " << mongo_embedded_v1_status_get_error(status.get())
                          << std::endl;
                std::cerr << "Error message: "
                          << mongo_embedded_v1_status_get_explanation(status.get()) << std::endl;
            }
        }
    }
};

using MongoDBCAPIClientPtr = std::unique_ptr<mongo_embedded_v1_client, ClientDestructor>;

class MongodbCAPITest : public mongo::unittest::Test {
protected:
    void setUp() {
        mongo_embedded_v1_init_params params;
        params.log_flags = MONGO_EMBEDDED_V1_LOG_STDOUT;
        params.log_callback = nullptr;
        params.log_user_data = nullptr;

        YAML::Emitter yaml;
        yaml << YAML::BeginMap;

        yaml << YAML::Key << "storage";
        yaml << YAML::Value << YAML::BeginMap;
        yaml << YAML::Key << "dbPath";
        yaml << YAML::Value << getGlobalTempDir()->path();
        yaml << YAML::EndMap;  // storage

        yaml << YAML::EndMap;

        params.yaml_config = yaml.c_str();

        auto* status = mongo_embedded_v1_status_create();
        ASSERT(status);

        lib = mongo_embedded_v1_lib_init(&params, status);
        ASSERT(lib != nullptr) << mongo_embedded_v1_status_get_explanation(status);

        db = mongo_embedded_v1_instance_create(lib, yaml.c_str(), status);
        ASSERT(db != nullptr) << mongo_embedded_v1_status_get_explanation(status);

        mongo_embedded_v1_status_destroy(status);
    }

    void tearDown() {
        auto* status = mongo_embedded_v1_status_create();
        ASSERT(status);

        ASSERT_EQUALS(mongo_embedded_v1_instance_destroy(db, status), MONGO_EMBEDDED_V1_SUCCESS)
            << mongo_embedded_v1_status_get_explanation(status);
        ASSERT_EQUALS(mongo_embedded_v1_lib_fini(lib, status), MONGO_EMBEDDED_V1_SUCCESS)
            << mongo_embedded_v1_status_get_explanation(status);

        mongo_embedded_v1_status_destroy(status);
    }

    mongo_embedded_v1_instance* getDB() const {
        return db;
    }

    MongoDBCAPIClientPtr createClient() const {
        auto* status = mongo_embedded_v1_status_create();
        ASSERT(status);

        MongoDBCAPIClientPtr client(mongo_embedded_v1_client_create(db, status));
        ASSERT(client.get() != nullptr) << mongo_embedded_v1_status_get_explanation(status);

        mongo_embedded_v1_status_destroy(status);
        return client;
    }

    mongo::Message messageFromBuffer(void* data, size_t dataLen) {
        auto sb = mongo::SharedBuffer::allocate(dataLen);
        memcpy(sb.get(), data, dataLen);
        mongo::Message msg(std::move(sb));
        return msg;
    }

    mongo::BSONObj performRpc(MongoDBCAPIClientPtr& client, mongo::OpMsgRequest request) {
        auto inputMessage = request.serialize();

        // declare the output size and pointer
        void* output;
        size_t outputSize;

        auto* status = mongo_embedded_v1_status_create();
        ASSERT(status);

        // call the wire protocol
        int err = mongo_embedded_v1_client_invoke(
            client.get(), inputMessage.buf(), inputMessage.size(), &output, &outputSize, status);
        ASSERT_EQUALS(err, MONGO_EMBEDDED_V1_SUCCESS);

        mongo_embedded_v1_status_destroy(status);

        // convert the shared buffer to a mongo::message and ensure that it is valid
        auto outputMessage = messageFromBuffer(output, outputSize);
        ASSERT(outputMessage.size() > 0);
        ASSERT(outputMessage.operation() == inputMessage.operation());

        // convert the message into an OpMessage to examine its BSON
        auto outputOpMsg = mongo::OpMsg::parseOwned(outputMessage);
        ASSERT_OK(validateBSON(outputOpMsg.body));
        return outputOpMsg.body;
    }


protected:
    mongo_embedded_v1_lib* lib;
    mongo_embedded_v1_instance* db;
};

TEST_F(MongodbCAPITest, CreateAndDestroyDB) {
    // Test the setUp() and tearDown() test fixtures
}

TEST_F(MongodbCAPITest, CreateAndDestroyDBAndClient) {
    auto client = createClient();
}

// This test is to make sure that destroying the db will fail if there's remaining clients left.
TEST_F(MongodbCAPITest, DoNotDestroyClient) {
    auto client = createClient();
    ASSERT(mongo_embedded_v1_instance_destroy(getDB(), nullptr) != MONGO_EMBEDDED_V1_SUCCESS);
}

TEST_F(MongodbCAPITest, CreateMultipleClients) {
    const int numClients = 10;
    std::set<MongoDBCAPIClientPtr> clients;
    for (int i = 0; i < numClients; i++) {
        clients.insert(createClient());
    }

    // ensure that each client is unique by making sure that the set size equals the number of
    // clients instantiated
    ASSERT_EQUALS(static_cast<int>(clients.size()), numClients);
}

TEST_F(MongodbCAPITest, IsMaster) {
    // create the client object
    auto client = createClient();

    // craft the isMaster message
    mongo::BSONObj inputObj = mongo::fromjson("{isMaster: 1}");
    auto inputOpMsg = mongo::OpMsgRequest::fromDBAndBody("admin", inputObj);
    auto output = performRpc(client, inputOpMsg);
    ASSERT(output.getBoolField("ismaster"));
}

TEST_F(MongodbCAPITest, CreateIndex) {
    // create the client object
    auto client = createClient();

    // craft the createIndexes message
    mongo::BSONObj inputObj = mongo::fromjson(
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
    auto inputOpMsg = mongo::OpMsgRequest::fromDBAndBody("index_db", inputObj);
    auto output = performRpc(client, inputOpMsg);

    ASSERT(output.hasField("ok")) << output;
    ASSERT(output.getField("ok").numberDouble() == 1.0) << output;
    ASSERT(output.getIntField("numIndexesAfter") == output.getIntField("numIndexesBefore") + 1)
        << output;
}

TEST_F(MongodbCAPITest, CreateBackgroundIndex) {
    // create the client object
    auto client = createClient();

    // craft the createIndexes message
    mongo::BSONObj inputObj = mongo::fromjson(
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
    auto inputOpMsg = mongo::OpMsgRequest::fromDBAndBody("background_index_db", inputObj);
    auto output = performRpc(client, inputOpMsg);

    ASSERT(output.hasField("ok")) << output;
    ASSERT(output.getField("ok").numberDouble() != 1.0) << output;
}

TEST_F(MongodbCAPITest, CreateTTLIndex) {
    // create the client object
    auto client = createClient();

    // craft the createIndexes message
    mongo::BSONObj inputObj = mongo::fromjson(
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
    auto inputOpMsg = mongo::OpMsgRequest::fromDBAndBody("ttl_index_db", inputObj);
    auto output = performRpc(client, inputOpMsg);

    ASSERT(output.hasField("ok")) << output;
    ASSERT(output.getField("ok").numberDouble() != 1.0) << output;
}

TEST_F(MongodbCAPITest, InsertDocument) {
    auto client = createClient();

    mongo::BSONObj insertObj = mongo::fromjson(
        "{insert: 'collection_name', documents: [{firstName: 'Mongo', lastName: 'DB', age: 10}]}");
    auto insertOpMsg = mongo::OpMsgRequest::fromDBAndBody("db_name", insertObj);
    auto outputBSON = performRpc(client, insertOpMsg);
    ASSERT(outputBSON.hasField("n"));
    ASSERT(outputBSON.getIntField("n") == 1);
    ASSERT(outputBSON.hasField("ok"));
    ASSERT(outputBSON.getField("ok").numberDouble() == 1.0);
}

TEST_F(MongodbCAPITest, InsertMultipleDocuments) {
    auto client = createClient();

    mongo::BSONObj insertObj = mongo::fromjson(
        "{insert: 'collection_name', documents: [{firstName: 'doc1FirstName', lastName: "
        "'doc1LastName', age: 30}, {firstName: 'doc2FirstName', lastName: 'doc2LastName', age: "
        "20}]}");
    auto insertOpMsg = mongo::OpMsgRequest::fromDBAndBody("db_name", insertObj);
    auto outputBSON = performRpc(client, insertOpMsg);
    ASSERT(outputBSON.hasField("n"));
    ASSERT(outputBSON.getIntField("n") == 2);
    ASSERT(outputBSON.hasField("ok"));
    ASSERT(outputBSON.getField("ok").numberDouble() == 1.0);
}

TEST_F(MongodbCAPITest, KillOp) {
    mongo::unittest::threadAssertionMonitoredTest([&](auto& assertionMonitor) {
        auto client = createClient();

        auto killOpThread = assertionMonitor.spawn([&]() {
            auto client = createClient();

            mongo::BSONObj currentOpObj = mongo::fromjson("{currentOp: 1}");
            auto currentOpMsg = mongo::OpMsgRequest::fromDBAndBody("admin", currentOpObj);
            mongo::BSONObj outputBSON;

            // Wait for the sleep command to start in the main test thread.
            int opid = -1;
            do {
                outputBSON = performRpc(client, currentOpMsg);
                auto inprog = outputBSON.getObjectField("inprog");

                // See if we find the sleep command among the running commands
                for (const auto& elt : inprog) {
                    auto inprogObj = inprog.getObjectField(elt.fieldNameStringData());
                    std::string ns = inprogObj.getStringField("ns").toString();
                    if (ns == "admin.$cmd") {
                        opid = inprogObj.getIntField("opid");
                        break;
                    }
                }
            } while (opid == -1);

            // Sleep command found, kill it.
            std::stringstream ss;
            ss << "{'killOp': 1, 'op': " << opid << "}";
            mongo::BSONObj killOpObj = mongo::fromjson(ss.str());
            auto killOpMsg = mongo::OpMsgRequest::fromDBAndBody("admin", killOpObj);
            outputBSON = performRpc(client, killOpMsg);

            ASSERT(outputBSON.hasField("ok"));
            ASSERT(outputBSON.getField("ok").numberDouble() == 1.0);
        });

        mongo::ScopeGuard guard = [&] { killOpThread.join(); };

        mongo::BSONObj sleepObj = mongo::fromjson("{'sleep': {'secs': 1000}}");
        auto sleepOpMsg = mongo::OpMsgRequest::fromDBAndBody("admin", sleepObj);
        auto outputBSON = performRpc(client, sleepOpMsg);

        ASSERT(outputBSON.hasField("ok"));
        ASSERT(outputBSON.getField("ok").numberDouble() != 1.0);
        ASSERT(outputBSON.getIntField("code") == mongo::ErrorCodes::Interrupted);
    });
}

TEST_F(MongodbCAPITest, ReadDB) {
    auto client = createClient();

    mongo::BSONObj findObj = mongo::fromjson("{find: 'collection_name', limit: 2}");
    auto findMsg = mongo::OpMsgRequest::fromDBAndBody("db_name", findObj);
    auto outputBSON = performRpc(client, findMsg);


    ASSERT_OK(validateBSON(outputBSON));
    ASSERT(outputBSON.hasField("cursor"));
    ASSERT(outputBSON.getField("cursor").embeddedObject().hasField("firstBatch"));
    mongo::BSONObj arrObj =
        outputBSON.getField("cursor").embeddedObject().getField("firstBatch").embeddedObject();
    ASSERT(arrObj.couldBeArray());

    mongo::BSONObjIterator i(arrObj);
    int index = 0;
    while (i.moreWithEOO()) {
        mongo::BSONElement e = i.next();
        if (e.eoo())
            break;
        index++;
    }
    ASSERT(index == 2);
}

TEST_F(MongodbCAPITest, InsertAndRead) {
    auto client = createClient();

    mongo::BSONObj insertObj = mongo::fromjson(
        "{insert: 'collection_name', documents: [{firstName: 'Mongo', lastName: 'DB', age: 10}]}");
    auto insertOpMsg = mongo::OpMsgRequest::fromDBAndBody("db_name", insertObj);
    auto outputBSON1 = performRpc(client, insertOpMsg);
    ASSERT_OK(validateBSON(outputBSON1));
    ASSERT(outputBSON1.hasField("n"));
    ASSERT(outputBSON1.getIntField("n") == 1);
    ASSERT(outputBSON1.hasField("ok"));
    ASSERT(outputBSON1.getField("ok").numberDouble() == 1.0);

    mongo::BSONObj findObj = mongo::fromjson("{find: 'collection_name', limit: 1}");
    auto findMsg = mongo::OpMsgRequest::fromDBAndBody("db_name", findObj);
    auto outputBSON2 = performRpc(client, findMsg);
    ASSERT_OK(validateBSON(outputBSON2));
    ASSERT(outputBSON2.hasField("cursor"));
    ASSERT(outputBSON2.getField("cursor").embeddedObject().hasField("firstBatch"));
    mongo::BSONObj arrObj =
        outputBSON2.getField("cursor").embeddedObject().getField("firstBatch").embeddedObject();
    ASSERT(arrObj.couldBeArray());

    mongo::BSONObjIterator i(arrObj);
    int index = 0;
    while (i.moreWithEOO()) {
        mongo::BSONElement e = i.next();
        if (e.eoo())
            break;
        index++;
    }
    ASSERT(index == 1);
}

TEST_F(MongodbCAPITest, InsertAndReadDifferentClients) {
    auto client1 = createClient();
    auto client2 = createClient();

    mongo::BSONObj insertObj = mongo::fromjson(
        "{insert: 'collection_name', documents: [{firstName: 'Mongo', lastName: 'DB', age: 10}]}");
    auto insertOpMsg = mongo::OpMsgRequest::fromDBAndBody("db_name", insertObj);
    auto outputBSON1 = performRpc(client1, insertOpMsg);
    ASSERT_OK(validateBSON(outputBSON1));
    ASSERT(outputBSON1.hasField("n"));
    ASSERT(outputBSON1.getIntField("n") == 1);
    ASSERT(outputBSON1.hasField("ok"));
    ASSERT(outputBSON1.getField("ok").numberDouble() == 1.0);

    mongo::BSONObj findObj = mongo::fromjson("{find: 'collection_name', limit: 1}");
    auto findMsg = mongo::OpMsgRequest::fromDBAndBody("db_name", findObj);
    auto outputBSON2 = performRpc(client2, findMsg);
    ASSERT_OK(validateBSON(outputBSON2));
    ASSERT(outputBSON2.hasField("cursor"));
    ASSERT(outputBSON2.getField("cursor").embeddedObject().hasField("firstBatch"));
    mongo::BSONObj arrObj =
        outputBSON2.getField("cursor").embeddedObject().getField("firstBatch").embeddedObject();
    ASSERT(arrObj.couldBeArray());

    mongo::BSONObjIterator i(arrObj);
    int index = 0;
    while (i.moreWithEOO()) {
        mongo::BSONElement e = i.next();
        if (e.eoo())
            break;
        index++;
    }
    ASSERT(index == 1);
}

TEST_F(MongodbCAPITest, InsertAndDelete) {
    auto client = createClient();
    mongo::BSONObj insertObj = mongo::fromjson(
        "{insert: 'collection_name', documents: [{firstName: 'toDelete', lastName: 'notImportant', "
        "age: 10}]}");
    auto insertOpMsg = mongo::OpMsgRequest::fromDBAndBody("db_name", insertObj);
    auto outputBSON1 = performRpc(client, insertOpMsg);
    ASSERT_OK(validateBSON(outputBSON1));
    ASSERT(outputBSON1.hasField("n"));
    ASSERT(outputBSON1.getIntField("n") == 1);
    ASSERT(outputBSON1.hasField("ok"));
    ASSERT(outputBSON1.getField("ok").numberDouble() == 1.0);


    // Delete
    mongo::BSONObj deleteObj = mongo::fromjson(
        "{delete: 'collection_name', deletes:   [{q: {firstName: 'toDelete', age: 10}, limit: "
        "1}]}");
    auto deleteOpMsg = mongo::OpMsgRequest::fromDBAndBody("db_name", deleteObj);
    auto outputBSON2 = performRpc(client, deleteOpMsg);
    ASSERT_OK(validateBSON(outputBSON2));
    ASSERT(outputBSON2.hasField("n"));
    ASSERT(outputBSON2.getIntField("n") == 1);
    ASSERT(outputBSON2.hasField("ok"));
    ASSERT(outputBSON2.getField("ok").numberDouble() == 1.0);
}


TEST_F(MongodbCAPITest, InsertAndUpdate) {
    auto client = createClient();

    mongo::BSONObj insertObj = mongo::fromjson(
        "{insert: 'collection_name', documents: [{firstName: 'toUpdate', lastName: 'notImportant', "
        "age: 10}]}");
    auto insertOpMsg = mongo::OpMsgRequest::fromDBAndBody("db_name", insertObj);
    auto outputBSON1 = performRpc(client, insertOpMsg);
    ASSERT_OK(validateBSON(outputBSON1));
    ASSERT(outputBSON1.hasField("n"));
    ASSERT(outputBSON1.getIntField("n") == 1);
    ASSERT(outputBSON1.hasField("ok"));
    ASSERT(outputBSON1.getField("ok").numberDouble() == 1.0);


    // Update
    mongo::BSONObj updateObj = mongo::fromjson(
        "{update: 'collection_name', updates: [ {q: {firstName: 'toUpdate', age: 10}, u: {'$inc': "
        "{age: 5}}}]}");
    auto updateOpMsg = mongo::OpMsgRequest::fromDBAndBody("db_name", updateObj);
    auto outputBSON2 = performRpc(client, updateOpMsg);
    ASSERT_OK(validateBSON(outputBSON2));
    ASSERT(outputBSON2.hasField("ok"));
    ASSERT(outputBSON2.getField("ok").numberDouble() == 1.0);
    ASSERT(outputBSON2.hasField("nModified"));
    ASSERT(outputBSON2.getIntField("nModified") == 1);
}

TEST_F(MongodbCAPITest, RunListCommands) {
    auto client = createClient();

    std::vector<std::string> allowlist = {"_hashBSONElement",
                                          "_killOperations",
                                          "aggregate",
                                          "analyze",
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
                                          "listDatabasesForAllTenants",
                                          "listIndexes",
                                          "lockInfo",
                                          "logMessage",
                                          "ping",
                                          "planCacheClear",
                                          "planCacheClearFilters",
                                          "planCacheListFilters",
                                          "planCacheSetFilter",
                                          "reIndex",
                                          "refreshLogicalSessionCacheNow",
                                          "refreshSessions",
                                          "renameCollection",
                                          "serverStatus",
                                          "setParameter",
                                          "sleep",
                                          "startSession",
                                          "update",
                                          "validate",
                                          "validateDBMetadata",
                                          "waitForFailPoint",
                                          "whatsmysni"};

    std::sort(allowlist.begin(), allowlist.end());

    mongo::BSONObj listCommandsObj = mongo::fromjson("{ listCommands: 1 }");
    auto listCommandsOpMsg = mongo::OpMsgRequest::fromDBAndBody("db_name", listCommandsObj);
    auto output = performRpc(client, listCommandsOpMsg);
    auto commandsBSON = output["commands"];
    std::vector<std::string> commands;
    for (const auto& element : commandsBSON.Obj()) {
        commands.push_back(element.fieldNameStringData().toString());
    }
    std::sort(commands.begin(), commands.end());

    std::vector<std::string> missing;
    std::vector<std::string> unsupported;
    std::set_difference(allowlist.begin(),
                        allowlist.end(),
                        commands.begin(),
                        commands.end(),
                        std::back_inserter(missing));
    std::set_difference(commands.begin(),
                        commands.end(),
                        allowlist.begin(),
                        allowlist.end(),
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

    ASSERT(missing.empty()) << mongo::StringSplitter::join(missing, ", ");
    ASSERT(unsupported.empty()) << mongo::StringSplitter::join(unsupported, ", ");
}

// This test is temporary to make sure that only one database can be created
// This restriction may be relaxed at a later time
TEST_F(MongodbCAPITest, CreateMultipleDBs) {
    auto status = makeStatusPtr();
    ASSERT(status.get());
    mongo_embedded_v1_instance* db2 = mongo_embedded_v1_instance_create(lib, nullptr, status.get());
    ASSERT(db2 == nullptr);
    ASSERT_EQUALS(mongo_embedded_v1_status_get_error(status.get()),
                  MONGO_EMBEDDED_V1_ERROR_DB_MAX_OPEN);
}
}  // namespace

// Define main function as an entry to these tests.
// These test functions cannot use the main() defined for unittests because they
// call runGlobalInitializers(). The embedded C API calls mongoDbMain() which
// calls runGlobalInitializers().
int main(const int argc, const char* const* const argv) {
    moe::Environment environment;
    moe::OptionSection options;

    auto ret = mongo::embedded::addMongoEmbeddedTestOptions(&options);
    if (!ret.isOK()) {
        std::cerr << ret << std::endl;
        return static_cast<int>(mongo::ExitCode::fail);
    }

    ret = moe::OptionsParser().run(
        options, std::vector<std::string>(argv, argv + argc), &environment);
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

    // Allocate an error descriptor for use in non-configured tests
    const auto status = makeStatusPtr();

    mongo::setTestCommandsEnabled(true);

    // Perform one cycle of global initialization/deinitialization before running test. This will
    // make sure everything that is needed is setup for the unittest infrastructure.
    // The reason this works is that the unittest system relies on other systems being initialized
    // through global init and deinitialize just deinitializes systems that explicitly supports
    // deinit leaving the systems unittest needs initialized.
    ret = mongo::runGlobalInitializers(std::vector<std::string>{argv, argv + argc});
    if (!ret.isOK()) {
        std::cerr << "Global initilization failed";
        return static_cast<int>(mongo::ExitCode::fail);
    }

    ret = mongo::runGlobalDeinitializers();
    if (!ret.isOK()) {
        std::cerr << "Global deinitilization failed";
        return static_cast<int>(mongo::ExitCode::fail);
    }

    // Check so we can initialize the library without providing init params
    mongo_embedded_v1_lib* lib = mongo_embedded_v1_lib_init(nullptr, status.get());
    if (lib == nullptr) {
        std::cerr << "mongo_embedded_v1_init() failed with "
                  << mongo_embedded_v1_status_get_error(status.get()) << ": "
                  << mongo_embedded_v1_status_get_explanation(status.get()) << std::endl;
        return static_cast<int>(mongo::ExitCode::fail);
    }

    if (mongo_embedded_v1_lib_fini(lib, status.get()) != MONGO_EMBEDDED_V1_SUCCESS) {
        std::cerr << "mongo_embedded_v1_fini() failed with "
                  << mongo_embedded_v1_status_get_error(status.get()) << ": "
                  << mongo_embedded_v1_status_get_explanation(status.get()) << std::endl;
        return static_cast<int>(mongo::ExitCode::fail);
    }

    // Initialize the library with a log callback and test so we receive at least one callback
    // during the lifetime of the test
    mongo_embedded_v1_init_params params{};

    bool receivedCallback = false;
    params.log_flags = MONGO_EMBEDDED_V1_LOG_STDOUT | MONGO_EMBEDDED_V1_LOG_CALLBACK;
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

    YAML::Emitter yaml;
    yaml << YAML::BeginMap;
    yaml << YAML::Key << "storage";
    yaml << YAML::Value << YAML::BeginMap;
    yaml << YAML::Key << "dbPath";
    yaml << YAML::Value << getGlobalTempDir()->path();
    yaml << YAML::EndMap;  // storage
    yaml << YAML::EndMap;
    params.yaml_config = yaml.c_str();

    lib = mongo_embedded_v1_lib_init(&params, nullptr);
    if (lib == nullptr) {
        std::cerr << "mongo_embedded_v1_init() failed with "
                  << mongo_embedded_v1_status_get_error(status.get()) << ": "
                  << mongo_embedded_v1_status_get_explanation(status.get()) << std::endl;
    }

    // Attempt to create an embedded instance to make sure something gets logged. This will probably
    // fail but that's fine.
    mongo_embedded_v1_instance* instance =
        mongo_embedded_v1_instance_create(lib, yaml.c_str(), nullptr);
    if (instance) {
        mongo_embedded_v1_instance_destroy(instance, nullptr);
    }

    if (mongo_embedded_v1_lib_fini(lib, nullptr) != MONGO_EMBEDDED_V1_SUCCESS) {
        std::cerr << "mongo_embedded_v1_fini() failed with "
                  << mongo_embedded_v1_status_get_error(status.get()) << ": "
                  << mongo_embedded_v1_status_get_explanation(status.get()) << std::endl;
    }

    if (!receivedCallback) {
        std::cerr << "Did not get a log callback." << std::endl;
        return static_cast<int>(mongo::ExitCode::fail);
    }

    const auto result = ::mongo::unittest::Suite::run(std::vector<std::string>(), "", "", 1);

    getGlobalTempDir().reset();
    mongo::quickExit(result);
}
