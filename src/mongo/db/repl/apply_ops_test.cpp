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

#include "mongo/platform/basic.h"

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
#include "mongo/stdx/memory.h"

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
    ReplicationCoordinator::set(service, stdx::make_unique<ReplicationCoordinatorMock>(service));
    setOplogCollectionName();
    createOplog(opCtx.get());

    // Ensure that we are primary.
    auto replCoord = ReplicationCoordinator::get(opCtx.get());
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_PRIMARY));

    // Use OpObserverMock to track notifications for applyOps().
    auto opObserver = stdx::make_unique<OpObserverMock>();
    _opObserver = opObserver.get();
    service->setOpObserver(std::move(opObserver));

    // This test uses StorageInterface to create collections and inspect documents inside
    // collections.
    _storage = stdx::make_unique<StorageInterfaceImpl>();
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
                                << "ns"
                                << nss.ns()
                                << "o"
                                << documentToInsert
                                << "ui"
                                << *uuid)
                         : BSON("op"
                                << "i"
                                << "ns"
                                << nss.ns()
                                << "o"
                                << documentToInsert);
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
    ASSERT_EQUALS(ErrorCodes::UnknownError,
                  applyOps(opCtx.get(), "test", cmdObj, mode, &resultBuilder));
    auto result = resultBuilder.obj();
    auto status = getStatusFromApplyOpsResult(result);
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, status);
}

TEST_F(ApplyOpsTest, AtomicApplyOpsInsertIntoCollectionWithoutUuid) {
    auto opCtx = cc().makeOperationContext();
    auto mode = OplogApplication::Mode::kApplyOpsCmd;
    NamespaceString nss("test.t");

    // Collection has no uuid.
    CollectionOptions collectionOptions;
    ASSERT_OK(_storage->createCollection(opCtx.get(), nss, collectionOptions));

    auto documentToInsert = BSON("_id" << 0);
    auto cmdObj = makeApplyOpsWithInsertOperation(nss, boost::none, documentToInsert);
    BSONObjBuilder resultBuilder;
    ASSERT_OK(applyOps(opCtx.get(), "test", cmdObj, mode, &resultBuilder));
    ASSERT_BSONOBJ_EQ(cmdObj, _opObserver->onApplyOpsCmdObj);
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

TEST_F(ApplyOpsTest, AtomicApplyOpsInsertWithUuidIntoCollectionWithoutUuid) {
    auto opCtx = cc().makeOperationContext();
    auto mode = OplogApplication::Mode::kApplyOpsCmd;
    NamespaceString nss("test.t");

    auto uuid = UUID::gen();

    // Collection has no uuid.
    CollectionOptions collectionOptions;
    ASSERT_OK(_storage->createCollection(opCtx.get(), nss, collectionOptions));

    // The applyOps returns a NamespaceNotFound error because of the failed UUID lookup
    // even though a collection exists with the same namespace as the insert operation.
    auto documentToInsert = BSON("_id" << 0);
    auto cmdObj = makeApplyOpsWithInsertOperation(nss, uuid, documentToInsert);
    BSONObjBuilder resultBuilder;
    ASSERT_EQUALS(ErrorCodes::UnknownError,
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
                               << "ns"
                               << nss.getCommandNS().ns()
                               << "o"
                               << BSON("dropDatabase" << 1));

    auto dropDatabaseCmdObj = BSON("applyOps" << BSON_ARRAY(dropDatabaseOp));
    BSONObjBuilder resultBuilder;
    auto status =
        applyOps(opCtx.get(), nss.db().toString(), dropDatabaseCmdObj, mode, &resultBuilder);
    ASSERT_EQUALS(ErrorCodes::UnknownError, status);
}

}  // namespace
}  // namespace repl
}  // namespace mongo
