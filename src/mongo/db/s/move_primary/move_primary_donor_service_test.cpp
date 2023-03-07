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

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/primary_only_service_test_fixture.h"
#include "mongo/db/s/move_primary/move_primary_donor_service.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

const auto kDatabaseName = NamespaceString{"testDb"};
constexpr auto kNewPrimaryShardName = "newPrimaryId";

class MovePrimaryDonorServiceTest : public repl::PrimaryOnlyServiceMongoDTest {
protected:
    using DonorInstance = MovePrimaryDonor;

    std::unique_ptr<repl::PrimaryOnlyService> makeService(ServiceContext* serviceContext) override {
        return std::make_unique<MovePrimaryDonorService>(serviceContext);
    }

    MovePrimaryCommonMetadata createMetadata() const {
        MovePrimaryCommonMetadata metadata;
        metadata.set_id(UUID::gen());
        metadata.setDatabaseName(kDatabaseName);
        metadata.setShardName(kNewPrimaryShardName);
        return metadata;
    }

    MovePrimaryDonorDocument createStateDocument() const {
        MovePrimaryDonorDocument doc;
        doc.setMetadata(createMetadata());
        return doc;
    }

    MovePrimaryDonorDocument getStateDocumentOnDisk(OperationContext* opCtx) {
        DBDirectClient client(opCtx);
        auto doc = client.findOne(NamespaceString::kMovePrimaryDonorNamespace, BSONObj{});
        IDLParserContext errCtx("MovePrimaryDonorServiceTest::getStateDocumentOnDisk()");
        return MovePrimaryDonorDocument::parse(errCtx, doc);
    }

    static constexpr auto kBefore = "before";
    static constexpr auto kPartial = "partial";
    static constexpr auto kAfter = "after";

    auto pauseStateTransitionImpl(const std::string& progress,
                                  MovePrimaryDonorStateEnum state,
                                  const std::string& failpointName) {
        auto fp = globalFailPointRegistry().find(failpointName);
        auto count = fp->setMode(FailPoint::alwaysOn,
                                 0,
                                 fromjson(fmt::format("{{progress: '{}', state: '{}'}}",
                                                      progress,
                                                      MovePrimaryDonorState_serializer(state))));
        return std::tuple{fp, count};
    }

    auto pauseStateTransition(const std::string& progress, MovePrimaryDonorStateEnum state) {
        return pauseStateTransitionImpl(
            progress, state, "pauseDuringMovePrimaryDonorStateEnumTransition");
    }

    auto pauseStateTransitionAlternate(const std::string& progress,
                                       MovePrimaryDonorStateEnum state) {
        return pauseStateTransitionImpl(
            progress, state, "pauseDuringMovePrimaryDonorStateEnumTransitionAlternate");
    }

    auto failCrudOpsOn(NamespaceString nss) {
        auto fp = globalFailPointRegistry().find("failCommand");
        auto count =
            fp->setMode(FailPoint::alwaysOn,
                        0,
                        fromjson(fmt::format("{{failCommands:['insert', 'update', 'delete'], "
                                             "namespace: '{}', failLocalClients: true, "
                                             "failInternalCommands: true, errorCode: {}}}",
                                             nss.toString(),
                                             ErrorCodes::Interrupted)));
        return std::tuple{fp, count};
    }

    BSONObj getMetrics(const std::shared_ptr<DonorInstance>& instance) {
        auto currentOp = instance->reportForCurrentOp(
            MongoProcessInterface::CurrentOpConnectionsMode::kExcludeIdle,
            MongoProcessInterface::CurrentOpSessionsMode::kExcludeIdle);
        ASSERT_TRUE(currentOp);
        return *currentOp;
    }

    std::shared_ptr<DonorInstance> getExistingInstance(OperationContext* opCtx, const UUID& id) {
        auto instanceId = BSON(MovePrimaryDonorDocument::k_idFieldName << id);
        auto instance = DonorInstance::lookup(opCtx, _service, instanceId);
        if (!instance) {
            return nullptr;
        }
        return *instance;
    }
};

TEST_F(MovePrimaryDonorServiceTest, CreateInstance) {
    auto opCtx = makeOperationContext();
    auto stateDoc = createStateDocument();
    auto instance = DonorInstance::getOrCreate(opCtx.get(), _service, stateDoc.toBSON());
}

TEST_F(MovePrimaryDonorServiceTest, GetMetadata) {
    auto opCtx = makeOperationContext();
    auto stateDoc = createStateDocument();
    auto instance = DonorInstance::getOrCreate(opCtx.get(), _service, stateDoc.toBSON());
    ASSERT_BSONOBJ_EQ(stateDoc.getMetadata().toBSON(), instance->getMetadata().toBSON());
}

TEST_F(MovePrimaryDonorServiceTest, CannotCreateTwoInstancesForSameDb) {
    auto opCtx = makeOperationContext();
    auto stateDoc = createStateDocument();
    auto instance = DonorInstance::getOrCreate(opCtx.get(), _service, stateDoc.toBSON());
    auto otherStateDoc = stateDoc;
    otherStateDoc.getMetadata().set_id(UUID::gen());
    ASSERT_THROWS_CODE(DonorInstance::getOrCreate(opCtx.get(), _service, otherStateDoc.toBSON()),
                       DBException,
                       ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(MovePrimaryDonorServiceTest, SameUuidMustHaveSameDb) {
    auto opCtx = makeOperationContext();
    auto stateDoc = createStateDocument();
    auto instance = DonorInstance::getOrCreate(opCtx.get(), _service, stateDoc.toBSON());
    auto otherStateDoc = stateDoc;
    otherStateDoc.getMetadata().setDatabaseName(NamespaceString{"someOtherDb"});
    ASSERT_THROWS_CODE(DonorInstance::getOrCreate(opCtx.get(), _service, otherStateDoc.toBSON()),
                       DBException,
                       ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(MovePrimaryDonorServiceTest, SameUuidMustHaveSameRecipient) {
    auto opCtx = makeOperationContext();
    auto stateDoc = createStateDocument();
    auto instance = DonorInstance::getOrCreate(opCtx.get(), _service, stateDoc.toBSON());
    auto otherStateDoc = stateDoc;
    otherStateDoc.getMetadata().setShardName("someOtherShard");
    ASSERT_THROWS_CODE(DonorInstance::getOrCreate(opCtx.get(), _service, otherStateDoc.toBSON()),
                       DBException,
                       ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(MovePrimaryDonorServiceTest, StateDocumentIsPersistedAfterInitializing) {
    auto opCtx = makeOperationContext();
    auto stateDoc = createStateDocument();
    auto [fp, count] = pauseStateTransition(kAfter, MovePrimaryDonorStateEnum::kInitializing);
    auto instance = DonorInstance::getOrCreate(opCtx.get(), _service, stateDoc.toBSON());
    fp->waitForTimesEntered(count + 1);
    auto onDiskState = getStateDocumentOnDisk(opCtx.get());
    ASSERT_EQ(onDiskState.getMutableFields().getState(), MovePrimaryDonorStateEnum::kInitializing);
    fp->setMode(FailPoint::off);
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());
}

TEST_F(MovePrimaryDonorServiceTest, StateDocumentInsertionRetriesIfWriteFails) {
    auto opCtx = makeOperationContext();
    auto stateDoc = createStateDocument();
    auto [beforeFp, beforeCount] =
        pauseStateTransition(kBefore, MovePrimaryDonorStateEnum::kInitializing);
    auto [afterFp, afterCount] =
        pauseStateTransitionAlternate(kAfter, MovePrimaryDonorStateEnum::kInitializing);
    auto instance = DonorInstance::getOrCreate(opCtx.get(), _service, stateDoc.toBSON());
    beforeFp->waitForTimesEntered(beforeCount + 1);

    auto [failCrud, crudCount] = failCrudOpsOn(NamespaceString::kMovePrimaryDonorNamespace);
    beforeFp->setMode(FailPoint::off);
    failCrud->waitForTimesEntered(crudCount + 1);
    failCrud->setMode(FailPoint::off);

    afterFp->waitForTimesEntered(afterCount + 1);
    auto onDiskState = getStateDocumentOnDisk(opCtx.get());
    ASSERT_EQ(onDiskState.getMutableFields().getState(), MovePrimaryDonorStateEnum::kInitializing);
    afterFp->setMode(FailPoint::off);
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());
}

TEST_F(MovePrimaryDonorServiceTest, InitializingUpdatesInMemoryState) {
    auto opCtx = makeOperationContext();
    auto stateDoc = createStateDocument();
    auto [fp, count] = pauseStateTransition(kAfter, MovePrimaryDonorStateEnum::kInitializing);
    auto instance = DonorInstance::getOrCreate(opCtx.get(), _service, stateDoc.toBSON());
    fp->waitForTimesEntered(count + 1);

    ASSERT_EQ(getMetrics(instance).getStringField("state"), "initializing");

    fp->setMode(FailPoint::off);
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());
}

TEST_F(MovePrimaryDonorServiceTest, FailoverInInitializing) {
    auto opCtx = makeOperationContext();
    auto stateDoc = createStateDocument();

    {
        auto [fp, count] = pauseStateTransition(kAfter, MovePrimaryDonorStateEnum::kInitializing);
        auto instance = DonorInstance::getOrCreate(opCtx.get(), _service, stateDoc.toBSON());
        fp->waitForTimesEntered(count + 1);

        stepDown();
        fp->setMode(FailPoint::off);
        ASSERT_NOT_OK(instance->getCompletionFuture().getNoThrow());
    }

    auto fp = globalFailPointRegistry().find("pauseBeforeBeginningMovePrimaryDonorWorkflow");
    auto count = fp->setMode(FailPoint::alwaysOn);
    stepUp(opCtx.get());
    auto instance = getExistingInstance(opCtx.get(), stateDoc.get_id());
    fp->waitForTimesEntered(count + 1);
    fp->setMode(FailPoint::off);
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());
}

}  // namespace
}  // namespace mongo
