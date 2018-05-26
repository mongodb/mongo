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


#include "mongo/embedded/capi.h"

#include <memory>
#include <set>
#include <yaml-cpp/yaml.h>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/json.h"
#include "mongo/db/server_options.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/shared_buffer.h"
#include "mongo/util/signal_handlers_synchronous.h"

namespace moe = mongo::optionenvironment;

mongo_embedded_v1_lib* global_lib_handle;

namespace {

std::unique_ptr<mongo::unittest::TempDir> globalTempDir;

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
        status = mongo_embedded_v1_status_create();
        ASSERT(status != nullptr);

        if (!globalTempDir) {
            globalTempDir = std::make_unique<mongo::unittest::TempDir>("embedded_mongo");
        }

        mongo_embedded_v1_init_params params;
        params.log_flags = 0;
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

        lib = mongo_embedded_v1_lib_init(&params, status);
        ASSERT(lib != nullptr) << mongo_embedded_v1_status_get_explanation(status);

        db = mongo_embedded_v1_instance_create(lib, yaml.c_str(), status);
        ASSERT(db != nullptr) << mongo_embedded_v1_status_get_explanation(status);
    }

    void tearDown() {
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
        MongoDBCAPIClientPtr client(mongo_embedded_v1_client_create(db, status));
        ASSERT(client.get() != nullptr) << mongo_embedded_v1_status_get_explanation(status);
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

        // call the wire protocol
        int err = mongo_embedded_v1_client_invoke(
            client.get(), inputMessage.buf(), inputMessage.size(), &output, &outputSize, status);
        ASSERT_EQUALS(err, MONGO_EMBEDDED_V1_SUCCESS);

        // convert the shared buffer to a mongo::message and ensure that it is valid
        auto outputMessage = messageFromBuffer(output, outputSize);
        ASSERT(outputMessage.size() > 0);
        ASSERT(outputMessage.operation() == inputMessage.operation());

        // convert the message into an OpMessage to examine its BSON
        auto outputOpMsg = mongo::OpMsg::parseOwned(outputMessage);
        ASSERT(outputOpMsg.body.valid(mongo::BSONVersion::kLatest));
        return outputOpMsg.body;
    }


protected:
    mongo_embedded_v1_lib* lib;
    mongo_embedded_v1_instance* db;
    mongo_embedded_v1_status* status;
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

    ASSERT(output.hasField("ok"));
    ASSERT(output.getField("ok").numberDouble() == 1.0);
    ASSERT(output.getIntField("numIndexesAfter") == output.getIntField("numIndexesBefore") + 1);
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

    ASSERT(output.hasField("ok"));
    ASSERT(output.getField("ok").numberDouble() != 1.0);
}

TEST_F(MongodbCAPITest, TrimMemory) {
    // create the client object
    auto client = createClient();

    // craft the isMaster message
    mongo::BSONObj inputObj = mongo::fromjson("{trimMemory: 'aggressive'}");
    auto inputOpMsg = mongo::OpMsgRequest::fromDBAndBody("admin", inputObj);
    performRpc(client, inputOpMsg);
}

TEST_F(MongodbCAPITest, BatteryLevel) {
    // create the client object
    auto client = createClient();

    // craft the isMaster message
    mongo::BSONObj inputObj = mongo::fromjson("{setBatteryLevel: 'low'}");
    auto inputOpMsg = mongo::OpMsgRequest::fromDBAndBody("admin", inputObj);
    performRpc(client, inputOpMsg);
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
    auto client = createClient();

    mongo::stdx::thread killOpThread([this]() {
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
        mongo::BSONObj killOpObj = mongo::fromjson(ss.str());
        auto killOpMsg = mongo::OpMsgRequest::fromDBAndBody("admin", killOpObj);
        outputBSON = performRpc(client, killOpMsg);

        ASSERT(outputBSON.hasField("ok"));
        ASSERT(outputBSON.getField("ok").numberDouble() == 1.0);
    });

    mongo::BSONObj sleepObj = mongo::fromjson("{'sleep': {'secs': 1000}}");
    auto sleepOpMsg = mongo::OpMsgRequest::fromDBAndBody("admin", sleepObj);
    auto outputBSON = performRpc(client, sleepOpMsg);

    ASSERT(outputBSON.hasField("ok"));
    ASSERT(outputBSON.getField("ok").numberDouble() != 1.0);
    ASSERT(outputBSON.getIntField("code") == mongo::ErrorCodes::Interrupted);

    killOpThread.join();
}

TEST_F(MongodbCAPITest, ReadDB) {
    auto client = createClient();

    mongo::BSONObj findObj = mongo::fromjson("{find: 'collection_name', limit: 2}");
    auto findMsg = mongo::OpMsgRequest::fromDBAndBody("db_name", findObj);
    auto outputBSON = performRpc(client, findMsg);


    ASSERT(outputBSON.valid(mongo::BSONVersion::kLatest));
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
    ASSERT(outputBSON1.valid(mongo::BSONVersion::kLatest));
    ASSERT(outputBSON1.hasField("n"));
    ASSERT(outputBSON1.getIntField("n") == 1);
    ASSERT(outputBSON1.hasField("ok"));
    ASSERT(outputBSON1.getField("ok").numberDouble() == 1.0);

    mongo::BSONObj findObj = mongo::fromjson("{find: 'collection_name', limit: 1}");
    auto findMsg = mongo::OpMsgRequest::fromDBAndBody("db_name", findObj);
    auto outputBSON2 = performRpc(client, findMsg);
    ASSERT(outputBSON2.valid(mongo::BSONVersion::kLatest));
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
    ASSERT(outputBSON1.valid(mongo::BSONVersion::kLatest));
    ASSERT(outputBSON1.hasField("n"));
    ASSERT(outputBSON1.getIntField("n") == 1);
    ASSERT(outputBSON1.hasField("ok"));
    ASSERT(outputBSON1.getField("ok").numberDouble() == 1.0);

    mongo::BSONObj findObj = mongo::fromjson("{find: 'collection_name', limit: 1}");
    auto findMsg = mongo::OpMsgRequest::fromDBAndBody("db_name", findObj);
    auto outputBSON2 = performRpc(client2, findMsg);
    ASSERT(outputBSON2.valid(mongo::BSONVersion::kLatest));
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
    ASSERT(outputBSON1.valid(mongo::BSONVersion::kLatest));
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
    ASSERT(outputBSON2.valid(mongo::BSONVersion::kLatest));
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
    ASSERT(outputBSON1.valid(mongo::BSONVersion::kLatest));
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
    ASSERT(outputBSON2.valid(mongo::BSONVersion::kLatest));
    ASSERT(outputBSON2.hasField("ok"));
    ASSERT(outputBSON2.getField("ok").numberDouble() == 1.0);
    ASSERT(outputBSON2.hasField("nModified"));
    ASSERT(outputBSON2.getIntField("nModified") == 1);
}

TEST_F(MongodbCAPITest, RunListCommands) {
    auto client = createClient();

    std::vector<std::string> whitelist = {"_hashBSONElement",
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
                                          "explain",
                                          "find",
                                          "findAndModify",
                                          "geoNear",
                                          "getLastError",
                                          "getMore",
                                          "getParameter",
                                          "getPrevError",
                                          "getShardMap",
                                          "insert",
                                          "isMaster",
                                          "killCursors",
                                          "killOp",
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
                                          "renameCollection",
                                          "repairCursor",
                                          "repairDatabase",
                                          "resetError",
                                          "serverStatus",
                                          "setBatteryLevel",
                                          "setParameter",
                                          "sleep",
                                          "trimMemory",
                                          "update",
                                          "validate"};
    std::sort(whitelist.begin(), whitelist.end());

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

    options.addOptionChaining(
        "tempPath", "tempPath", moe::String, "directory to place mongo::TempDir subdirectories");

    std::map<std::string, std::string> env;
    mongo::Status ret = moe::OptionsParser().run(
        options, std::vector<std::string>(argv, argv + argc), env, &environment);
    if (!ret.isOK()) {
        std::cerr << options.helpString();
        return EXIT_FAILURE;
    }
    if (environment.count("tempPath")) {
        ::mongo::unittest::TempDir::setTempPath(environment["tempPath"].as<std::string>());
    }

    ::mongo::clearSignalMask();
    ::mongo::setupSynchronousSignalHandlers();
    ::mongo::serverGlobalParams.noUnixSocket = true;
    ::mongo::unittest::setupTestLogger();

    // Allocate an error descriptor for use in non-configured tests
    const auto status = makeStatusPtr();

    mongo::setTestCommandsEnabled(true);

    // Check so we can initialize the library without providing init params
    mongo_embedded_v1_lib* lib = mongo_embedded_v1_lib_init(nullptr, status.get());
    if (lib == nullptr) {
        std::cerr << "mongo_embedded_v1_init() failed with "
                  << mongo_embedded_v1_status_get_error(status.get()) << ": "
                  << mongo_embedded_v1_status_get_explanation(status.get()) << std::endl;
        return EXIT_FAILURE;
    }

    if (mongo_embedded_v1_lib_fini(lib, status.get()) != MONGO_EMBEDDED_V1_SUCCESS) {
        std::cerr << "mongo_embedded_v1_fini() failed with "
                  << mongo_embedded_v1_status_get_error(status.get()) << ": "
                  << mongo_embedded_v1_status_get_explanation(status.get()) << std::endl;
        return EXIT_FAILURE;
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

    lib = mongo_embedded_v1_lib_init(&params, nullptr);
    if (lib == nullptr) {
        std::cerr << "mongo_embedded_v1_init() failed with "
                  << mongo_embedded_v1_status_get_error(status.get()) << ": "
                  << mongo_embedded_v1_status_get_explanation(status.get()) << std::endl;
    }

    if (mongo_embedded_v1_lib_fini(lib, nullptr) != MONGO_EMBEDDED_V1_SUCCESS) {
        std::cerr << "mongo_embedded_v1_fini() failed with "
                  << mongo_embedded_v1_status_get_error(status.get()) << ": "
                  << mongo_embedded_v1_status_get_explanation(status.get()) << std::endl;
    }

    if (!receivedCallback) {
        std::cerr << "Did not get a log callback." << std::endl;
    }

    const auto result = ::mongo::unittest::Suite::run(std::vector<std::string>(), "", 1);

    globalTempDir.reset();
    mongo::quickExit(result);
}
