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

#include "mongo/db/repl/apply_ops.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/repl/apply_ops_command_info.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/tenant_id.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <ostream>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace repl {
namespace {

/**
 * Mock OpObserver that tracks applyOps events.
 */
class OpObserverMock : public OpObserverNoop {
public:
    // If not empty, holds the command object passed to last invocation of onApplyOps().
    BSONObj onApplyOpsCmdObj;
};

/**
 * Test fixture for applyOps().
 */
class ApplyOpsTest : public ServiceContextMongoDTest {
private:
    void setUp() override;
    void tearDown() override;

protected:
    // Reset default log level when each test is over in case it was changed.
    unittest::MinimumLoggedSeverityGuard _verbosityGuard{logv2::LogComponent::kReplication};

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
    createOplog(opCtx.get());

    // Ensure that we are primary.
    auto replCoord = ReplicationCoordinator::get(opCtx.get());
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_PRIMARY));

    // Use OpObserverMock to track notifications for applyOps().
    auto opObserver = std::make_unique<OpObserverMock>();
    _opObserver = opObserver.get();
    opObserverRegistry()->addObserver(std::move(opObserver));

    // This test uses StorageInterface to create collections and inspect documents inside
    // collections.
    _storage = std::make_unique<StorageInterfaceImpl>();
}

void ApplyOpsTest::tearDown() {
    _storage = {};
    _opObserver = nullptr;

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
    builder.appendNumber("ok", code == 0);
    auto newResult = builder.obj();
    return getStatusFromCommandResult(newResult);
}

TEST_F(ApplyOpsTest, CommandInNestedApplyOpsReturnsSuccess) {
    auto opCtx = cc().makeOperationContext();
    auto mode = OplogApplication::Mode::kApplyOpsCmd;
    BSONObjBuilder resultBuilder;
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "foo");
    auto innerCmdObj = BSON("op" << "c"
                                 << "ns" << nss.getCommandNS().ns_forTest() << "o"
                                 << BSON("create" << nss.coll()));
    auto innerApplyOpsObj = BSON("op" << "c"
                                      << "ns" << nss.getCommandNS().ns_forTest() << "o"
                                      << BSON("applyOps" << BSON_ARRAY(innerCmdObj)));
    auto cmdObj = BSON("applyOps" << BSON_ARRAY(innerApplyOpsObj));

    ASSERT_OK(applyOps(opCtx.get(), nss.dbName(), cmdObj, mode, &resultBuilder));
}

/**
 * Creates an applyOps command object with a single insert operation.
 */
BSONObj makeApplyOpsWithInsertOperation(const NamespaceString& nss,
                                        const boost::optional<UUID>& uuid,
                                        const BSONObj& documentToInsert) {
    auto insertOp = uuid
        ? BSON("op" << "i"
                    << "ns" << nss.ns_forTest() << "o" << documentToInsert << "ui" << *uuid)
        : BSON("op" << "i"
                    << "ns" << nss.ns_forTest() << "o" << documentToInsert);
    return BSON("applyOps" << BSON_ARRAY(insertOp));
}

TEST_F(ApplyOpsTest, ApplyOpsInsertIntoNonexistentCollectionReturnsNamespaceNotFoundInResult) {
    auto opCtx = cc().makeOperationContext();
    auto mode = OplogApplication::Mode::kApplyOpsCmd;
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    auto documentToInsert = BSON("_id" << 0);
    auto cmdObj = makeApplyOpsWithInsertOperation(nss, boost::none, documentToInsert);
    BSONObjBuilder resultBuilder;
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound,
                  applyOps(opCtx.get(),
                           DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                           cmdObj,
                           mode,
                           &resultBuilder));
    auto result = resultBuilder.obj();
    auto status = getStatusFromApplyOpsResult(result);
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, status);
}

TEST_F(ApplyOpsTest, ApplyOpsInsertWithUuidIntoCollectionWithOtherUuid) {
    auto opCtx = cc().makeOperationContext();
    auto mode = OplogApplication::Mode::kApplyOpsCmd;
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");

    auto applyOpsUuid = UUID::gen();

    // Collection has a different UUID.
    CollectionOptions collectionOptions;
    collectionOptions.uuid = UUID::gen();
    ASSERT_NOT_EQUALS(applyOpsUuid, *collectionOptions.uuid);
    ASSERT_OK(_storage->createCollection(opCtx.get(), nss, collectionOptions));

    // The applyOps returns an Unknown error because of the failed UUID lookup
    // even though a collection exists with the same namespace as the insert operation.
    auto documentToInsert = BSON("_id" << 0);
    auto cmdObj = makeApplyOpsWithInsertOperation(nss, applyOpsUuid, documentToInsert);
    BSONObjBuilder resultBuilder;
    ASSERT_EQUALS(ErrorCodes::UnknownError,
                  applyOps(opCtx.get(),
                           DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                           cmdObj,
                           mode,
                           &resultBuilder));
}

TEST_F(ApplyOpsTest, ApplyOpsPropagatesOplogApplicationMode) {
    auto opCtx = cc().makeOperationContext();

    // Increase log component verbosity to check for op application messages.
    auto verbosityGuard = unittest::MinimumLoggedSeverityGuard{logv2::LogComponent::kReplication,
                                                               logv2::LogSeverity::Debug(3)};

    // Test that the 'applyOps' function passes the oplog application mode through correctly to the
    // underlying op application functions.
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.coll");
    auto uuid = UUID::gen();

    // Create a collection for us to insert documents into.
    CollectionOptions collectionOptions;
    collectionOptions.uuid = uuid;
    ASSERT_OK(_storage->createCollection(opCtx.get(), nss, collectionOptions));

    BSONObjBuilder resultBuilder;

    // Make sure the oplog application mode is passed through via 'applyOps' correctly.
    unittest::LogCaptureGuard logs;

    auto docToInsert0 = BSON("_id" << 0);
    auto cmdObj = makeApplyOpsWithInsertOperation(nss, uuid, docToInsert0);

    ASSERT_OK(applyOps(
        opCtx.get(), nss.dbName(), cmdObj, OplogApplication::Mode::kInitialSync, &resultBuilder));
    ASSERT_EQUALS(1,
                  logs.countBSONContainingSubset(
                      BSON("attr" << BSON("oplogApplicationMode" << "InitialSync"))));

    auto docToInsert1 = BSON("_id" << 1);
    cmdObj = makeApplyOpsWithInsertOperation(nss, uuid, docToInsert1);

    ASSERT_OK(applyOps(
        opCtx.get(), nss.dbName(), cmdObj, OplogApplication::Mode::kSecondary, &resultBuilder));
    ASSERT_EQUALS(1,
                  logs.countBSONContainingSubset(
                      BSON("attr" << BSON("oplogApplicationMode" << "Secondary"))));
}

/**
 * Generates oplog entries with the given number used for the timestamp.
 */
OplogEntry makeOplogEntry(OpTypeEnum opType,
                          const BSONObj& oField,
                          const std::vector<StmtId>& stmtIds = {},
                          OperationSessionInfo sessionInfo = {}) {
    return {DurableOplogEntry(OpTime(Timestamp(1, 1), 1),                             // optime
                              opType,                                                 // op type
                              NamespaceString::createNamespaceString_forTest("a.a"),  // namespace
                              boost::none,                                            // uuid
                              boost::none,                                            // fromMigrate
                              boost::none,                // checkExistenceForDiffInsert
                              boost::none,                // versionContext
                              OplogEntry::kOplogVersion,  // version
                              oField,                     // o
                              boost::none,                // o2
                              sessionInfo,                // sessionInfo
                              boost::none,                // upsert
                              Date_t(),                   // wall clock time
                              stmtIds,                    // statement ids
                              boost::none,    // optime of previous write within same transaction
                              boost::none,    // pre-image optime
                              boost::none,    // post-image optime
                              boost::none,    // ShardId of resharding recipient
                              boost::none,    // _id
                              boost::none)};  // needsRetryImage
}

TEST_F(ApplyOpsTest, ExtractOperationsReturnsTypeMismatchIfNotCommand) {
    ASSERT_THROWS_CODE(
        ApplyOps::extractOperations(makeOplogEntry(OpTypeEnum::kInsert, BSON("_id" << 0))),
        DBException,
        ErrorCodes::TypeMismatch);
}

TEST_F(ApplyOpsTest, ExtractOperationsReturnsCommandNotSupportedIfNotApplyOpsCommand) {
    ASSERT_THROWS_CODE(
        ApplyOps::extractOperations(makeOplogEntry(OpTypeEnum::kCommand, BSON("create" << "t"))),
        DBException,
        ErrorCodes::CommandNotSupported);
}

TEST_F(ApplyOpsTest, ExtractOperationsReturnsEmptyArrayIfApplyOpsContainsNoOperations) {
    auto operations = ApplyOps::extractOperations(
        makeOplogEntry(OpTypeEnum::kCommand, BSON("applyOps" << BSONArray())));
    ASSERT_EQUALS(0U, operations.size());
}

TEST_F(ApplyOpsTest, ExtractOperationsReturnsOperationsWithSameOpTimeAsApplyOps) {
    NamespaceString ns1 = NamespaceString::createNamespaceString_forTest("test.a");
    auto ui1 = UUID::gen();
    auto op1 = BSON("op" << "i"
                         << "ns" << ns1.ns_forTest() << "ui" << ui1 << "o" << BSON("_id" << 1));

    NamespaceString ns2 = NamespaceString::createNamespaceString_forTest("test.b");
    auto ui2 = UUID::gen();
    auto op2 = BSON("op" << "i"
                         << "ns" << ns2.ns_forTest() << "ui" << ui2 << "o" << BSON("_id" << 2));

    NamespaceString ns3 = NamespaceString::createNamespaceString_forTest("test.c");
    auto ui3 = UUID::gen();
    auto op3 = BSON("op" << "u"
                         << "ns" << ns3.ns_forTest() << "ui" << ui3 << "b" << true << "o"
                         << BSON("x" << 1) << "o2" << BSON("_id" << 3));

    auto oplogEntry =
        makeOplogEntry(OpTypeEnum::kCommand, BSON("applyOps" << BSON_ARRAY(op1 << op2 << op3)));

    auto operations = ApplyOps::extractOperations(oplogEntry);
    ASSERT_EQUALS(3U, operations.size())
        << "Unexpected number of operations extracted: " << oplogEntry.toBSONForLogging();

    // Check extracted CRUD operations.
    auto it = operations.cbegin();
    {
        ASSERT(operations.cend() != it);
        const auto& operation1 = *(it++);
        ASSERT(OpTypeEnum::kInsert == operation1.getOpType())
            << "Unexpected op type: " << operation1.toBSONForLogging();
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
            << "Unexpected op type: " << operation2.toBSONForLogging();
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
            << "Unexpected op type: " << operation3.toBSONForLogging();
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

TEST_F(ApplyOpsTest, ExtractOperationsFromApplyOpsMultiStmtIds) {
    NamespaceString ns1 = NamespaceString::createNamespaceString_forTest("test.a");
    auto ui1 = UUID::gen();
    auto op1 = BSON("op" << "i"
                         << "ns" << ns1.ns_forTest() << "ui" << ui1 << "o" << BSON("_id" << 1));

    NamespaceString ns2 = NamespaceString::createNamespaceString_forTest("test.b");
    auto ui2 = UUID::gen();
    auto op2 = BSON("op" << "u"
                         << "ns" << ns2.ns_forTest() << "ui" << ui2 << "b" << true << "o"
                         << BSON("x" << 1) << "o2" << BSON("_id" << 2));

    auto oplogEntry =
        makeOplogEntry(OpTypeEnum::kCommand, BSON("applyOps" << BSON_ARRAY(op1 << op2)), {0, 1});

    auto operations = ApplyOps::extractOperations(oplogEntry);
    ASSERT_EQUALS(2U, operations.size())
        << "Unexpected number of operations extracted: " << oplogEntry.toBSONForLogging();

    // Check extracted CRUD operations.
    auto it = operations.cbegin();
    {
        ASSERT(operations.cend() != it);
        const auto& operation1 = *(it++);
        ASSERT(OpTypeEnum::kInsert == operation1.getOpType())
            << "Unexpected op type: " << operation1.toBSONForLogging();
        ASSERT_EQUALS(ui1, *operation1.getUuid());
        ASSERT_EQUALS(ns1, operation1.getNss());
        ASSERT_BSONOBJ_EQ(BSON("_id" << 1), operation1.getOperationToApply());

        // OpTime of CRUD operation should match applyOps.
        ASSERT_EQUALS(oplogEntry.getOpTime(), operation1.getOpTime());
    }

    {
        ASSERT(operations.cend() != it);
        const auto& operation2 = *(it++);
        ASSERT(OpTypeEnum::kUpdate == operation2.getOpType())
            << "Unexpected op type: " << operation2.toBSONForLogging();
        ASSERT_EQUALS(ui2, *operation2.getUuid());
        ASSERT_EQUALS(ns2, operation2.getNss());
        ASSERT_BSONOBJ_EQ(BSON("x" << 1), operation2.getOperationToApply());

        auto optionalUpsertBool = operation2.getUpsert();
        ASSERT(optionalUpsertBool);
        ASSERT(*optionalUpsertBool);

        // OpTime of CRUD operation should match applyOps.
        ASSERT_EQUALS(oplogEntry.getOpTime(), operation2.getOpTime());
    }

    ASSERT(operations.cend() == it);
}

TEST_F(ApplyOpsTest, ExtractOperationsIsUpsertDependsOnOperationAndAlwaysUpsert) {
    NamespaceString ns1 = NamespaceString::createNamespaceString_forTest("test.a");
    auto ui1 = UUID::gen();
    auto op1 = BSON("op" << "u"
                         << "ns" << ns1.ns_forTest() << "ui" << ui1 << "o"
                         << BSON("$set" << BSON("a" << 1)) << "o2" << BSON("_id" << 1));

    NamespaceString ns2 = NamespaceString::createNamespaceString_forTest("test.b");
    auto ui2 = UUID::gen();
    auto op2 =
        BSON("op" << "u"
                  << "ns" << ns2.ns_forTest() << "ui" << ui2 << "o"
                  << BSON("$set" << BSON("a" << 2)) << "o2" << BSON("_id" << 2) << "b" << false);

    NamespaceString ns3 = NamespaceString::createNamespaceString_forTest("test.c");
    auto ui3 = UUID::gen();
    auto op3 = BSON("op" << "u"
                         << "ns" << ns3.ns_forTest() << "ui" << ui3 << "b" << true << "o"
                         << BSON("$set" << BSON("a" << 3)) << "o2" << BSON("_id" << 3));

    // AlwayUpsert defaults to false.
    auto oplogEntry =
        makeOplogEntry(OpTypeEnum::kCommand, BSON("applyOps" << BSON_ARRAY(op1 << op2 << op3)));

    auto operations = ApplyOps::extractOperations(oplogEntry);
    ASSERT_EQUALS(3U, operations.size())
        << "Unexpected number of operations extracted: " << oplogEntry.toBSONForLogging();

    // Check extracted CRUD operations.
    auto it = operations.cbegin();
    {
        ASSERT(operations.cend() != it);
        const auto& operation1 = *(it++);
        ASSERT(OpTypeEnum::kUpdate == operation1.getOpType())
            << "Unexpected op type: " << operation1.toBSONForLogging();
        ASSERT_EQUALS(ui1, *operation1.getUuid());
        ASSERT_EQUALS(ns1, operation1.getNss());
        ASSERT_BSONOBJ_EQ(BSON("$set" << BSON("a" << 1)), operation1.getOperationToApply());
        ASSERT(operation1.getObject2());
        ASSERT_BSONOBJ_EQ(BSON("_id" << 1), *operation1.getObject2());

        // No "b" and "alwaysUpsert" false -> no upsert.
        ASSERT_FALSE(operation1.getUpsert());
    }

    {
        ASSERT(operations.cend() != it);
        const auto& operation2 = *(it++);
        ASSERT(OpTypeEnum::kUpdate == operation2.getOpType())
            << "Unexpected op type: " << operation2.toBSONForLogging();
        ASSERT_EQUALS(ui2, *operation2.getUuid());
        ASSERT_EQUALS(ns2, operation2.getNss());
        ASSERT_BSONOBJ_EQ(BSON("$set" << BSON("a" << 2)), operation2.getOperationToApply());
        ASSERT(operation2.getObject2());
        ASSERT_BSONOBJ_EQ(BSON("_id" << 2), *operation2.getObject2());

        // "b" false and "alwaysUpsert" false -> upsert false.
        ASSERT_TRUE(operation2.getUpsert());
        ASSERT_FALSE(*operation2.getUpsert());
    }

    {
        ASSERT(operations.cend() != it);
        const auto& operation3 = *(it++);
        ASSERT(OpTypeEnum::kUpdate == operation3.getOpType())
            << "Unexpected op type: " << operation3.toBSONForLogging();
        ASSERT_EQUALS(ui3, *operation3.getUuid());
        ASSERT_EQUALS(ns3, operation3.getNss());
        ASSERT_BSONOBJ_EQ(BSON("$set" << BSON("a" << 3)), operation3.getOperationToApply());
        ASSERT(operation3.getObject2());
        ASSERT_BSONOBJ_EQ(BSON("_id" << 3), *operation3.getObject2());

        // "b" true and "alwaysUpsert" false -> upsert true.
        ASSERT_TRUE(operation3.getUpsert());
        ASSERT(*operation3.getUpsert());
    }
    ASSERT(operations.cend() == it);
}

TEST_F(ApplyOpsTest, ApplyOpsFailsToDropAdmin) {
    auto opCtx = cc().makeOperationContext();
    auto mode = OplogApplication::Mode::kApplyOpsCmd;

    // Create a collection on the admin database.
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("admin.foo");
    CollectionOptions options;
    options.uuid = UUID::gen();
    ASSERT_OK(_storage->createCollection(opCtx.get(), nss, options));

    auto dropDatabaseOp =
        BSON("op" << "c"
                  << "ns" << nss.getCommandNS().ns_forTest() << "o" << BSON("dropDatabase" << 1));

    auto dropDatabaseCmdObj = BSON("applyOps" << BSON_ARRAY(dropDatabaseOp));
    BSONObjBuilder resultBuilder;
    auto status = applyOps(opCtx.get(), nss.dbName(), dropDatabaseCmdObj, mode, &resultBuilder);
    ASSERT_EQUALS(ErrorCodes::IllegalOperation, status);
}

}  // namespace
}  // namespace repl
}  // namespace mongo
