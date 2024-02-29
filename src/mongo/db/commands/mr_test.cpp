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

#include <cstdint>
#include <fmt/format.h>
#include <functional>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/map_reduce_out_options.h"
#include "mongo/db/commands/mr_common.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/protocol.h"
#include "mongo/scripting/dbdirectclient_factory.h"
#include "mongo/scripting/engine.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {
namespace {

/**
 * Helper function to verify field of map_reduce_common::OutputOptions.
 */
template <typename T>
void _compareOutputOptionField(const DatabaseName& dbName,
                               const std::string& cmdObjStr,
                               const std::string& fieldName,
                               const T& actual,
                               const T& expected) {
    if (actual == expected)
        return;
    FAIL(str::stream() << "parseOutputOptions(\"" << dbName.toStringForErrorMsg() << ", "
                       << cmdObjStr << "): " << fieldName << ": Expected: " << expected
                       << ". Actual: " << actual);
}

/**
 * Returns string representation of OutputType
 */
std::string _getOutTypeString(OutputType outType) {
    switch (outType) {
        case OutputType::Replace:
            return "REPLACE";
        case OutputType::Merge:
            return "MERGE";
        case OutputType::Reduce:
            return "REDUCE";
        case OutputType::InMemory:
            return "INMEMORY";
    }
    MONGO_UNREACHABLE;
}

/**
 * Test helper function to check expected result of parseOutputOptions.
 */
void _testConfigParseOutputOptions(const DatabaseName& dbName,
                                   const std::string& cmdObjStr,
                                   const std::string& expectedOutDb,
                                   const std::string& expectedCollectionName,
                                   const std::string& expectedFinalNamespace,
                                   bool expectedOutNonAtomic,
                                   OutputType expectedOutType) {
    const BSONObj cmdObj = fromjson(cmdObjStr);
    map_reduce_common::OutputOptions outputOptions =
        map_reduce_common::parseOutputOptions(dbName, cmdObj);
    _compareOutputOptionField(dbName, cmdObjStr, "outDb", outputOptions.outDB, expectedOutDb);
    _compareOutputOptionField(
        dbName, cmdObjStr, "collectionName", outputOptions.collectionName, expectedCollectionName);
    _compareOutputOptionField(dbName,
                              cmdObjStr,
                              "finalNamespace",
                              outputOptions.finalNamespace.toString_forTest(),
                              expectedFinalNamespace);
    _compareOutputOptionField(
        dbName, cmdObjStr, "outNonAtomic", outputOptions.outNonAtomic, expectedOutNonAtomic);
    _compareOutputOptionField(dbName,
                              cmdObjStr,
                              "outType",
                              _getOutTypeString(outputOptions.outType),
                              _getOutTypeString(expectedOutType));
}

/**
 * Tests for map_reduce_common::parseOutputOptions.
 */
TEST(ConfigOutputOptionsTest, parseOutputOptions) {
    DatabaseName dbName = DatabaseName::createDatabaseName_forTest(boost::none, "mydb");
    DatabaseName dbName1 = DatabaseName::createDatabaseName_forTest(boost::none, "mydb1");
    // Missing 'out' field.
    ASSERT_THROWS(map_reduce_common::parseOutputOptions(dbName, fromjson("{}")),
                  AssertionException);
    // 'out' must be either string or object.
    ASSERT_THROWS(map_reduce_common::parseOutputOptions(dbName, fromjson("{out: 99}")),
                  AssertionException);
    // 'out.nonAtomic' false is not supported.
    ASSERT_THROWS(map_reduce_common::parseOutputOptions(
                      dbName, fromjson("{out: {normal: 'mycoll', nonAtomic: false}}")),
                  AssertionException);
    ASSERT_THROWS(map_reduce_common::parseOutputOptions(
                      dbName, fromjson("{out: {replace: 'mycoll', nonAtomic: false}}")),
                  AssertionException);
    ASSERT_THROWS(map_reduce_common::parseOutputOptions(
                      dbName, fromjson("{out: {inline: 'mycoll', nonAtomic: false}}")),
                  AssertionException);

    // Unknown output specifer.
    ASSERT_THROWS(map_reduce_common::parseOutputOptions(
                      dbName, fromjson("{out: {no_such_out_type: 'mycoll'}}")),
                  AssertionException);

    // 'out' is string.
    _testConfigParseOutputOptions(
        dbName, "{out: 'mycoll'}", "", "mycoll", "mydb.mycoll", true, OutputType::Replace);
    // 'out' is object.
    _testConfigParseOutputOptions(dbName,
                                  "{out: {normal: 'mycoll'}}",
                                  "",
                                  "mycoll",
                                  "mydb.mycoll",
                                  true,
                                  OutputType::Replace);
    // 'out.db' overrides dbname parameter
    _testConfigParseOutputOptions(dbName1,
                                  "{out: {replace: 'mycoll', db: 'mydb2'}}",
                                  "mydb2",
                                  "mycoll",
                                  "mydb2.mycoll",
                                  true,
                                  OutputType::Replace);
    // 'out.nonAtomic' is supported with merge and reduce.
    _testConfigParseOutputOptions(dbName,
                                  "{out: {merge: 'mycoll', nonAtomic: true}}",
                                  "",
                                  "mycoll",
                                  "mydb.mycoll",
                                  true,
                                  OutputType::Merge);
    _testConfigParseOutputOptions(dbName,
                                  "{out: {reduce: 'mycoll', nonAtomic: true}}",
                                  "",
                                  "mycoll",
                                  "mydb.mycoll",
                                  true,
                                  OutputType::Reduce);
    // inline
    _testConfigParseOutputOptions(dbName1,
                                  "{out: {inline: 'mycoll', db: 'mydb2'}}",
                                  "mydb2",
                                  "",
                                  "",
                                  true,
                                  OutputType::InMemory);

    // Order should not matter in fields of 'out' object.
    _testConfigParseOutputOptions(dbName1,
                                  "{out: {db: 'mydb2', normal: 'mycoll'}}",
                                  "mydb2",
                                  "mycoll",
                                  "mydb2.mycoll",
                                  true,
                                  OutputType::Replace);
    _testConfigParseOutputOptions(dbName1,
                                  "{out: {db: 'mydb2', replace: 'mycoll'}}",
                                  "mydb2",
                                  "mycoll",
                                  "mydb2.mycoll",
                                  true,
                                  OutputType::Replace);
    _testConfigParseOutputOptions(dbName1,
                                  "{out: {nonAtomic: true, merge: 'mycoll'}}",
                                  "",
                                  "mycoll",
                                  "mydb1.mycoll",
                                  true,
                                  OutputType::Merge);
    _testConfigParseOutputOptions(dbName1,
                                  "{out: {nonAtomic: true, reduce: 'mycoll'}}",
                                  "",
                                  "mycoll",
                                  "mydb1.mycoll",
                                  true,
                                  OutputType::Reduce);
    _testConfigParseOutputOptions(dbName1,
                                  "{out: {db: 'mydb2', inline: 'mycoll'}}",
                                  "mydb2",
                                  "",
                                  "",
                                  true,
                                  OutputType::InMemory);
}

/**
 * OpObserver for mapReduce test fixture.
 */
class MapReduceOpObserver : public OpObserverNoop {
public:
    /**
     * This function is called whenever mapReduce copies indexes from an existing output collection
     * to a temporary collection.
     */
    void onCreateIndex(OperationContext* opCtx,
                       const NamespaceString& nss,
                       const UUID& uuid,
                       BSONObj indexDoc,
                       bool fromMigrate) override;

    /**
     * This function is called whenever mapReduce copies indexes from an existing output collection
     * to a temporary collection.
     */
    void onStartIndexBuild(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const UUID& collUUID,
                           const UUID& indexBuildUUID,
                           const std::vector<BSONObj>& indexes,
                           bool fromMigrate) override;

    /**
     * This function is called whenever mapReduce inserts documents into a temporary output
     * collection.
     */
    void onInserts(OperationContext* opCtx,
                   const CollectionPtr& coll,
                   std::vector<InsertStatement>::const_iterator begin,
                   std::vector<InsertStatement>::const_iterator end,
                   const std::vector<RecordId>& recordIds,
                   std::vector<bool> fromMigrate,
                   bool defaultFromMigrate,
                   OpStateAccumulator* opAccumulator = nullptr) override;

    /**
     * Tracks the temporary collections mapReduces creates.
     */
    void onCreateCollection(OperationContext* opCtx,
                            const CollectionPtr& coll,
                            const NamespaceString& collectionName,
                            const CollectionOptions& options,
                            const BSONObj& idIndex,
                            const OplogSlot& createOpTime,
                            bool fromMigrate) override;

    repl::OpTime onDropCollection(OperationContext* opCtx,
                                  const NamespaceString& collectionName,
                                  const UUID& uuid,
                                  std::uint64_t numRecords,
                                  CollectionDropType dropType,
                                  bool markFromMigrate) override;

    // Hook for onInserts. Defaults to a no-op function but may be overridden to inject exceptions
    // while mapReduce inserts its results into the temporary output collection.
    std::function<void()> onInsertsFn = [] {
    };

    // Holds indexes copied from existing output collection to the temporary collections.
    std::vector<BSONObj> indexesCreated;

    // Holds namespaces of temporary collections created by mapReduce.
    std::vector<NamespaceString> tempNamespaces;

    const repl::OpTime dropOpTime = {Timestamp(Seconds(100), 1U), 1LL};
};

void MapReduceOpObserver::onCreateIndex(OperationContext* opCtx,
                                        const NamespaceString& nss,
                                        const UUID& uuid,
                                        BSONObj indexDoc,
                                        bool fromMigrate) {
    indexesCreated.push_back(indexDoc.getOwned());
}

void MapReduceOpObserver::onStartIndexBuild(OperationContext* opCtx,
                                            const NamespaceString& nss,
                                            const UUID& collUUID,
                                            const UUID& indexBuildUUID,
                                            const std::vector<BSONObj>& indexes,
                                            bool fromMigrate) {
    for (auto&& obj : indexes) {
        indexesCreated.push_back(obj.getOwned());
    }
}

void MapReduceOpObserver::onInserts(OperationContext* opCtx,
                                    const CollectionPtr& coll,
                                    std::vector<InsertStatement>::const_iterator begin,
                                    std::vector<InsertStatement>::const_iterator end,
                                    const std::vector<RecordId>& recordIds,
                                    std::vector<bool> fromMigrate,
                                    bool defaultFromMigrate,
                                    OpStateAccumulator* opAccumulator) {
    onInsertsFn();
}

void MapReduceOpObserver::onCreateCollection(OperationContext*,
                                             const CollectionPtr&,
                                             const NamespaceString& collectionName,
                                             const CollectionOptions& options,
                                             const BSONObj&,
                                             const OplogSlot&,
                                             bool fromMigrate) {
    if (!options.temp) {
        return;
    }
    tempNamespaces.push_back(collectionName);
}

repl::OpTime MapReduceOpObserver::onDropCollection(OperationContext* opCtx,
                                                   const NamespaceString& collectionName,
                                                   const UUID& uuid,
                                                   std::uint64_t numRecords,
                                                   const CollectionDropType dropType,
                                                   bool markFromMigrate) {
    // If the oplog is not disabled for this namespace, then we need to reserve an op time for the
    // drop.
    if (!repl::ReplicationCoordinator::get(opCtx)->isOplogDisabledFor(opCtx, collectionName)) {
        OpObserver::Times::get(opCtx).reservedOpTimes.push_back(dropOpTime);
    }
    return {};
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

const NamespaceString MapReduceCommandTest::inputNss =
    NamespaceString::createNamespaceString_forTest("myDB.myCollection");
const NamespaceString MapReduceCommandTest::outputNss =
    NamespaceString::createNamespaceString_forTest(inputNss.getSisterNS("outCollection"));

void MapReduceCommandTest::setUp() {
    ServiceContextMongoDTest::setUp();
    ScriptEngine::setup(ExecutionEnvironment::Server);
    auto service = getServiceContext();
    DBDirectClientFactory::get(service).registerImplementation(
        [](OperationContext* opCtx) { return std::make_unique<DBDirectClient>(opCtx); });
    repl::ReplicationCoordinator::set(service,
                                      std::make_unique<repl::ReplicationCoordinatorMock>(service));

    repl::DropPendingCollectionReaper::set(
        service,
        std::make_unique<repl::DropPendingCollectionReaper>(repl::StorageInterface::get(service)));

    // Set up an OpObserver to track the temporary collections mapReduce creates.
    auto opObserver = std::make_unique<MapReduceOpObserver>();
    _opObserver = opObserver.get();
    auto opObserverRegistry = dynamic_cast<OpObserverRegistry*>(service->getOpObserver());
    opObserverRegistry->addObserver(std::move(opObserver));

    _opCtx = cc().makeOperationContext();

    // Transition to PRIMARY so that the server can accept writes.
    ASSERT_OK(_getReplCoord()->setFollowerMode(repl::MemberState::RS_PRIMARY));
    repl::createOplog(_opCtx.get());

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
    auto command = CommandHelpers::findCommand(_opCtx.get(), "mapReduce");
    ASSERT(command) << "Unable to look up mapReduce command";

    auto request = OpMsgRequestBuilder::createWithValidatedTenancyScope(
        inputNss.dbName(),
        auth::ValidatedTenancyScope::get(_opCtx.get()),
        _makeCmdObj(mapCode, reduceCode));
    auto replyBuilder = rpc::makeReplyBuilder(rpc::Protocol::kOpMsg);
    auto result = CommandHelpers::runCommandDirectly(_opCtx.get(), request);
    auto status = getStatusFromCommandResult(result);
    if (!status.isOK()) {
        return status.withContext(str::stream() << "mapReduce command failed: " << request.body);
    }

    _assertTemporaryCollectionsAreDropped();
    return Status::OK();
}

void MapReduceCommandTest::_assertTemporaryCollectionsAreDropped() {
    for (const auto& tempNss : _opObserver->tempNamespaces) {
        ASSERT_EQUALS(ErrorCodes::NamespaceNotFound,
                      _storage.getCollectionCount(_opCtx.get(), tempNss))
            << "mapReduce did not remove temporary collection on success: " << tempNss.ns_forTest();
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

    _opObserver->onInsertsFn = [] {
        uasserted(ErrorCodes::OperationFailed, "");
    };

    auto mapCode = "function() { emit(this._id, this._id); }"_sd;
    auto reduceCode = "function(k, v) { return Array.sum(v); }"_sd;
    ASSERT_EQ(_runCommand(mapCode, reduceCode), ErrorCodes::OperationFailed);

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
    ASSERT_EQ(_runCommand(mapCode, reduceCode), ErrorCodes::OperationFailed);

    // Temporary collections should still be present because the server will not accept writes after
    // stepping down.
    shard_role_details::getRecoveryUnit(_opCtx.get())
        ->setTimestampReadSource(RecoveryUnit::ReadSource::kLastApplied);
    for (const auto& tempNss : _opObserver->tempNamespaces) {
        ASSERT_OK(_storage.getCollectionCount(_opCtx.get(), tempNss).getStatus())
            << "missing mapReduce temporary collection: " << tempNss.toStringForErrorMsg();
    }
}

TEST_F(MapReduceCommandTest, ReplacingExistingOutputCollectionPreservesIndexes) {
    CollectionOptions options;
    options.uuid = UUID::gen();
    ASSERT_OK(_storage.createCollection(_opCtx.get(), outputNss, options));

    // Create an index in the existing output collection. This should be present in the recreated
    // output collection.
    auto indexSpec = BSON("v" << 2 << "key" << BSON("a" << 1) << "name"
                              << "a_1");
    {
        AutoGetCollection coll(_opCtx.get(), outputNss, MODE_X);
        ASSERT(coll);
        writeConflictRetry(
            _opCtx.get(), "ReplacingExistingOutputCollectionPreservesIndexes", outputNss, [&] {
                WriteUnitOfWork wuow(_opCtx.get());
                ASSERT_OK(
                    coll.getWritableCollection(_opCtx.get())
                        ->getIndexCatalog()
                        ->createIndexOnEmptyCollection(
                            _opCtx.get(), coll.getWritableCollection(_opCtx.get()), indexSpec));
                wuow.commit();
            });
    }

    auto sourceDoc = BSON("_id" << 0);
    ASSERT_OK(_storage.insertDocument(_opCtx.get(), inputNss, {sourceDoc, Timestamp(0)}, 1LL));

    auto mapCode = "function() { emit(this._id, this._id); }"_sd;
    auto reduceCode = "function(k, v) { return Array.sum(v); }"_sd;
    ASSERT_OK(_runCommand(mapCode, reduceCode));

    // MapReduce should filter existing indexes in the temporary collection, such as
    // the _id index.
    ASSERT_EQUALS(1U, _opObserver->indexesCreated.size())
        << BSON("indexesCreated" << _opObserver->indexesCreated);
    ASSERT_BSONOBJ_EQ(indexSpec, _opObserver->indexesCreated[0]);

    ASSERT_NOT_EQUALS(
        *options.uuid,
        *CollectionCatalog::get(_opCtx.get())->lookupUUIDByNSS(_opCtx.get(), outputNss))
        << "Output collection " << outputNss.toStringForErrorMsg() << " was not replaced";

    _assertTemporaryCollectionsAreDropped();
}

}  // namespace
}  // namespace mongo
