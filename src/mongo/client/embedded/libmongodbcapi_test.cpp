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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/json.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/message.h"
#include "mongo/util/net/op_msg.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/shared_buffer.h"
#include "mongo/util/signal_handlers_synchronous.h"

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
        const char* argv[] = {
            "mongo_embedded_capi_test", "--port", "0", "--dbpath", globalTempDir->path().c_str()};
        db = libmongodbcapi_db_new(5, argv, nullptr);
        ASSERT(db != nullptr);
    }

    void tearDown() {
        libmongodbcapi_db_destroy(db);
        ASSERT_EQUALS(libmongodbcapi_get_last_error(), LIBMONGODB_CAPI_ERROR_SUCCESS);
    }

    libmongodbcapi_db* getDB() {
        return db;
    }

    MongoDBCAPIClientPtr createClient() {
        MongoDBCAPIClientPtr client(libmongodbcapi_db_client_new(db));
        ASSERT(client != nullptr);
        ASSERT_EQUALS(libmongodbcapi_get_last_error(), LIBMONGODB_CAPI_ERROR_SUCCESS);
        return client;
    }

    mongo::Message messageFromBuffer(void* data, size_t dataLen) {
        auto sb = mongo::SharedBuffer::allocate(dataLen);
        memcpy(sb.get(), data, dataLen);
        mongo::Message msg(std::move(sb));
        return msg;
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

// This test is to make sure that destroying the db will destroy all of its clients
// This test will only fail under ASAN
TEST_F(MongodbCAPITest, DoNotDestroyClient) {
    createClient().release();
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
    ASSERT_EQUALS(err, LIBMONGODB_CAPI_ERROR_SUCCESS);
}

TEST_F(MongodbCAPITest, IsMaster) {
    // create the client object
    auto client = createClient();

    // craft the isMaster message
    mongo::BSONObj inputObj = mongo::fromjson("{isMaster: 1}");
    auto inputOpMsg = mongo::OpMsgRequest::fromDBAndBody("admin", inputObj);
    auto inputMessage = inputOpMsg.serialize();


    // declare the output size and pointer
    void* output;
    size_t outputSize;

    // call the wire protocol
    int err = libmongodbcapi_db_client_wire_protocol_rpc(
        client.get(), inputMessage.buf(), inputMessage.size(), &output, &outputSize);
    ASSERT_EQUALS(err, LIBMONGODB_CAPI_ERROR_SUCCESS);

    // convert the shared buffer to a mongo::message and ensure that it is valid
    auto outputMessage = messageFromBuffer(output, outputSize);
    ASSERT(outputMessage.size() > 0);
    ASSERT(outputMessage.operation() == inputMessage.operation());

    // convert the message into an OpMessage to examine its BSON
    auto outputOpMsg = mongo::OpMsg::parseOwned(outputMessage);
    ASSERT(outputOpMsg.body.valid(mongo::BSONVersion::kLatest));
    ASSERT(outputOpMsg.body.getBoolField("ismaster"));
}


TEST_F(MongodbCAPITest, InsertDocument) {
    auto client = createClient();

    mongo::BSONObj insertObj = mongo::fromjson(
        "{insert: 'collection_name', documents: [{firstName: 'Mongo', lastName: 'DB', age: 10}]}");
    auto insertOpMsg = mongo::OpMsgRequest::fromDBAndBody("db_name", insertObj);
    mongo::Message insertMessage = insertOpMsg.serialize();

    void* output;
    size_t outputSize;
    int err = libmongodbcapi_db_client_wire_protocol_rpc(
        client.get(), insertMessage.buf(), insertMessage.size(), &output, &outputSize);
    ASSERT_EQUALS(err, LIBMONGODB_CAPI_ERROR_SUCCESS);

    auto outputMessage = messageFromBuffer(output, outputSize);
    ASSERT(outputMessage.size() > 0);
    ASSERT(outputMessage.operation() == insertMessage.operation());

    auto outputOpMsg = mongo::OpMsg::parseOwned(outputMessage);
    auto outputBSON = outputOpMsg.body;

    ASSERT(outputBSON.valid(mongo::BSONVersion::kLatest));
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
    mongo::Message insertMessage = insertOpMsg.serialize();

    void* output;
    size_t outputSize;
    int err = libmongodbcapi_db_client_wire_protocol_rpc(
        client.get(), insertMessage.buf(), insertMessage.size(), &output, &outputSize);
    ASSERT_EQUALS(err, LIBMONGODB_CAPI_ERROR_SUCCESS);

    auto outputMessage = messageFromBuffer(output, outputSize);
    ASSERT(outputMessage.size() > 0);
    ASSERT(outputMessage.operation() == insertMessage.operation());

    auto outputOpMsg = mongo::OpMsg::parseOwned(outputMessage);
    auto outputBSON = outputOpMsg.body;

    ASSERT(outputBSON.valid(mongo::BSONVersion::kLatest));
    ASSERT(outputBSON.hasField("n"));
    ASSERT(outputBSON.getIntField("n") == 2);
    ASSERT(outputBSON.hasField("ok"));
    ASSERT(outputBSON.getField("ok").numberDouble() == 1.0);
}

TEST_F(MongodbCAPITest, ReadDB) {
    auto client = createClient();

    mongo::BSONObj findObj = mongo::fromjson("{find: 'collection_name', limit: 2}");
    auto findMsg = mongo::OpMsgRequest::fromDBAndBody("db_name", findObj);
    mongo::Message findMessage = findMsg.serialize();

    void* output;
    size_t outputSize;
    int err = libmongodbcapi_db_client_wire_protocol_rpc(
        client.get(), findMessage.buf(), findMessage.size(), &output, &outputSize);
    ASSERT_EQUALS(err, LIBMONGODB_CAPI_ERROR_SUCCESS);

    auto outputMessage = messageFromBuffer(output, outputSize);
    ASSERT(outputMessage.size() > 0);

    auto outputOpMsg = mongo::OpMsg::parseOwned(outputMessage);
    mongo::BSONObj outputBSON = outputOpMsg.body;

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
    mongo::Message insertMessage = insertOpMsg.serialize();

    void* output;
    size_t outputSize;
    int err = libmongodbcapi_db_client_wire_protocol_rpc(
        client.get(), insertMessage.buf(), insertMessage.size(), &output, &outputSize);
    ASSERT_EQUALS(err, LIBMONGODB_CAPI_ERROR_SUCCESS);

    auto outputMessage1 = messageFromBuffer(output, outputSize);
    ASSERT(outputMessage1.size() > 0);
    ASSERT(outputMessage1.operation() == insertMessage.operation());

    auto outputOpMsg1 = mongo::OpMsg::parseOwned(outputMessage1);
    mongo::BSONObj outputBSON1 = outputOpMsg1.body;
    ASSERT(outputBSON1.valid(mongo::BSONVersion::kLatest));
    ASSERT(outputBSON1.hasField("n"));
    ASSERT(outputBSON1.getIntField("n") == 1);
    ASSERT(outputBSON1.hasField("ok"));
    ASSERT(outputBSON1.getField("ok").numberDouble() == 1.0);

    mongo::BSONObj findObj = mongo::fromjson("{find: 'collection_name', limit: 1}");
    auto findMsg = mongo::OpMsgRequest::fromDBAndBody("db_name", findObj);
    mongo::Message findMessage = findMsg.serialize();

    void* output2;
    size_t outputSize2;
    err = libmongodbcapi_db_client_wire_protocol_rpc(
        client.get(), findMessage.buf(), findMessage.size(), &output2, &outputSize2);
    ASSERT_EQUALS(err, LIBMONGODB_CAPI_ERROR_SUCCESS);

    auto outputMessage2 = messageFromBuffer(output2, outputSize2);
    ASSERT(outputMessage2.size() > 0);

    auto outputOpMsg2 = mongo::OpMsg::parseOwned(outputMessage2);
    mongo::BSONObj outputBSON2 = outputOpMsg2.body;

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
    mongo::Message insertMessage = insertOpMsg.serialize();

    void* output;
    size_t outputSize;
    int err = libmongodbcapi_db_client_wire_protocol_rpc(
        client1.get(), insertMessage.buf(), insertMessage.size(), &output, &outputSize);
    ASSERT_EQUALS(err, LIBMONGODB_CAPI_ERROR_SUCCESS);

    auto outputMessage1 = messageFromBuffer(output, outputSize);
    ASSERT(outputMessage1.size() > 0);
    ASSERT(outputMessage1.operation() == insertMessage.operation());

    auto outputOpMsg1 = mongo::OpMsg::parseOwned(outputMessage1);
    mongo::BSONObj outputBSON1 = outputOpMsg1.body;
    ASSERT(outputBSON1.valid(mongo::BSONVersion::kLatest));
    ASSERT(outputBSON1.hasField("n"));
    ASSERT(outputBSON1.getIntField("n") == 1);
    ASSERT(outputBSON1.hasField("ok"));
    ASSERT(outputBSON1.getField("ok").numberDouble() == 1.0);

    mongo::BSONObj findObj = mongo::fromjson("{find: 'collection_name', limit: 1}");
    auto findMsg = mongo::OpMsgRequest::fromDBAndBody("db_name", findObj);
    mongo::Message findMessage = findMsg.serialize();

    void* output2;
    size_t outputSize2;
    err = libmongodbcapi_db_client_wire_protocol_rpc(
        client2.get(), findMessage.buf(), findMessage.size(), &output2, &outputSize2);
    ASSERT_EQUALS(err, LIBMONGODB_CAPI_ERROR_SUCCESS);

    auto outputMessage2 = messageFromBuffer(output2, outputSize2);
    ASSERT(outputMessage2.size() > 0);

    auto outputOpMsg2 = mongo::OpMsg::parseOwned(outputMessage2);
    mongo::BSONObj outputBSON2 = outputOpMsg2.body;

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
    mongo::Message insertMessage = insertOpMsg.serialize();

    void* output;
    size_t outputSize;
    int err = libmongodbcapi_db_client_wire_protocol_rpc(
        client.get(), insertMessage.buf(), insertMessage.size(), &output, &outputSize);
    ASSERT_EQUALS(err, LIBMONGODB_CAPI_ERROR_SUCCESS);

    auto outputMessage1 = messageFromBuffer(output, outputSize);
    ASSERT(outputMessage1.size() > 0);
    ASSERT(outputMessage1.operation() == insertMessage.operation());

    auto outputOpMsg1 = mongo::OpMsg::parseOwned(outputMessage1);
    mongo::BSONObj outputBSON1 = outputOpMsg1.body;
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
    mongo::Message deleteMessage = deleteOpMsg.serialize();

    void* output2;
    size_t outputSize2;
    err = libmongodbcapi_db_client_wire_protocol_rpc(
        client.get(), deleteMessage.buf(), deleteMessage.size(), &output2, &outputSize2);
    ASSERT_EQUALS(err, LIBMONGODB_CAPI_ERROR_SUCCESS);

    auto outputMessage2 = messageFromBuffer(output2, outputSize2);
    ASSERT(outputMessage2.size() > 0);
    ASSERT(outputMessage2.operation() == deleteMessage.operation());

    auto outputOpMsg2 = mongo::OpMsg::parseOwned(outputMessage2);
    mongo::BSONObj outputBSON2 = outputOpMsg2.body;
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
    mongo::Message insertMessage = insertOpMsg.serialize();

    void* output;
    size_t outputSize;
    int err = libmongodbcapi_db_client_wire_protocol_rpc(
        client.get(), insertMessage.buf(), insertMessage.size(), &output, &outputSize);
    ASSERT_EQUALS(err, LIBMONGODB_CAPI_ERROR_SUCCESS);

    auto outputMessage1 = messageFromBuffer(output, outputSize);
    ASSERT(outputMessage1.size() > 0);
    ASSERT(outputMessage1.operation() == insertMessage.operation());

    auto outputOpMsg1 = mongo::OpMsg::parseOwned(outputMessage1);
    mongo::BSONObj outputBSON1 = outputOpMsg1.body;
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
    mongo::Message updateMessage = updateOpMsg.serialize();

    void* output2;
    size_t outputSize2;
    err = libmongodbcapi_db_client_wire_protocol_rpc(
        client.get(), updateMessage.buf(), updateMessage.size(), &output2, &outputSize2);
    ASSERT_EQUALS(err, LIBMONGODB_CAPI_ERROR_SUCCESS);

    auto outputMessage2 = messageFromBuffer(output2, outputSize2);
    ASSERT(outputMessage2.size() > 0);
    ASSERT(outputMessage2.operation() == updateMessage.operation());

    auto outputOpMsg2 = mongo::OpMsg::parseOwned(outputMessage2);
    mongo::BSONObj outputBSON2 = outputOpMsg2.body;
    ASSERT(outputBSON2.valid(mongo::BSONVersion::kLatest));
    ASSERT(outputBSON2.hasField("ok"));
    ASSERT(outputBSON2.getField("ok").numberDouble() == 1.0);
    ASSERT(outputBSON2.hasField("nModified"));
    ASSERT(outputBSON2.getIntField("nModified") == 1);
}

// This test is temporary to make sure that only one database can be created
// This restriction may be relaxed at a later time
TEST_F(MongodbCAPITest, CreateMultipleDBs) {
    libmongodbcapi_db* db2 = libmongodbcapi_db_new(0, nullptr, nullptr);
    ASSERT(db2 == nullptr);
    ASSERT_EQUALS(libmongodbcapi_get_last_error(), LIBMONGODB_CAPI_ERROR_UNKNOWN);
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
