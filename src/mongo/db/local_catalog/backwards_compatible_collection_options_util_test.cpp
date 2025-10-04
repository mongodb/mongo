/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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


#include "mongo/db/local_catalog/backwards_compatible_collection_options_util.h"

#include "mongo/db/dbhelpers.h"
#include "mongo/db/local_catalog/create_collection.h"
#include "mongo/db/local_catalog/ddl/coll_mod_gen.h"
#include "mongo/db/local_catalog/ddl/create_gen.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/operation_logger_impl.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/timeseries/timeseries_collmod.h"
#include "mongo/db/timeseries/timeseries_gen.h"

namespace mongo {
namespace {

const std::string kCollMod = "collMod";

class BackwardsCompatibleCollOptionsTest : public ServiceContextMongoDTest {
protected:
    void setUp() override {
        ServiceContextMongoDTest::setUp();

        auto service = getServiceContext();

        // Set up ReplicationCoordinator and ensure that we are primary.
        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(service);
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
        repl::ReplicationCoordinator::set(service, std::move(replCoord));

        _opCtx = _makeOpCtx();

        // Create test time-series collection
        auto tsOptions = TimeseriesOptions("t");
        CreateCommand cmd = CreateCommand(nss);
        cmd.getCreateCollectionRequest().setTimeseries(std::move(tsOptions));
        uassertStatusOK(createCollection(operationContext(), cmd));
    }

    OperationContext* operationContext() {
        return _opCtx.get();
    }

    void tearDown() override {
        ServiceContextMongoDTest::tearDown();
    }

    /* Test time-series namespaces */
    const std::string dbName = "db";
    const std::string collName = "coll";
    const std::string bucketCollName = "system.buckets." + collName;

    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(dbName + "." + collName);
    const NamespaceString bucketNss =
        NamespaceString::createNamespaceString_forTest(dbName + "." + bucketCollName);

private:
    ServiceContext::UniqueOperationContext _makeOpCtx() {
        auto opCtx = cc().makeOperationContext();
        opObserverRegistry()->addObserver(
            std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerImpl>()));
        repl::createOplog(opCtx.get());
        return opCtx;
    }

    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(BackwardsCompatibleCollOptionsTest, BackwardsIncompatibleFieldStripped) {
    CollModRequest collMod;
    collMod.setTimeseriesBucketsMayHaveMixedSchemaData(true);

    auto originalCmd = collMod.toBSON();
    auto [backwardsCompatibleCmd, additionalO2Field] =
        backwards_compatible_collection_options::getCollModCmdAndAdditionalO2Field(originalCmd);

    // backwards compatible command == original command - stripped params
    ASSERT_BSONOBJ_NE(originalCmd, backwardsCompatibleCmd);

    // original command == backwards compatible command + stripped params
    ASSERT_BSONOBJ_EQ(originalCmd, backwardsCompatibleCmd.addFields(additionalO2Field));
}

TEST_F(BackwardsCompatibleCollOptionsTest, CollModOplogEntryLoggedInBackwardsCompatibleFormat) {
    const auto opCtx = operationContext();

    CollMod collModCmd(nss);
    CollModRequest collModRequest;
    collModRequest.setTimeseriesBucketsMayHaveMixedSchemaData(true);
    collModCmd.setCollModRequest(collModRequest);

    BSONObjBuilder result;
    uassertStatusOK(timeseries::processCollModCommandWithTimeSeriesTranslation(
        opCtx, nss, collModCmd, true, &result));

    // Assert that the collMod oplog entry is in the expected backwards compatible format
    // This set of assertions are indirectly testing
    // `backwards_compatible_collection_options::getCollModCmdAndAdditionalO2Field`
    BSONObj oplogEntry;
    Helpers::getLast(opCtx, NamespaceString::kRsOplogNamespace, oplogEntry);
    ASSERT_BSONOBJ_NE(oplogEntry, BSONObj());
    ASSERT_BSONOBJ_EQ(oplogEntry.getObjectField("o"), BSON(kCollMod << bucketCollName));
    ASSERT_BSONOBJ_EQ(oplogEntry.getObjectField("o2").getObjectField(
                          backwards_compatible_collection_options::additionalCollModO2Field),
                      BSON("timeseriesBucketsMayHaveMixedSchemaData" << true));

    // Assert that reconstructing a collMod request from an oplog entry with backwards incompatible
    // catalog options add back the original parameters
    auto reconstructedCollModCmd =
        backwards_compatible_collection_options::parseCollModCmdFromOplogEntry(
            repl::OplogEntry(oplogEntry));

    // Strip out "collMod" because the original command refers the view namespace while the oplog
    // entry refers the translated bucket namespace
    ASSERT_BSONOBJ_EQ(reconstructedCollModCmd.removeField(kCollMod),
                      collModCmd.toBSON().removeField(kCollMod));
}

TEST_F(BackwardsCompatibleCollOptionsTest, ErrorParsingNonCollModOplogEntry) {
    BSONObj oplogEntry;
    Helpers::getLast(operationContext(), NamespaceString::kRsOplogNamespace, oplogEntry);
    ASSERT_THROWS_CODE(backwards_compatible_collection_options::parseCollModCmdFromOplogEntry(
                           repl::OplogEntry(oplogEntry)),
                       DBException,
                       ErrorCodes::IllegalOperation);
}

}  // namespace
}  // namespace mongo
