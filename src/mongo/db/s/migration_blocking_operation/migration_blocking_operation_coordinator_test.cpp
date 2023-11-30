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

#include "mongo/db/catalog_raii.h"
#include "mongo/db/repl/primary_only_service_test_fixture.h"
#include "mongo/db/s/migration_blocking_operation/migration_blocking_operation_coordinator.h"
#include "mongo/db/s/migration_blocking_operation/migration_blocking_operation_coordinator_gen.h"
#include "mongo/db/s/sharding_ddl_coordinator_external_state_for_test.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

class MigrationBlockingOperationCoordinatorServiceTest : public repl::PrimaryOnlyServiceMongoDTest {
protected:
    using Service = ShardingDDLCoordinatorService;
    using Instance = MigrationBlockingOperationCoordinator;

    const NamespaceString kNamespace =
        NamespaceString::createNamespaceString_forTest("testDb", "coll");
    const DatabaseVersion kDbVersion{UUID::gen(), Timestamp(1, 0)};

    std::unique_ptr<repl::PrimaryOnlyService> makeService(ServiceContext* serviceContext) override {
        return std::make_unique<Service>(
            serviceContext, std::make_unique<ShardingDDLCoordinatorExternalStateFactoryForTest>());
    }

    ShardingDDLCoordinatorId getCoordinatorId() const {
        return ShardingDDLCoordinatorId{kNamespace,
                                        DDLCoordinatorTypeEnum::kMigrationBlockingOperation};
    }

    ShardingDDLCoordinatorMetadata createMetadata(OperationContext* opCtx) const {
        ShardingDDLCoordinatorMetadata metadata(getCoordinatorId());
        metadata.setForwardableOpMetadata(ForwardableOperationMetadata(opCtx));
        metadata.setDatabaseVersion(kDbVersion);
        return metadata;
    }

    MigrationBlockingOperationCoordinatorDocument createStateDocument(
        OperationContext* opCtx) const {
        MigrationBlockingOperationCoordinatorDocument doc;
        auto metadata = createMetadata(opCtx);
        doc.setShardingDDLCoordinatorMetadata(metadata);
        return doc;
    }
};

TEST_F(MigrationBlockingOperationCoordinatorServiceTest, CreateCoordinator) {
    auto opCtx = makeOperationContext();
    auto stateDocument = createStateDocument(opCtx.get());
    auto instance = Instance::getOrCreate(opCtx.get(), _service, stateDocument.toBSON());
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());
}

}  // namespace mongo
