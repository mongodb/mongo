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

#include <memory>

#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/op_observer_noop.h"
#include "mongo/db/repl/apply_ops.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/logger/logger.h"
#include "mongo/rpc/get_status_from_command_result.h"

namespace mongo {
namespace repl {
namespace {

/**
 * Mock OpObserver that tracks applyOps events.
 */
class OpObserverMock : public OpObserverNoop {
public:
    /**
     * Called by applyOps() when ops are applied atomically.
     */
    void onApplyOps(OperationContext* opCtx,
                    const std::string& dbName,
                    const BSONObj& applyOpCmd) override;

    // If not empty, holds the command object passed to last invocation of onApplyOps().
    BSONObj onApplyOpsCmdObj;
};

void OpObserverMock::onApplyOps(OperationContext* opCtx,
                                const std::string& dbName,
                                const BSONObj& applyOpCmd) {
    ASSERT_FALSE(applyOpCmd.isEmpty());
    // Get owned copy because 'applyOpCmd' may be a temporary BSONObj created by applyOps().
    onApplyOpsCmdObj = applyOpCmd.getOwned();
}

/**
 * Test fixture for applyOps().
 */
class ApplyOpsTest : public ServiceContextMongoDTest {
private:
    void setUp() override;
    void tearDown() override;

protected:
    OpObserverMock* _opObserver = nullptr;
    std::unique_ptr<StorageInterface> _storage;
};

void ApplyOpsTest::setUp() {
    // Set up mongod.
    ServiceContextMongoDTest::setUp();

    auto service = getServiceContext();
    auto opCtx = cc().makeOperationContext();

    // Set up ReplicationCoordinator and create oplog.
    ReplicationCoordinator::set(service, std::make_unique<ReplicationCoordinatorMock>(service));
    setOplogCollectionName(service);
    createOplog(opCtx.get());

    // Ensure that we are primary.
    auto replCoord = ReplicationCoordinator::get(opCtx.get());
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_PRIMARY));

    // Use OpObserverMock to track notifications for applyOps().
    auto opObserver = std::make_unique<OpObserverMock>();
    _opObserver = opObserver.get();
    service->setOpObserver(std::move(opObserver));

    // This test uses StorageInterface to create collections and inspect documents inside
    // collections.
    _storage = std::make_unique<StorageInterfaceImpl>();
}

void ApplyOpsTest::tearDown() {
    _storage = {};
    _opObserver = nullptr;

    // Reset default log level in case it was changed.
    logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogComponent::kReplication,
                                                        logger::LogSeverity::Debug(0));

    ServiceContextMongoDTest::tearDown();
}

/**
 * Fixes up result document returned by applyOps and converts to Status.
 */
Status getStatusFromApplyOpsResult(const BSONObj& result) {
    if (result["ok"]) {
        return getStatusFromCommandResult(result);
    }

    BSONObjBuilder builder;
    builder.appendElements(result);
    auto code = result.getIntField("code");
    builder.appendIntOrLL("ok", code == 0);
    auto newResult = builder.obj();
    return getStatusFromCommandResult(newResult);
}

TEST_F(ApplyOpsTest, CommandInNestedApplyOpsReturnsSuccess) {
    auto opCtx = cc().makeOperationContext();
    auto mode = OplogApplication::Mode::kApplyOpsCmd;
    BSONObjBuilder resultBuilder;
    NamespaceString nss("test", "foo");
    auto innerCmdObj =
        BSON("op"
             << "c"
             << "ns" << nss.getCommandNS().ns() << "o" << BSON("create" << nss.coll()));
    auto innerApplyOpsObj = BSON("op"
                                 << "c"
                                 << "ns" << nss.getCommandNS().ns() << "o"
                                 << BSON("applyOps" << BSON_ARRAY(innerCmdObj)));
    auto cmdObj = BSON("applyOps" << BSON_ARRAY(innerApplyOpsObj));

    ASSERT_OK(applyOps(opCtx.get(), nss.db().toString(), cmdObj, mode, &resultBuilder));
    ASSERT_BSONOBJ_EQ({}, _opObserver->onApplyOpsCmdObj);
}

TEST_F(ApplyOpsTest, InsertInNestedApplyOpsReturnsSuccess) {
    auto opCtx = cc().makeOperationContext();
    auto mode = OplogApplication::Mode::kApplyOpsCmd;
    // Make sure the apply ops command object contains the correct UUID information.
    CollectionOptions options;
    options.uuid = UUID::gen();
    BSONObjBuilder resultBuilder;
    NamespaceString nss("test", "foo");
    auto innerCmdObj = BSON("op"
                            << "i"
                            << "ns" << nss.ns() << "o"
                            << BSON("_id"
                                    << "a")
                            << "ui" << options.uuid.get());
    auto innerApplyOpsObj = BSON("op"
                                 << "c"
                                 << "ns" << nss.getCommandNS().ns() << "o"
                                 << BSON("applyOps" << BSON_ARRAY(innerCmdObj)));
    auto cmdObj = BSON("applyOps" << BSON_ARRAY(innerApplyOpsObj));

    ASSERT_OK(_storage->createCollection(opCtx.get(), nss, options));
    ASSERT_OK(applyOps(opCtx.get(), nss.db().toString(), cmdObj, mode, &resultBuilder));
    ASSERT_BSONOBJ_EQ(BSON("applyOps" << BSON_ARRAY(innerCmdObj)), _opObserver->onApplyOpsCmdObj);
}

TEST_F(ApplyOpsTest, AtomicApplyOpsWithNoOpsReturnsSuccess) {
    auto opCtx = cc().makeOperationContext();
    auto mode = OplogApplication::Mode::kApplyOpsCmd;
    BSONObjBuilder resultBuilder;
    auto cmdObj = BSON("applyOps" << BSONArray());
    ASSERT_OK(applyOps(opCtx.get(), "test", cmdObj, mode, &resultBuilder));
    ASSERT_BSONOBJ_EQ(cmdObj, _opObserver->onApplyOpsCmdObj);
}

/**
 * Creates an applyOps command object with a single insert operation.
 */
BSONObj makeApplyOpsWithInsertOperation(const NamespaceString& nss,
                                        const OptionalCollectionUUID& uuid,
                                        const BSONObj& documentToInsert) {
    auto insertOp = uuid ? BSON("op"
                                << "i"
                                << "ns" << nss.ns() << "o" << documentToInsert << "ui" << *uuid)
                         : BSON("op"
                                << "i"
                                << "ns" << nss.ns() << "o" << documentToInsert);
    return BSON("applyOps" << BSON_ARRAY(insertOp));
}

TEST_F(ApplyOpsTest,
       AtomicApplyOpsInsertIntoNonexistentCollectionReturnsNamespaceNotFoundInResult) {
    auto opCtx = cc().makeOperationContext();
    auto mode = OplogApplication::Mode::kApplyOpsCmd;
    NamespaceString nss("test.t");
    auto documentToInsert = BSON("_id" << 0);
    auto cmdObj = makeApplyOpsWithInsertOperation(nss, boost::none, documentToInsert);
    BSONObjBuilder resultBuilder;
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound,
                  applyOps(opCtx.get(), "test", cmdObj, mode, &resultBuilder));
    auto result = resultBuilder.obj();
    auto status = getStatusFromApplyOpsResult(result);
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, status);
}


TEST_F(ApplyOpsTest, AtomicApplyOpsInsertWithUuidIntoCollectionWithUuid) {
    auto opCtx = cc().makeOperationContext();
    auto mode = OplogApplication::Mode::kApplyOpsCmd;
    NamespaceString nss("test.t");

    auto uuid = UUID::gen();

    CollectionOptions collectionOptions;
    collectionOptions.uuid = uuid;
    ASSERT_OK(_storage->createCollection(opCtx.get(), nss, collectionOptions));

    auto documentToInsert = BSON("_id" << 0);
    auto cmdObj = makeApplyOpsWithInsertOperation(nss, uuid, documentToInsert);
    BSONObjBuilder resultBuilder;
    ASSERT_OK(applyOps(opCtx.get(), "test", cmdObj, mode, &resultBuilder));
    ASSERT_BSONOBJ_EQ(cmdObj, _opObserver->onApplyOpsCmdObj);
}

TEST_F(ApplyOpsTest, AtomicApplyOpsInsertWithUuidIntoCollectionWithOtherUuid) {
    auto opCtx = cc().makeOperationContext();
    auto mode = OplogApplication::Mode::kApplyOpsCmd;
    NamespaceString nss("test.t");

    auto applyOpsUuid = UUID::gen();

    // Collection has a different UUID.
    CollectionOptions collectionOptions;
    collectionOptions.uuid = UUID::gen();
    ASSERT_NOT_EQUALS(applyOpsUuid, *collectionOptions.uuid);
    ASSERT_OK(_storage->createCollection(opCtx.get(), nss, collectionOptions));

    // The applyOps returns a NamespaceNotFound error because of the failed UUID lookup
    // even though a collection exists with the same namespace as the insert operation.
    auto documentToInsert = BSON("_id" << 0);
    auto cmdObj = makeApplyOpsWithInsertOperation(nss, applyOpsUuid, documentToInsert);
    BSONObjBuilder resultBuilder;
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound,
                  applyOps(opCtx.get(), "test", cmdObj, mode, &resultBuilder));
    auto result = resultBuilder.obj();
    auto status = getStatusFromApplyOpsResult(result);
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, status);
}

TEST_F(ApplyOpsTest, AtomicApplyOpsInsertWithoutUuidIntoCollectionWithUuid) {
    auto opCtx = cc().makeOperationContext();
    auto mode = OplogApplication::Mode::kApplyOpsCmd;
    NamespaceString nss("test.t");

    auto uuid = UUID::gen();

    CollectionOptions collectionOptions;
    collectionOptions.uuid = uuid;
    ASSERT_OK(_storage->createCollection(opCtx.get(), nss, collectionOptions));

    auto documentToInsert = BSON("_id" << 0);
    auto cmdObj = makeApplyOpsWithInsertOperation(nss, boost::none, documentToInsert);
    BSONObjBuilder resultBuilder;
    ASSERT_OK(applyOps(opCtx.get(), "test", cmdObj, mode, &resultBuilder));

    // Insert operation provided by caller did not contain collection uuid but applyOps() should add
    // the uuid to the oplog entry.
    auto expectedCmdObj = makeApplyOpsWithInsertOperation(nss, uuid, documentToInsert);
    ASSERT_BSONOBJ_EQ(expectedCmdObj, _opObserver->onApplyOpsCmdObj);
}

TEST_F(ApplyOpsTest, ApplyOpsPropagatesOplogApplicationMode) {
    auto opCtx = cc().makeOperationContext();

    // Increase log component verbosity to check for op application messages.
    logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogComponent::kReplication,
                                                        logger::LogSeverity::Debug(3));

    // Test that the 'applyOps' function passes the oplog application mode through correctly to the
    // underlying op application functions.
    NamespaceString nss("test.coll");
    auto uuid = UUID::gen();

    // Create a collection for us to insert documents into.
    CollectionOptions collectionOptions;
    collectionOptions.uuid = uuid;
    ASSERT_OK(_storage->createCollection(opCtx.get(), nss, collectionOptions));

    BSONObjBuilder resultBuilder;

    // Make sure the oplog application mode is passed through via 'applyOps' correctly.
    startCapturingLogMessages();

    auto docToInsert0 = BSON("_id" << 0);
    auto cmdObj = makeApplyOpsWithInsertOperation(nss, uuid, docToInsert0);

    ASSERT_OK(applyOps(opCtx.get(),
                       nss.coll().toString(),
                       cmdObj,
                       OplogApplication::Mode::kInitialSync,
                       &resultBuilder));
    ASSERT_EQUALS(1, countLogLinesContaining("oplog application mode: InitialSync"));

    auto docToInsert1 = BSON("_id" << 1);
    cmdObj = makeApplyOpsWithInsertOperation(nss, uuid, docToInsert1);

    ASSERT_OK(applyOps(opCtx.get(),
                       nss.coll().toString(),
                       cmdObj,
                       OplogApplication::Mode::kSecondary,
                       &resultBuilder));
    ASSERT_EQUALS(1, countLogLinesContaining("oplog application mode: Secondary"));

    stopCapturingLogMessages();
}

/**
 * Generates oplog entries with the given number used for the timestamp.
 */
OplogEntry makeOplogEntry(OpTypeEnum opType, const BSONObj& oField) {
    return OplogEntry(OpTime(Timestamp(1, 1), 1),  // optime
                      boost::none,                 // hash
                      opType,                      // op type
                      NamespaceString("a.a"),      // namespace
                      boost::none,                 // uuid
                      boost::none,                 // fromMigrate
                      OplogEntry::kOplogVersion,   // version
                      oField,                      // o
                      boost::none,                 // o2
                      {},                          // sessionInfo
                      boost::none,                 // upsert
                      boost::none,                 // wall clock time
                      boost::none,                 // statement id
                      boost::none,   // optime of previous write within same transaction
                      boost::none,   // pre-image optime
                      boost::none);  // post-image optime
}

TEST_F(ApplyOpsTest, ExtractOperationsReturnsTypeMismatchIfNotCommand) {
    ASSERT_THROWS_CODE(
        ApplyOps::extractOperations(makeOplogEntry(OpTypeEnum::kInsert, BSON("_id" << 0))),
        DBException,
        ErrorCodes::TypeMismatch);
}

TEST_F(ApplyOpsTest, ExtractOperationsReturnsCommandNotSupportedIfNotApplyOpsCommand) {
    ASSERT_THROWS_CODE(ApplyOps::extractOperations(makeOplogEntry(OpTypeEnum::kCommand,
                                                                  BSON("create"
                                                                       << "t"))),
                       DBException,
                       ErrorCodes::CommandNotSupported);
}

TEST_F(ApplyOpsTest, ExtractOperationsReturnsEmptyArrayIfApplyOpsContainsNoOperations) {
    auto operations = ApplyOps::extractOperations(
        makeOplogEntry(OpTypeEnum::kCommand, BSON("applyOps" << BSONArray())));
    ASSERT_EQUALS(0U, operations.size());
}

TEST_F(ApplyOpsTest, ExtractOperationsReturnsOperationsWithSameOpTimeAsApplyOps) {
    NamespaceString ns1("test.a");
    auto ui1 = UUID::gen();
    auto op1 = BSON("op"
                    << "i"
                    << "ns" << ns1.ns() << "ui" << ui1 << "o" << BSON("_id" << 1));

    NamespaceString ns2("test.b");
    auto ui2 = UUID::gen();
    auto op2 = BSON("op"
                    << "i"
                    << "ns" << ns2.ns() << "ui" << ui2 << "o" << BSON("_id" << 2));

    NamespaceString ns3("test.c");
    auto ui3 = UUID::gen();
    auto op3 = BSON("op"
                    << "u"
                    << "ns" << ns3.ns() << "ui" << ui3 << "b" << true << "o" << BSON("x" << 1)
                    << "o2" << BSON("_id" << 3));

    auto oplogEntry =
        makeOplogEntry(OpTypeEnum::kCommand, BSON("applyOps" << BSON_ARRAY(op1 << op2 << op3)));

    auto operations = ApplyOps::extractOperations(oplogEntry);
    ASSERT_EQUALS(3U, operations.size())
        << "Unexpected number of operations extracted: " << oplogEntry.toBSON();

    // Check extracted CRUD operations.
    auto it = operations.cbegin();
    {
        ASSERT(operations.cend() != it);
        const auto& operation1 = *(it++);
        ASSERT(OpTypeEnum::kInsert == operation1.getOpType())
            << "Unexpected op type: " << operation1.toBSON();
        ASSERT_EQUALS(ui1, *operation1.getUuid());
        ASSERT_EQUALS(ns1, operation1.getNss());
        ASSERT_BSONOBJ_EQ(BSON("_id" << 1), operation1.getOperationToApply());

        // OpTime of CRUD operation should match applyOps.
        ASSERT_EQUALS(oplogEntry.getOpTime(), operation1.getOpTime());
    }

    {
        ASSERT(operations.cend() != it);
        const auto& operation2 = *(it++);
        ASSERT(OpTypeEnum::kInsert == operation2.getOpType())
            << "Unexpected op type: " << operation2.toBSON();
        ASSERT_EQUALS(ui2, *operation2.getUuid());
        ASSERT_EQUALS(ns2, operation2.getNss());
        ASSERT_BSONOBJ_EQ(BSON("_id" << 2), operation2.getOperationToApply());

        // OpTime of CRUD operation should match applyOps.
        ASSERT_EQUALS(oplogEntry.getOpTime(), operation2.getOpTime());
    }

    {
        ASSERT(operations.cend() != it);
        const auto& operation3 = *(it++);
        ASSERT(OpTypeEnum::kUpdate == operation3.getOpType())
            << "Unexpected op type: " << operation3.toBSON();
        ASSERT_EQUALS(ui3, *operation3.getUuid());
        ASSERT_EQUALS(ns3, operation3.getNss());
        ASSERT_BSONOBJ_EQ(BSON("x" << 1), operation3.getOperationToApply());

        auto optionalUpsertBool = operation3.getUpsert();
        ASSERT(optionalUpsertBool);
        ASSERT(*optionalUpsertBool);

        // OpTime of CRUD operation should match applyOps.
        ASSERT_EQUALS(oplogEntry.getOpTime(), operation3.getOpTime());
    }

    ASSERT(operations.cend() == it);
}

TEST_F(ApplyOpsTest, ApplyOpsFailsToDropAdmin) {
    auto opCtx = cc().makeOperationContext();
    auto mode = OplogApplication::Mode::kApplyOpsCmd;

    // Create a collection on the admin database.
    NamespaceString nss("admin.foo");
    CollectionOptions options;
    options.uuid = UUID::gen();
    ASSERT_OK(_storage->createCollection(opCtx.get(), nss, options));

    auto dropDatabaseOp = BSON("op"
                               << "c"
                               << "ns" << nss.getCommandNS().ns() << "o"
                               << BSON("dropDatabase" << 1));

    auto dropDatabaseCmdObj = BSON("applyOps" << BSON_ARRAY(dropDatabaseOp));
    BSONObjBuilder resultBuilder;
    auto status =
        applyOps(opCtx.get(), nss.db().toString(), dropDatabaseCmdObj, mode, &resultBuilder);
    ASSERT_EQUALS(ErrorCodes::IllegalOperation, status);
}

}  // namespace
}  // namespace repl
}  // namespace mongo
