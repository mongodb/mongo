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

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/s/request_types/move_chunk_request.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using unittest::assertGet;

class MoveChunkRegistration : public ServiceContextMongoDTest {
protected:
    ActiveMigrationsRegistry _registry;
};

MoveChunkRequest createMoveChunkRequest(const NamespaceString& nss) {
    const ChunkVersion chunkVersion(1, 2, OID::gen());

    BSONObjBuilder builder;
    MoveChunkRequest::appendAsCommand(
        &builder,
        nss,
        chunkVersion,
        assertGet(ConnectionString::parse("TestConfigRS/CS1:12345,CS2:12345,CS3:12345")),
        ShardId("shard0001"),
        ShardId("shard0002"),
        ChunkRange(BSON("Key" << -100), BSON("Key" << 100)),
        1024,
        MigrationSecondaryThrottleOptions::create(MigrationSecondaryThrottleOptions::kOff),
        true);
    return assertGet(MoveChunkRequest::createFromCommand(nss, builder.obj()));
}

TEST_F(MoveChunkRegistration, ScopedDonateChunkMoveConstructorAndAssignment) {
    auto originalScopedDonateChunk = assertGet(_registry.registerDonateChunk(
        createMoveChunkRequest(NamespaceString("TestDB", "TestColl"))));
    ASSERT(originalScopedDonateChunk.mustExecute());

    ScopedDonateChunk movedScopedDonateChunk(std::move(originalScopedDonateChunk));
    ASSERT(movedScopedDonateChunk.mustExecute());

    originalScopedDonateChunk = std::move(movedScopedDonateChunk);
    ASSERT(originalScopedDonateChunk.mustExecute());

    // Need to signal the registered migration so the destructor doesn't invariant
    originalScopedDonateChunk.signalComplete(Status::OK());
}

TEST_F(MoveChunkRegistration, GetActiveMigrationNamespace) {
    ASSERT(!_registry.getActiveDonateChunkNss());

    const NamespaceString nss("TestDB", "TestColl");

    auto originalScopedDonateChunk =
        assertGet(_registry.registerDonateChunk(createMoveChunkRequest(nss)));

    ASSERT_EQ(nss.ns(), _registry.getActiveDonateChunkNss()->ns());

    // Need to signal the registered migration so the destructor doesn't invariant
    originalScopedDonateChunk.signalComplete(Status::OK());
}

TEST_F(MoveChunkRegistration, SecondMigrationReturnsConflictingOperationInProgress) {
    auto originalScopedDonateChunk = assertGet(_registry.registerDonateChunk(
        createMoveChunkRequest(NamespaceString("TestDB", "TestColl1"))));

    auto secondScopedDonateChunkStatus = _registry.registerDonateChunk(
        createMoveChunkRequest(NamespaceString("TestDB", "TestColl2")));
    ASSERT_EQ(ErrorCodes::ConflictingOperationInProgress,
              secondScopedDonateChunkStatus.getStatus());

    originalScopedDonateChunk.signalComplete(Status::OK());
}

TEST_F(MoveChunkRegistration, SecondMigrationWithSameArgumentsJoinsFirst) {
    auto originalScopedDonateChunk = assertGet(_registry.registerDonateChunk(
        createMoveChunkRequest(NamespaceString("TestDB", "TestColl"))));
    ASSERT(originalScopedDonateChunk.mustExecute());

    auto secondScopedDonateChunk = assertGet(_registry.registerDonateChunk(
        createMoveChunkRequest(NamespaceString("TestDB", "TestColl"))));
    ASSERT(!secondScopedDonateChunk.mustExecute());

    originalScopedDonateChunk.signalComplete({ErrorCodes::InternalError, "Test error"});
    auto opCtx = makeOperationContext();
    ASSERT_EQ(Status(ErrorCodes::InternalError, "Test error"),
              secondScopedDonateChunk.waitForCompletion(opCtx.get()));
}

}  // namespace
}  // namespace mongo
