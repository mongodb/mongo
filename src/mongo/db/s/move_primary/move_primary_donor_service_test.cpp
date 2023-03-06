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

}  // namespace
}  // namespace mongo
