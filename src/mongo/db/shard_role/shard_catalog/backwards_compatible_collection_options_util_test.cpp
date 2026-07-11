// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/shard_role/shard_catalog/backwards_compatible_collection_options_util.h"

#include "mongo/db/dbhelpers.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/operation_logger_impl.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_role/ddl/coll_mod_gen.h"
#include "mongo/db/shard_role/ddl/create_gen.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/db/timeseries/timeseries_collmod.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/timeseries_test_util.h"

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

    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(dbName + "." + collName);

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
    auto resolvedNss = timeseries::test_util::resolveTimeseriesNss(nss);
    ASSERT_BSONOBJ_EQ(oplogEntry.getObjectField("o"),
                      BSON(kCollMod << std::string{resolvedNss.coll()}));
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
