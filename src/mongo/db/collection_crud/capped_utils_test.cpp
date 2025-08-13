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

#include "mongo/db/collection_crud/capped_utils.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/local_catalog/clustered_collection_options_gen.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <ostream>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>

namespace {

using namespace mongo;

class CappedUtilsTest : public ServiceContextMongoDTest {
private:
    void setUp() override;
    void tearDown() override;

protected:
    // Use StorageInterface to access storage features below catalog interface.
    std::unique_ptr<repl::StorageInterface> _storage;
};

void CappedUtilsTest::setUp() {
    // Set up mongod.
    ServiceContextMongoDTest::setUp();

    auto service = getServiceContext();

    // Set up ReplicationCoordinator and ensure that we are primary.
    auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(service);
    ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
    repl::ReplicationCoordinator::set(service, std::move(replCoord));

    _storage = std::make_unique<repl::StorageInterfaceImpl>();
}

void CappedUtilsTest::tearDown() {
    _storage = {};

    // Tear down mongod.
    ServiceContextMongoDTest::tearDown();
}

/**
 * Creates an OperationContext.
 */
ServiceContext::UniqueOperationContext makeOpCtx() {
    auto opCtx = cc().makeOperationContext();
    repl::createOplog(opCtx.get());
    return opCtx;
}

/**
 * Returns true if collection exists.
 */
bool collectionExists(OperationContext* opCtx, const NamespaceString& nss) {
    const auto coll = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest(nss,
                                     PlacementConcern(boost::none, ShardVersion::UNSHARDED()),
                                     repl::ReadConcernArgs::get(opCtx),
                                     AcquisitionPrerequisites::kRead),
        MODE_IS);
    return coll.exists();
}

/**
 * Returns collection options.
 */
CollectionOptions getCollectionOptions(OperationContext* opCtx, const NamespaceString& nss) {
    const auto coll = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest(nss,
                                     PlacementConcern(boost::none, ShardVersion::UNSHARDED()),
                                     repl::ReadConcernArgs::get(opCtx),
                                     AcquisitionPrerequisites::kRead),
        MODE_IS);
    ASSERT_TRUE(coll.exists()) << "Unable to get collections options for "
                               << nss.toStringForErrorMsg()
                               << " because collection does not exist.";
    return coll.getCollectionPtr()->getCollectionOptions();
}

// Size of capped collection to be passed to convertToCapped() which accepts a double.
// According to documentation, the size (in bytes) of capped collection must be a minimum of 4096
// and a multiple of 256.
const double cappedCollectionSize = 8192.0;

TEST_F(CappedUtilsTest, ConvertToCappedReturnsNamespaceNotFoundIfCollectionIsMissing) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    auto opCtx = makeOpCtx();
    ASSERT_FALSE(collectionExists(opCtx.get(), nss));
    ASSERT_THROWS_CODE(convertToCapped(opCtx.get(), nss, 1000.0, false /*fromMigrate*/),
                       DBException,
                       ErrorCodes::NamespaceNotFound);
}

TEST_F(CappedUtilsTest, ConvertToCappedUpdatesCollectionOptionsOnSuccess) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");

    auto opCtx = makeOpCtx();
    ASSERT_OK(_storage->createCollection(opCtx.get(), nss, {}));
    auto options = getCollectionOptions(opCtx.get(), nss);
    ASSERT_FALSE(options.capped);

    convertToCapped(opCtx.get(), nss, cappedCollectionSize, false /*fromMigrate*/);
    options = getCollectionOptions(opCtx.get(), nss);
    ASSERT_TRUE(options.capped);
    ASSERT_APPROX_EQUAL(cappedCollectionSize, options.cappedSize, 0.001)
        << "unexpected capped collection size: " << options.toBSON();
}

}  // namespace
