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
#include "mongo/unittest/death_test.h"

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

    ShardingDDLCoordinatorMetadata createMetadata() const {
        ShardingDDLCoordinatorMetadata metadata(getCoordinatorId());
        metadata.setForwardableOpMetadata(ForwardableOperationMetadata(_opCtx));
        metadata.setDatabaseVersion(kDbVersion);
        return metadata;
    }

    MigrationBlockingOperationCoordinatorDocument createStateDocument() const {
        MigrationBlockingOperationCoordinatorDocument doc;
        auto metadata = createMetadata();
        doc.setShardingDDLCoordinatorMetadata(metadata);
        return doc;
    }

    void setUp() override {
        PrimaryOnlyServiceMongoDTest::setUp();

        _opCtxHolder = makeOperationContext();
        _opCtx = _opCtxHolder.get();
        auto stateDocument = createStateDocument();
        _instance = checked_pointer_cast<MigrationBlockingOperationCoordinator>(
            Instance::getOrCreate(_opCtx, _service, stateDocument.toBSON()));
    }

    bool stateDocumentExistsOnDisk() {
        DBDirectClient client(_opCtx);
        auto count = client.count(NamespaceString::kShardingDDLCoordinatorsNamespace,
                                  BSON("_id" << getCoordinatorId().toBSON()));
        return count > 0;
    }

    MigrationBlockingOperationCoordinatorDocument getStateDocumentOnDisk() {
        ASSERT_TRUE(stateDocumentExistsOnDisk());
        DBDirectClient client(_opCtx);
        auto doc = client.findOne(NamespaceString::kShardingDDLCoordinatorsNamespace,
                                  BSON("_id" << getCoordinatorId().toBSON()));
        IDLParserContext errCtx(
            "MigrationBlockingOperationCoordinatorTest::getStateDocumentOnDisk()");
        return MigrationBlockingOperationCoordinatorDocument::parse(errCtx, doc);
    }

    void assertOperationCountOnDisk(int expectedCount) {
        auto doc = getStateDocumentOnDisk();
        ASSERT_EQ(expectedCount, doc.getOperations().get().size());
    }

    void beginOperations() {
        for (const auto& operationId : _operations) {
            _instance->beginOperation(_opCtx, operationId);
        }

        ASSERT_TRUE(stateDocumentExistsOnDisk());
    }

    void endOperations() {
        for (const auto& operationId : _operations) {
            _instance->endOperation(_opCtx, operationId);
        }
    }

    void tearDown() override {
        if (stateDocumentExistsOnDisk()) {
            auto doc = getStateDocumentOnDisk();
            for (const auto& operationId : doc.getOperations().get()) {
                _instance->endOperation(_opCtx, operationId);
            }

            ASSERT_OK(_instance->getCompletionFuture().getNoThrow());
            ASSERT_FALSE(stateDocumentExistsOnDisk());
        }

        PrimaryOnlyServiceMongoDTest::tearDown();
    }

    std::shared_ptr<MigrationBlockingOperationCoordinator> _instance;
    ServiceContext::UniqueOperationContext _opCtxHolder;
    OperationContext* _opCtx;
    std::vector<UUID> _operations;
};

TEST_F(MigrationBlockingOperationCoordinatorServiceTest, CreateAndDeleteStateDocument) {
    ASSERT_FALSE(stateDocumentExistsOnDisk());

    _operations = {UUID::gen()};
    beginOperations();
    assertOperationCountOnDisk(1);
    endOperations();

    ASSERT_OK(_instance->getCompletionFuture().getNoThrow());
    ASSERT_FALSE(stateDocumentExistsOnDisk());
}

TEST_F(MigrationBlockingOperationCoordinatorServiceTest, EndOperationDecrementsCount) {
    _operations = {UUID::gen(), UUID::gen()};
    beginOperations();
    _instance->endOperation(_opCtx, _operations[0]);
    assertOperationCountOnDisk(1);
}

TEST_F(MigrationBlockingOperationCoordinatorServiceTest, BeginMultipleOperations) {
    _operations = {UUID::gen(), UUID::gen()};
    beginOperations();
    assertOperationCountOnDisk(2);
}

TEST_F(MigrationBlockingOperationCoordinatorServiceTest, BeginSameOperationMultipleTimes) {
    _operations = {UUID::gen()};
    beginOperations();
    beginOperations();
    assertOperationCountOnDisk(1);
}

TEST_F(MigrationBlockingOperationCoordinatorServiceTest, EndSameOperationMultipleTimes) {
    _operations = {UUID::gen(), UUID::gen()};
    beginOperations();

    _instance->endOperation(_opCtx, _operations[0]);
    _instance->endOperation(_opCtx, _operations[0]);

    assertOperationCountOnDisk(1);
}

DEATH_TEST_F(MigrationBlockingOperationCoordinatorServiceTest,
             InvalidInitialStateDocument,
             "Operations should not be ongoing while migrations are running") {
    auto stateDocument = createStateDocument();

    std::vector<UUID> operations = {UUID::gen()};
    stateDocument.setOperations(operations);

    // Trigger a stepDown/stepUp to clear the default instance created in setUp().
    stepDown();
    stepUp(_opCtx);

    Instance::getOrCreate(_opCtx, _service, stateDocument.toBSON());
}

DEATH_TEST_F(MigrationBlockingOperationCoordinatorServiceTest,
             DuplicateOperationsOnDisk,
             "Duplicate operations found on disk with same UUID") {
    auto stateDocument = createStateDocument();
    stateDocument.setPhase(MigrationBlockingOperationCoordinatorPhaseEnum::kBlockingMigrations);

    auto duplicateUUID = UUID::gen();
    std::vector<UUID> duplicateOperationVector = {duplicateUUID, duplicateUUID};
    stateDocument.setOperations(duplicateOperationVector);

    // Trigger a stepDown/stepUp to clear the default instance created in setUp().
    stepDown();
    stepUp(_opCtx);

    Instance::getOrCreate(_opCtx, _service, stateDocument.toBSON());
}

TEST_F(MigrationBlockingOperationCoordinatorServiceTest, FunctionCallWhileCoordinatorCleaningUp) {
    auto fp = globalFailPointRegistry().find("hangBeforeRemovingCoordinatorDocument");
    invariant(fp);
    fp->setMode(FailPoint::alwaysOn);

    _operations = {UUID::gen()};
    beginOperations();
    endOperations();

    fp->waitForTimesEntered(1);

    ASSERT_THROWS_CODE(beginOperations(),
                       DBException,
                       ErrorCodes::MigrationBlockingOperationCoordinatorCleaningUp);

    ASSERT_THROWS_CODE(
        endOperations(), DBException, ErrorCodes::MigrationBlockingOperationCoordinatorCleaningUp);

    fp->setMode(FailPoint::off);

    ASSERT_OK(_instance->getCompletionFuture().getNoThrow());
    ASSERT_FALSE(stateDocumentExistsOnDisk());
}

}  // namespace mongo
