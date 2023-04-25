/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/s/move_primary/move_primary_cloner_test_fixture.h"

#include "mongo/base/checked_cast.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_catalog_helper.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/platform/basic.h"

namespace mongo {

void MovePrimaryClonerTestFixture::setUp() {
    ClonerTestFixture::setUp();
    serviceContext = getServiceContext();
    auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(serviceContext);
    ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
    repl::ReplicationCoordinator::set(serviceContext, std::move(replCoord));
    repl::StorageInterface::set(serviceContext, std::make_unique<repl::StorageInterfaceImpl>());
    _sharedData = std::make_unique<MovePrimarySharedData>(&_clock, _migrationId);

    _mockClient->setOperationTime(_operationTime);
}

void MovePrimaryClonerTestFixture::tearDown() {
    ClonerTestFixture::tearDown();
}

MovePrimarySharedData* MovePrimaryClonerTestFixture::getSharedData() {
    return checked_cast<MovePrimarySharedData*>(_sharedData.get());
}

Status MovePrimaryClonerTestFixture::createCollection(const NamespaceString& nss,
                                                      const CollectionOptions& options) {
    auto opCtx = cc().getOperationContext();
    repl::createOplog(opCtx);
    auto storage = repl::StorageInterface::get(serviceContext);
    return storage->createCollection(opCtx, nss, options);
}
}  // namespace mongo
