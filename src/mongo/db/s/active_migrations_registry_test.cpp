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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/s/move_chunk_request.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using unittest::assertGet;

ActiveMigrationsRegistry registry;

MoveChunkRequest createMoveChunkRequest(const NamespaceString& nss) {
    BSONObjBuilder builder;
    MoveChunkRequest::appendAsCommand(
        &builder,
        nss,
        ChunkVersion(2, 3, OID::gen()),
        assertGet(ConnectionString::parse("TestConfigRS/CS1:12345,CS2:12345,CS3:12345")),
        "shard0001",
        "shard0002",
        ChunkRange(BSON("Key" << -100), BSON("Key" << 100)),
        1024,
        MigrationSecondaryThrottleOptions::create(MigrationSecondaryThrottleOptions::kOff),
        true,
        true);
    return assertGet(MoveChunkRequest::createFromCommand(nss, builder.obj()));
}

TEST(MoveChunkRegistration, ScopedRegisterMigrationMoveConstructorAndAssignment) {
    auto originalScopedRegisterMigration = assertGet(
        registry.registerMigration(createMoveChunkRequest(NamespaceString("TestDB", "TestColl"))));

    ScopedRegisterMigration movedScopedRegisterMigration(
        std::move(originalScopedRegisterMigration));
    originalScopedRegisterMigration = std::move(movedScopedRegisterMigration);
}

TEST(MoveChunkRegistration, GetActiveMigrationNamespace) {
    ASSERT(!registry.getActiveMigrationNss());

    const NamespaceString nss("TestDB", "TestColl");

    auto originalScopedRegisterMigration =
        assertGet(registry.registerMigration(createMoveChunkRequest(nss)));

    ASSERT_EQ(nss.ns(), registry.getActiveMigrationNss()->ns());
}

TEST(MoveChunkRegistration, SecondMigrationReturnsConflictingOperationInProgress) {
    auto originalScopedRegisterMigration = assertGet(
        registry.registerMigration(createMoveChunkRequest(NamespaceString("TestDB", "TestColl1"))));

    auto secondScopedRegisterMigrationStatus =
        registry.registerMigration(createMoveChunkRequest(NamespaceString("TestDB", "TestColl2")));
    ASSERT_EQ(ErrorCodes::ConflictingOperationInProgress,
              secondScopedRegisterMigrationStatus.getStatus());
}

}  // namespace
}  // namespace mongo
