/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/base/checked_cast.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/tenant_cloner_test_fixture.h"

namespace mongo {
namespace repl {

void TenantClonerTestFixture::setUp() {
    ClonerTestFixture::setUp();

    serviceContext = getServiceContext();
    auto replCoord = std::make_unique<ReplicationCoordinatorMock>(serviceContext);
    ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
    ReplicationCoordinator::set(serviceContext, std::move(replCoord));
    StorageInterface::set(serviceContext, std::make_unique<StorageInterfaceImpl>());

    _sharedData = std::make_unique<TenantMigrationSharedData>(&_clock, _migrationId);

    _mockClient->setOperationTime(_operationTime);
}

TenantMigrationSharedData* TenantClonerTestFixture::getSharedData() {
    return checked_cast<TenantMigrationSharedData*>(_sharedData.get());
}

Status TenantClonerTestFixture::createCollection(const NamespaceString& nss,
                                                 const CollectionOptions& options) {
    auto storage = StorageInterface::get(serviceContext);
    auto opCtx = cc().makeOperationContext();
    repl::createOplog(opCtx.get());
    return storage->createCollection(opCtx.get(), nss, options);
}

}  // namespace repl
}  // namespace mongo
