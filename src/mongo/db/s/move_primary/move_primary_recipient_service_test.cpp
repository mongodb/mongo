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

#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/primary_only_service_test_fixture.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/move_primary/move_primary_recipient_cmds_gen.h"
#include "mongo/db/s/move_primary/move_primary_recipient_service.h"
#include "mongo/db/s/move_primary/move_primary_state_machine_gen.h"
#include "mongo/db/service_context_d_test_fixture.h"

namespace mongo {

class MovePrimaryRecipientServiceTest : public repl::PrimaryOnlyServiceMongoDTest {

    void setUp() override {
        repl::PrimaryOnlyServiceMongoDTest::setUp();

        _service = _registry->lookupServiceByName(
            MovePrimaryRecipientService::kMovePrimaryRecipientServiceName);
    }

    std::unique_ptr<repl::PrimaryOnlyService> makeService(ServiceContext* serviceContext) override {
        return std::make_unique<MovePrimaryRecipientService>(serviceContext);
    }

    void tearDown() override {
        repl::PrimaryOnlyServiceMongoDTest::tearDown();
    }

protected:
    MovePrimaryRecipientDocument createRecipientDoc() {
        UUID migrationId = UUID::gen();
        MovePrimaryRecipientDocument doc(migrationId);

        MovePrimaryRecipientMetadata metadata(migrationId, "foo", "first/localhost:27018");
        doc.setMovePrimaryRecipientMetadata(metadata);

        return doc;
    }
};

TEST_F(MovePrimaryRecipientServiceTest, MovePrimaryRecipientInstanceCreation) {
    auto doc = createRecipientDoc();
    auto opCtx = makeOperationContext();

    auto instance = MovePrimaryRecipientService::MovePrimaryRecipient::getOrCreate(
        opCtx.get(), _service, doc.toBSON());

    ASSERT(instance.get());
}

TEST_F(MovePrimaryRecipientServiceTest, CanTransitionTokStartedState) {
    auto doc = createRecipientDoc();

    auto movePrimaryRecipientPauseAfterInsertingStateDoc =
        globalFailPointRegistry().find("movePrimaryRecipientPauseAfterInsertingStateDoc");
    auto timesEnteredFailPoint =
        movePrimaryRecipientPauseAfterInsertingStateDoc->setMode(FailPoint::alwaysOn, 0);

    auto opCtx = makeOperationContext();
    auto instance = MovePrimaryRecipientService::MovePrimaryRecipient::getOrCreate(
        opCtx.get(), _service, doc.toBSON());

    movePrimaryRecipientPauseAfterInsertingStateDoc->waitForTimesEntered(timesEnteredFailPoint + 1);

    ASSERT(instance.get());
    ASSERT_EQ(doc.getMigrationId(), instance->getMigrationId());
    ASSERT(instance->getRecipientDocDurableFuture().isReady());

    movePrimaryRecipientPauseAfterInsertingStateDoc->setMode(FailPoint::off, 0);
}

TEST_F(MovePrimaryRecipientServiceTest, ThrowsWithConflictingOperation) {
    auto doc = createRecipientDoc();

    auto movePrimaryRecipientPauseAfterInsertingStateDoc =
        globalFailPointRegistry().find("movePrimaryRecipientPauseAfterInsertingStateDoc");
    auto timesEnteredFailPoint =
        movePrimaryRecipientPauseAfterInsertingStateDoc->setMode(FailPoint::alwaysOn, 0);

    auto opCtx = makeOperationContext();
    auto instance = MovePrimaryRecipientService::MovePrimaryRecipient::getOrCreate(
        opCtx.get(), _service, doc.toBSON());

    movePrimaryRecipientPauseAfterInsertingStateDoc->waitForTimesEntered(timesEnteredFailPoint + 1);

    auto conflictingDoc = createRecipientDoc();

    ASSERT_NE(doc.getId(), conflictingDoc.getId());

    // Asserts that a movePrimary op on same database fails with MovePrimaryInProgress
    ASSERT_THROWS_CODE(MovePrimaryRecipientService::MovePrimaryRecipient::getOrCreate(
                           opCtx.get(), _service, conflictingDoc.toBSON()),
                       DBException,
                       ErrorCodes::MovePrimaryInProgress);

    movePrimaryRecipientPauseAfterInsertingStateDoc->setMode(FailPoint::off, 0);
}

TEST_F(MovePrimaryRecipientServiceTest, ThrowsWithConflictingOptions) {
    auto doc = createRecipientDoc();

    auto movePrimaryRecipientPauseAfterInsertingStateDoc =
        globalFailPointRegistry().find("movePrimaryRecipientPauseAfterInsertingStateDoc");
    auto timesEnteredFailPoint =
        movePrimaryRecipientPauseAfterInsertingStateDoc->setMode(FailPoint::alwaysOn, 0);

    auto opCtx = makeOperationContext();
    auto instance = MovePrimaryRecipientService::MovePrimaryRecipient::getOrCreate(
        opCtx.get(), _service, doc.toBSON());

    movePrimaryRecipientPauseAfterInsertingStateDoc->waitForTimesEntered(timesEnteredFailPoint + 1);

    MovePrimaryRecipientDocument conflictingDoc(doc.getMigrationId());
    MovePrimaryRecipientMetadata metadata(doc.getMigrationId(), "bar", "second/localhost:27018");
    conflictingDoc.setMovePrimaryRecipientMetadata(metadata);

    // Asserts that a movePrimary op with a different fromShard fails with MovePrimaryInProgress
    ASSERT_THROWS_CODE(MovePrimaryRecipientService::MovePrimaryRecipient::getOrCreate(
                           opCtx.get(), _service, conflictingDoc.toBSON()),
                       DBException,
                       ErrorCodes::MovePrimaryInProgress);

    // Asserts that a movePrimary op with a different databaseName fails with MovePrimaryInProgress
    ASSERT_THROWS_CODE(MovePrimaryRecipientService::MovePrimaryRecipient::getOrCreate(
                           opCtx.get(), _service, conflictingDoc.toBSON()),
                       DBException,
                       ErrorCodes::MovePrimaryInProgress);

    movePrimaryRecipientPauseAfterInsertingStateDoc->setMode(FailPoint::off, 0);
}

}  // namespace mongo
