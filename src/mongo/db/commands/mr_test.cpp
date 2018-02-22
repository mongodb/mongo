/**
 *    Copyright (C) 2014 MongoDB Inc.
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

/**
 * This file contains tests for mongo/db/commands/mr.h
 */

#include "mongo/db/commands/mr.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/json.h"
#include "mongo/db/op_observer_noop.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/scripting/dbdirectclient_factory.h"
#include "mongo/scripting/engine.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

/**
 * Tests for mr::Config
 */

/**
 * Helper function to verify field of mr::Config::OutputOptions.
 */
template <typename T>
void _compareOutputOptionField(const std::string& dbname,
                               const std::string& cmdObjStr,
                               const std::string& fieldName,
                               const T& actual,
                               const T& expected) {
    if (actual == expected)
        return;
    FAIL(str::stream() << "parseOutputOptions(\"" << dbname << ", " << cmdObjStr << "): "
                       << fieldName
                       << ": Expected: "
                       << expected
                       << ". Actual: "
                       << actual);
}

/**
 * Returns string representation of mr::Config::OutputType
 */
std::string _getOutTypeString(mr::Config::OutputType outType) {
    switch (outType) {
        case mr::Config::REPLACE:
            return "REPLACE";
        case mr::Config::MERGE:
            return "MERGE";
        case mr::Config::REDUCE:
            return "REDUCE";
        case mr::Config::INMEMORY:
            return "INMEMORY";
    }
    invariant(0);
}

/**
 * Test helper function to check expected result of parseOutputOptions.
 */
void _testConfigParseOutputOptions(const std::string& dbname,
                                   const std::string& cmdObjStr,
                                   const std::string& expectedOutDb,
                                   const std::string& expectedCollectionName,
                                   const std::string& expectedFinalNamespace,
                                   bool expectedOutNonAtomic,
                                   mr::Config::OutputType expectedOutType) {
    const BSONObj cmdObj = fromjson(cmdObjStr);
    mr::Config::OutputOptions outputOptions = mr::Config::parseOutputOptions(dbname, cmdObj);
    _compareOutputOptionField(dbname, cmdObjStr, "outDb", outputOptions.outDB, expectedOutDb);
    _compareOutputOptionField(
        dbname, cmdObjStr, "collectionName", outputOptions.collectionName, expectedCollectionName);
    _compareOutputOptionField(dbname,
                              cmdObjStr,
                              "finalNamespace",
                              outputOptions.finalNamespace.ns(),
                              expectedFinalNamespace);
    _compareOutputOptionField(
        dbname, cmdObjStr, "outNonAtomic", outputOptions.outNonAtomic, expectedOutNonAtomic);
    _compareOutputOptionField(dbname,
                              cmdObjStr,
                              "outType",
                              _getOutTypeString(outputOptions.outType),
                              _getOutTypeString(expectedOutType));
}

/**
 * Tests for mr::Config::parseOutputOptions.
 */
TEST(ConfigOutputOptionsTest, parseOutputOptions) {
    // Missing 'out' field.
    ASSERT_THROWS(mr::Config::parseOutputOptions("mydb", fromjson("{}")), AssertionException);
    // 'out' must be either string or object.
    ASSERT_THROWS(mr::Config::parseOutputOptions("mydb", fromjson("{out: 99}")),
                  AssertionException);
    // 'out.nonAtomic' is not supported with normal, replace or inline.
    ASSERT_THROWS(mr::Config::parseOutputOptions(
                      "mydb", fromjson("{out: {normal: 'mycoll', nonAtomic: true}}")),
                  AssertionException);
    ASSERT_THROWS(mr::Config::parseOutputOptions(
                      "mydb", fromjson("{out: {replace: 'mycoll', nonAtomic: true}}")),
                  AssertionException);
    ASSERT_THROWS(mr::Config::parseOutputOptions(
                      "mydb", fromjson("{out: {inline: 'mycoll', nonAtomic: true}}")),
                  AssertionException);
    // Unknown output specifer.
    ASSERT_THROWS(
        mr::Config::parseOutputOptions("mydb", fromjson("{out: {no_such_out_type: 'mycoll'}}")),
        AssertionException);


    // 'out' is string.
    _testConfigParseOutputOptions(
        "mydb", "{out: 'mycoll'}", "", "mycoll", "mydb.mycoll", false, mr::Config::REPLACE);
    // 'out' is object.
    _testConfigParseOutputOptions("mydb",
                                  "{out: {normal: 'mycoll'}}",
                                  "",
                                  "mycoll",
                                  "mydb.mycoll",
                                  false,
                                  mr::Config::REPLACE);
    // 'out.db' overrides dbname parameter
    _testConfigParseOutputOptions("mydb1",
                                  "{out: {replace: 'mycoll', db: 'mydb2'}}",
                                  "mydb2",
                                  "mycoll",
                                  "mydb2.mycoll",
                                  false,
                                  mr::Config::REPLACE);
    // 'out.nonAtomic' is supported with merge and reduce.
    _testConfigParseOutputOptions("mydb",
                                  "{out: {merge: 'mycoll', nonAtomic: true}}",
                                  "",
                                  "mycoll",
                                  "mydb.mycoll",
                                  true,
                                  mr::Config::MERGE);
    _testConfigParseOutputOptions("mydb",
                                  "{out: {reduce: 'mycoll', nonAtomic: true}}",
                                  "",
                                  "mycoll",
                                  "mydb.mycoll",
                                  true,
                                  mr::Config::REDUCE);
    // inline
    _testConfigParseOutputOptions("mydb1",
                                  "{out: {inline: 'mycoll', db: 'mydb2'}}",
                                  "mydb2",
                                  "",
                                  "",
                                  false,
                                  mr::Config::INMEMORY);

    // Order should not matter in fields of 'out' object.
    _testConfigParseOutputOptions("mydb1",
                                  "{out: {db: 'mydb2', normal: 'mycoll'}}",
                                  "mydb2",
                                  "mycoll",
                                  "mydb2.mycoll",
                                  false,
                                  mr::Config::REPLACE);
    _testConfigParseOutputOptions("mydb1",
                                  "{out: {db: 'mydb2', replace: 'mycoll'}}",
                                  "mydb2",
                                  "mycoll",
                                  "mydb2.mycoll",
                                  false,
                                  mr::Config::REPLACE);
    _testConfigParseOutputOptions("mydb1",
                                  "{out: {nonAtomic: true, merge: 'mycoll'}}",
                                  "",
                                  "mycoll",
                                  "mydb1.mycoll",
                                  true,
                                  mr::Config::MERGE);
    _testConfigParseOutputOptions("mydb1",
                                  "{out: {nonAtomic: true, reduce: 'mycoll'}}",
                                  "",
                                  "mycoll",
                                  "mydb1.mycoll",
                                  true,
                                  mr::Config::REDUCE);
    _testConfigParseOutputOptions("mydb1",
                                  "{out: {db: 'mydb2', inline: 'mycoll'}}",
                                  "mydb2",
                                  "",
                                  "",
                                  false,
                                  mr::Config::INMEMORY);
}

TEST(ConfigTest, ParseCollation) {
    std::string dbname = "myDB";
    BSONObj collation = BSON("locale"
                             << "en_US");
    BSONObjBuilder bob;
    bob.append("mapReduce", "myCollection");
    bob.appendCode("map", "function() { emit(0, 1); }");
    bob.appendCode("reduce", "function(k, v) { return {count: 0}; }");
    bob.append("out", "outCollection");
    bob.append("collation", collation);
    BSONObj cmdObj = bob.obj();
    mr::Config config(dbname, cmdObj);
    ASSERT_BSONOBJ_EQ(config.collation, collation);
}

TEST(ConfigTest, ParseNoCollation) {
    std::string dbname = "myDB";
    BSONObjBuilder bob;
    bob.append("mapReduce", "myCollection");
    bob.appendCode("map", "function() { emit(0, 1); }");
    bob.appendCode("reduce", "function(k, v) { return {count: 0}; }");
    bob.append("out", "outCollection");
    BSONObj cmdObj = bob.obj();
    mr::Config config(dbname, cmdObj);
    ASSERT_BSONOBJ_EQ(config.collation, BSONObj());
}

TEST(ConfigTest, CollationNotAnObjectFailsToParse) {
    std::string dbname = "myDB";
    BSONObjBuilder bob;
    bob.append("mapReduce", "myCollection");
    bob.appendCode("map", "function() { emit(0, 1); }");
    bob.appendCode("reduce", "function(k, v) { return {count: 0}; }");
    bob.append("out", "outCollection");
    bob.append("collation", "en_US");
    BSONObj cmdObj = bob.obj();
    ASSERT_THROWS(mr::Config(dbname, cmdObj), AssertionException);
}

/**
 * OpObserver for mapReduce test fixture.
 */
class MapReduceOpObserver : public OpObserverNoop {
public:
    /**
     * This function is called whenever mapReduce inserts documents into a temporary output
     * collection.
     */
    void onInserts(OperationContext* opCtx,
                   const NamespaceString& nss,
                   OptionalCollectionUUID uuid,
                   std::vector<InsertStatement>::const_iterator begin,
                   std::vector<InsertStatement>::const_iterator end,
                   bool fromMigrate) override;

    /**
     * Tracks the temporary collections mapReduces creates.
     */
    void onCreateCollection(OperationContext* opCtx,
                            Collection* coll,
                            const NamespaceString& collectionName,
                            const CollectionOptions& options,
                            const BSONObj& idIndex,
                            const OplogSlot& createOpTime) override;

    // Hook for onInserts. Defaults to a no-op function but may be overridden to inject exceptions
    // while mapReduce inserts its results into the temporary output collection.
    std::function<void()> onInsertsFn = [] {};

    // Holds namespaces of temporary collections created by mapReduce.
    std::vector<NamespaceString> tempNamespaces;
};

void MapReduceOpObserver::onInserts(OperationContext* opCtx,
                                    const NamespaceString& nss,
                                    OptionalCollectionUUID uuid,
                                    std::vector<InsertStatement>::const_iterator begin,
                                    std::vector<InsertStatement>::const_iterator end,
                                    bool fromMigrate) {
    onInsertsFn();
}

void MapReduceOpObserver::onCreateCollection(OperationContext*,
                                             Collection*,
                                             const NamespaceString& collectionName,
                                             const CollectionOptions& options,
                                             const BSONObj&,
                                             const OplogSlot&) {
    if (!options.temp) {
        return;
    }
    tempNamespaces.push_back(collectionName);
}

/**
 * Test fixture for MapReduceCommand.
 */
class MapReduceCommandTest : public ServiceContextMongoDTest {
public:
    static const NamespaceString inputNss;
    static const NamespaceString outputNss;

private:
    void setUp() override;
    void tearDown() override;

protected:
    /**
     * Looks up the current ReplicationCoordinator.
     * The result is cast to a ReplicationCoordinatorMock to provide access to test features.
     */
    repl::ReplicationCoordinatorMock* _getReplCoord() const;

    /**
     * Creates a mapReduce command object that reads from 'inputNss' and writes results to
     * 'outputNss'.
     */
    BSONObj _makeCmdObj(StringData mapCode, StringData reduceCode);

    /**
     * Runs a mapReduce command.
     * Ensures that temporary collections created by mapReduce no longer exist on success.
     */
    Status _runCommand(StringData mapCode, StringData reduceCode);

    /**
     * Checks that temporary collections created during mapReduce have been dropped.
     * This is made a separate test helper to handle cases where mapReduce is unable to remove
     * its temporary collections.
     */
    void _assertTemporaryCollectionsAreDropped();

    ServiceContext::UniqueOperationContext _opCtx;
    repl::StorageInterfaceImpl _storage;
    MapReduceOpObserver* _opObserver = nullptr;
};

const NamespaceString MapReduceCommandTest::inputNss("myDB.myCollection");
const NamespaceString MapReduceCommandTest::outputNss(inputNss.getSisterNS("outCollection"));

void MapReduceCommandTest::setUp() {
    ServiceContextMongoDTest::setUp();
    ScriptEngine::setup();
    auto service = getServiceContext();
    DBDirectClientFactory::get(service).registerImplementation(
        [](OperationContext* opCtx) { return std::make_unique<DBDirectClient>(opCtx); });
    repl::ReplicationCoordinator::set(service,
                                      std::make_unique<repl::ReplicationCoordinatorMock>(service));

    // Set up an OpObserver to track the temporary collections mapReduce creates.
    auto opObserver = std::make_unique<MapReduceOpObserver>();
    _opObserver = opObserver.get();
    service->setOpObserver(std::move(opObserver));

    _opCtx = cc().makeOperationContext();

    // Transition to PRIMARY so that the server can accept writes.
    ASSERT_OK(_getReplCoord()->setFollowerMode(repl::MemberState::RS_PRIMARY));

    // Create collection with one document.
    CollectionOptions collectionOptions;
    collectionOptions.uuid = UUID::gen();
    ASSERT_OK(_storage.createCollection(_opCtx.get(), inputNss, collectionOptions));
}

void MapReduceCommandTest::tearDown() {
    _opCtx = {};
    _opObserver = nullptr;
    ScriptEngine::dropScopeCache();
    ServiceContextMongoDTest::tearDown();
}

repl::ReplicationCoordinatorMock* MapReduceCommandTest::_getReplCoord() const {
    auto replCoord = repl::ReplicationCoordinator::get(_opCtx.get());
    ASSERT(replCoord) << "No ReplicationCoordinator installed";
    auto replCoordMock = dynamic_cast<repl::ReplicationCoordinatorMock*>(replCoord);
    ASSERT(replCoordMock) << "Unexpected type for installed ReplicationCoordinator";
    return replCoordMock;
}

BSONObj MapReduceCommandTest::_makeCmdObj(StringData mapCode, StringData reduceCode) {
    BSONObjBuilder bob;
    bob.append("mapReduce", inputNss.coll());
    bob.appendCode("map", mapCode);
    bob.appendCode("reduce", reduceCode);
    bob.append("out", outputNss.coll());
    return bob.obj();
}

Status MapReduceCommandTest::_runCommand(StringData mapCode, StringData reduceCode) {
    auto command = Command::findCommand("mapReduce");
    ASSERT(command) << "Unable to look up mapReduce command";

    auto request = OpMsgRequest::fromDBAndBody(inputNss.db(), _makeCmdObj(mapCode, reduceCode));
    BSONObjBuilder result;
    auto success = command->publicRun(_opCtx.get(), request, result);
    if (!success) {
        auto status = getStatusFromCommandResult(result.obj());
        ASSERT_NOT_OK(status);
        return status.withContext(str::stream() << "mapReduce command failed: " << request.body);
    }

    _assertTemporaryCollectionsAreDropped();
    return Status::OK();
}

void MapReduceCommandTest::_assertTemporaryCollectionsAreDropped() {
    for (const auto& tempNss : _opObserver->tempNamespaces) {
        ASSERT_EQUALS(ErrorCodes::NamespaceNotFound,
                      _storage.getCollectionCount(_opCtx.get(), tempNss))
            << "mapReduce did not remove temporary collection on success: " << tempNss.ns();
    }
}

TEST_F(MapReduceCommandTest, MapIdToValue) {
    auto sourceDoc = BSON("_id" << 0);
    ASSERT_OK(_storage.insertDocument(_opCtx.get(), inputNss, {sourceDoc, Timestamp(0)}, 1LL));

    auto mapCode = "function() { emit(this._id, this._id); }"_sd;
    auto reduceCode = "function(k, v) { return Array.sum(v); }"_sd;
    ASSERT_OK(_runCommand(mapCode, reduceCode));

    auto targetDoc = BSON("_id" << 0 << "value" << 0);
    ASSERT_BSONOBJ_EQ(targetDoc,
                      unittest::assertGet(_storage.findSingleton(_opCtx.get(), outputNss)));
}

TEST_F(MapReduceCommandTest, DropTemporaryCollectionsOnInsertError) {
    auto sourceDoc = BSON("_id" << 0);
    ASSERT_OK(_storage.insertDocument(_opCtx.get(), inputNss, {sourceDoc, Timestamp(0)}, 1LL));

    _opObserver->onInsertsFn = [] { uasserted(ErrorCodes::OperationFailed, ""); };

    auto mapCode = "function() { emit(this._id, this._id); }"_sd;
    auto reduceCode = "function(k, v) { return Array.sum(v); }"_sd;
    ASSERT_THROWS_CODE(
        _runCommand(mapCode, reduceCode).ignore(), AssertionException, ErrorCodes::OperationFailed);

    // Temporary collections created by mapReduce will be removed on failure if the server is able
    // to accept writes.
    _assertTemporaryCollectionsAreDropped();
}

TEST_F(MapReduceCommandTest, PrimaryStepDownPreventsTemporaryCollectionDrops) {
    auto sourceDoc = BSON("_id" << 0);
    ASSERT_OK(_storage.insertDocument(_opCtx.get(), inputNss, {sourceDoc, Timestamp(0)}, 1LL));

    _opObserver->onInsertsFn = [this] {
        ASSERT_OK(_getReplCoord()->setFollowerMode(repl::MemberState::RS_SECONDARY));
        uasserted(ErrorCodes::OperationFailed, "");
    };

    auto mapCode = "function() { emit(this._id, this._id); }"_sd;
    auto reduceCode = "function(k, v) { return Array.sum(v); }"_sd;
    ASSERT_THROWS_CODE(
        _runCommand(mapCode, reduceCode).ignore(), AssertionException, ErrorCodes::OperationFailed);

    // Temporary collections should still be present because the server will not accept writes after
    // stepping down.
    for (const auto& tempNss : _opObserver->tempNamespaces) {
        ASSERT_OK(_storage.getCollectionCount(_opCtx.get(), tempNss).getStatus())
            << "missing mapReduce temporary collection: " << tempNss;
    }
}

}  // namespace
}  // namespace mongo
