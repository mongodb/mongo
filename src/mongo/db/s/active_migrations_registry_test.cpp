/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/s/move_chunk_request.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using unittest::assertGet;

class MoveChunkRegistration : public unittest::Test {
protected:
    void setUp() override {
        _client = _serviceContext.makeClient("MoveChunkRegistrationTest");
        _opCtx = _serviceContext.makeOperationContext(_client.get());
    }

    void tearDown() override {
        _opCtx.reset();
        _client.reset();
    }

    OperationContext* getTxn() const {
        return _opCtx.get();
    }

    ServiceContextNoop _serviceContext;
    ServiceContext::UniqueClient _client;
    ServiceContext::UniqueOperationContext _opCtx;

    ActiveMigrationsRegistry _registry;
};

MoveChunkRequest createMoveChunkRequest(const NamespaceString& nss) {
    BSONObjBuilder builder;
    MoveChunkRequest::appendAsCommand(
        &builder,
        nss,
        ChunkVersion(2, 3, OID::gen()),
        assertGet(ConnectionString::parse("TestConfigRS/CS1:12345,CS2:12345,CS3:12345")),
        ShardId("shard0001"),
        ShardId("shard0002"),
        ChunkRange(BSON("Key" << -100), BSON("Key" << 100)),
        1024,
        MigrationSecondaryThrottleOptions::create(MigrationSecondaryThrottleOptions::kOff),
        true,
        true);
    return assertGet(MoveChunkRequest::createFromCommand(nss, builder.obj()));
}

TEST_F(MoveChunkRegistration, ScopedRegisterMigrationMoveConstructorAndAssignment) {
    auto originalScopedRegisterMigration = assertGet(
        _registry.registerMigration(createMoveChunkRequest(NamespaceString("TestDB", "TestColl"))));
    ASSERT(originalScopedRegisterMigration.mustExecute());

    ScopedRegisterMigration movedScopedRegisterMigration(
        std::move(originalScopedRegisterMigration));
    ASSERT(movedScopedRegisterMigration.mustExecute());

    originalScopedRegisterMigration = std::move(movedScopedRegisterMigration);
    ASSERT(originalScopedRegisterMigration.mustExecute());

    // Need to signal the registered migration so the destructor doesn't invariant
    originalScopedRegisterMigration.complete(Status::OK());
}

TEST_F(MoveChunkRegistration, GetActiveMigrationNamespace) {
    ASSERT(!_registry.getActiveMigrationNss());

    const NamespaceString nss("TestDB", "TestColl");

    auto originalScopedRegisterMigration =
        assertGet(_registry.registerMigration(createMoveChunkRequest(nss)));

    ASSERT_EQ(nss.ns(), _registry.getActiveMigrationNss()->ns());

    // Need to signal the registered migration so the destructor doesn't invariant
    originalScopedRegisterMigration.complete(Status::OK());
}

TEST_F(MoveChunkRegistration, SecondMigrationReturnsConflictingOperationInProgress) {
    auto originalScopedRegisterMigration = assertGet(_registry.registerMigration(
        createMoveChunkRequest(NamespaceString("TestDB", "TestColl1"))));

    auto secondScopedRegisterMigrationStatus =
        _registry.registerMigration(createMoveChunkRequest(NamespaceString("TestDB", "TestColl2")));
    ASSERT_EQ(ErrorCodes::ConflictingOperationInProgress,
              secondScopedRegisterMigrationStatus.getStatus());

    originalScopedRegisterMigration.complete(Status::OK());
}

TEST_F(MoveChunkRegistration, SecondMigrationWithSameArgumentsJoinsFirst) {
    auto originalScopedRegisterMigration = assertGet(
        _registry.registerMigration(createMoveChunkRequest(NamespaceString("TestDB", "TestColl"))));
    ASSERT(originalScopedRegisterMigration.mustExecute());

    auto secondScopedRegisterMigration = assertGet(
        _registry.registerMigration(createMoveChunkRequest(NamespaceString("TestDB", "TestColl"))));
    ASSERT(!secondScopedRegisterMigration.mustExecute());

    originalScopedRegisterMigration.complete({ErrorCodes::InternalError, "Test error"});
    ASSERT_EQ(Status(ErrorCodes::InternalError, "Test error"),
              secondScopedRegisterMigration.waitForCompletion(getTxn()));
}

}  // namespace
}  // namespace mongo
