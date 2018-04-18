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
#include <yaml-cpp/yaml.h>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/json.h"
#include "mongo/db/server_options.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/message.h"
#include "mongo/util/net/op_msg.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/shared_buffer.h"
#include "mongo/util/signal_handlers_synchronous.h"

namespace moe = mongo::optionenvironment;

namespace {

std::unique_ptr<mongo::unittest::TempDir> globalTempDir;

struct MongodCapiCleaner {
    void operator()(libmongodbcapi_client* const p) {
        if (p) {
            libmongodbcapi_db_client_destroy(p);
        }
    }
};

using MongoDBCAPIClientPtr = std::unique_ptr<libmongodbcapi_client, MongodCapiCleaner>;

class MongodbCAPITest : public mongo::unittest::Test {
protected:
    void setUp() {
        if (!globalTempDir) {
            globalTempDir = mongo::stdx::make_unique<mongo::unittest::TempDir>("embedded_mongo");
        }

        YAML::Emitter yaml;
        yaml << YAML::BeginMap;

        yaml << YAML::Key << "storage";
        yaml << YAML::Value << YAML::BeginMap;
        yaml << YAML::Key << "dbPath";
        yaml << YAML::Value << globalTempDir->path();
        yaml << YAML::EndMap;  // storage

        yaml << YAML::EndMap;

        db = libmongodbcapi_db_new(yaml.c_str());
        ASSERT(db != nullptr);
    }

    void tearDown() {
        libmongodbcapi_db_destroy(db);
        ASSERT_EQUALS(libmongodbcapi_get_last_error(), LIBMONGODB_CAPI_SUCCESS);
    }

    libmongodbcapi_db* getDB() {
        return db;
    }

    MongoDBCAPIClientPtr createClient() {
        MongoDBCAPIClientPtr client(libmongodbcapi_db_client_new(db));
        ASSERT(client != nullptr);
        ASSERT_EQUALS(libmongodbcapi_get_last_error(), LIBMONGODB_CAPI_SUCCESS);
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
        int err = libmongodbcapi_db_client_wire_protocol_rpc(
            client.get(), inputMessage.buf(), inputMessage.size(), &output, &outputSize);
        ASSERT_EQUALS(err, LIBMONGODB_CAPI_SUCCESS);

        // convert the shared buffer to a mongo::message and ensure that it is valid
        auto outputMessage = messageFromBuffer(output, outputSize);
        ASSERT(outputMessage.size() > 0);
        ASSERT(outputMessage.operation() == inputMessage.operation());

        // convert the message into an OpMessage to examine its BSON
        auto outputOpMsg = mongo::OpMsg::parseOwned(outputMessage);
        ASSERT(outputOpMsg.body.valid(mongo::BSONVersion::kLatest));
        return outputOpMsg.body;
    }


private:
    libmongodbcapi_db* db;
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
    ASSERT(libmongodbcapi_db_destroy(getDB()) != LIBMONGODB_CAPI_SUCCESS);
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

TEST_F(MongodbCAPITest, DBPump) {
    libmongodbcapi_db* db = getDB();
    int err = libmongodbcapi_db_pump(db);
    ASSERT_EQUALS(err, LIBMONGODB_CAPI_SUCCESS);
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

// This test is temporary to make sure that only one database can be created
// This restriction may be relaxed at a later time
TEST_F(MongodbCAPITest, CreateMultipleDBs) {
    libmongodbcapi_db* db2 = libmongodbcapi_db_new(nullptr);
    ASSERT(db2 == nullptr);
    ASSERT_EQUALS(libmongodbcapi_get_last_error(), LIBMONGODB_CAPI_ERROR_UNKNOWN);
}
}  // namespace

// Define main function as an entry to these tests.
// These test functions cannot use the main() defined for unittests because they
// call runGlobalInitializers(). The embedded C API calls mongoDbMain() which
// calls runGlobalInitializers().
int main(int argc, char** argv, char** envp) {
    moe::OptionsParser parser;
    moe::Environment environment;
    moe::OptionSection options;
    std::map<std::string, std::string> env;

    options.addOptionChaining(
        "tempPath", "tempPath", moe::String, "directory to place mongo::TempDir subdirectories");
    std::vector<std::string> argVector(argv, argv + argc);
    mongo::Status ret = parser.run(options, argVector, env, &environment);
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

    mongo::setTestCommandsEnabled(true);

    // Check so we can initialize the library without providing init params
    int init = libmongodbcapi_init(nullptr);
    if (init != LIBMONGODB_CAPI_SUCCESS) {
        std::cerr << "libmongodbcapi_init() failed with " << init << std::endl;
        return EXIT_FAILURE;
    }

    int fini = libmongodbcapi_fini();
    if (fini != LIBMONGODB_CAPI_SUCCESS) {
        std::cerr << "libmongodbcapi_fini() failed with " << fini << std::endl;
        return EXIT_FAILURE;
    }

    // Initialize the library with a log callback and test so we receive at least one callback
    // during the lifetime of the test
    libmongodbcapi_init_params params;
    memset(&params, 0, sizeof(params));

    bool receivedCallback = false;
    params.log_flags = LIBMONGODB_CAPI_LOG_STDOUT | LIBMONGODB_CAPI_LOG_CALLBACK;
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

    init = libmongodbcapi_init(&params);
    if (init != LIBMONGODB_CAPI_SUCCESS) {
        std::cerr << "libmongodbcapi_init() failed with " << init << std::endl;
        return EXIT_FAILURE;
    }

    ::mongo::unittest::Suite::run(std::vector<std::string>(), "", 1);

    fini = libmongodbcapi_fini();
    if (fini != LIBMONGODB_CAPI_SUCCESS) {
        std::cerr << "libmongodbcapi_fini() failed with " << fini << std::endl;
        return EXIT_FAILURE;
    }

    ASSERT(receivedCallback);

    globalTempDir.reset();
}
