/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

// Exercises ReplicaSetWriteBlockOpObserver together with ReplicaSetWriteBlockState: replica-set
// write blocking, optional replica-set deletion blocking, bypass, internal DB exemptions, and
// <db>.system.profile. Helpers split insert/update from delete so range (fromMigrate) and TTL
// (direct client) delete paths can be tested separately.

#include "mongo/db/topology/user_write_block/replica_set_write_block_op_observer.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/op_observer/op_observer_util.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_role/ddl/create_gen.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_options_gen.h"
#include "mongo/db/shard_role/shard_catalog/collection_operation_source.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/topology/user_write_block/replica_set_write_block_bypass.h"
#include "mongo/db/topology/user_write_block/replica_set_write_block_state.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <memory>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

namespace mongo {
namespace {

struct DirectClientSetter {
    explicit DirectClientSetter(Client* client) : _client(client) {
        _client->setInDirectClient(true);
    }
    ~DirectClientSetter() {
        _client->setInDirectClient(false);
    }

private:
    Client* _client;
};

CreateCommand createCommandForTest(StringData nss) {
    return CreateCommand(NamespaceString::createNamespaceString_forTest(nss));
}

// Primary on a replset mock so canAcceptWritesFor is true and the op observer applies checks.
class ReplicaSetWriteBlockOpObserverTest : public ServiceContextMongoDTest {
public:
    void setUp() override {
        ServiceContextMongoDTest::setUp();

        auto service = getServiceContext();
        auto opCtx = cc().makeOperationContext();
        repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceMock>());

        // Set up ReplicationCoordinator and create oplog.
        repl::ReplicationCoordinator::set(
            service,
            std::make_unique<repl::ReplicationCoordinatorMock>(service, createReplSettings()));
        repl::createOplog(opCtx.get());

        // Ensure that we are primary.
        auto replCoord = repl::ReplicationCoordinator::get(opCtx.get());
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));

        ASSERT_OK(createCollection(opCtx.get(), createCommandForTest("userDB.coll")));
        ASSERT_OK(createCollection(opCtx.get(), createCommandForTest("userDB.system.profile")));
        ASSERT_OK(createCollection(opCtx.get(), createCommandForTest("admin.coll")));
        ASSERT_OK(createCollection(opCtx.get(), createCommandForTest("admin.collForRename")));
        ASSERT_OK(createCollection(opCtx.get(), createCommandForTest("local.coll")));
        ASSERT_OK(createCollection(opCtx.get(), createCommandForTest("config.coll")));
    }

protected:
    // Ensure that insert and update ops with the given opCtx on the given namespace will succeed or
    // fail depending on the value of shouldSucceed.
    void runInsertAndUpdate(OperationContext* opCtx,
                            const NamespaceString& nss,
                            bool fromMigrate,
                            bool shouldSucceed) {
        ASSERT(nss.isValid());

        AutoGetCollection autoColl(opCtx, nss, MODE_IX);
        if (!autoColl)
            FAIL(std::string(str::stream()
                             << "Collection " << nss.toStringForErrorMsg() << " doesn't exist"));

        ReplicaSetWriteBlockOpObserver opObserver;
        std::vector<InsertStatement> inserts;
        const auto criteria = BSON("_id" << 0);
        const auto preImageDoc = criteria;
        CollectionUpdateArgs collectionUpdateArgs{preImageDoc};
        collectionUpdateArgs.criteria = criteria;
        collectionUpdateArgs.source =
            fromMigrate ? OperationSource::kFromMigrate : OperationSource::kStandard;
        OplogUpdateEntryArgs updateArgs(&collectionUpdateArgs, *autoColl);
        if (shouldSucceed) {
            try {
                opObserver.onInserts(opCtx,
                                     *autoColl,
                                     inserts.begin(),
                                     inserts.end(),
                                     /*recordIds*/ {},
                                     /*fromMigrate=*/std::vector<bool>(inserts.size(), fromMigrate),
                                     /*defaultFromMigrate=*/fromMigrate);
                opObserver.onUpdate(opCtx, updateArgs);
            } catch (...) {
                // The test fails, log the failure through the ASSERT_OK machinary
                ASSERT_OK(exceptionToStatus());
            }
        } else {
            ASSERT_THROWS(
                opObserver.onInserts(opCtx,
                                     *autoColl,
                                     inserts.begin(),
                                     inserts.end(),
                                     /*recordIds*/ {},
                                     /*fromMigrate=*/std::vector<bool>(inserts.size(), fromMigrate),
                                     /*defaultFromMigrate=*/fromMigrate),
                AssertionException);
            ASSERT_THROWS(opObserver.onUpdate(opCtx, updateArgs), AssertionException);
        }
    }

    // Ensure that delete ops with the given opCtx on the given namespace will succeed or fail
    // depending on the value of shouldSucceed.
    void runDelete(OperationContext* opCtx,
                   const NamespaceString& nss,
                   bool useDirectClient,
                   bool fromMigrate,
                   bool shouldSucceed) {
        ASSERT(nss.isValid());
        AutoGetCollection autoColl(opCtx, nss, MODE_IX);
        if (!autoColl)
            FAIL(fmt::format("Collection {} doesn't exist", nss.toStringForErrorMsg()));

        ReplicaSetWriteBlockOpObserver opObserver;
        const auto preImageDoc = BSON("_id" << 0);
        const auto& documentKey = getDocumentKey(*autoColl, preImageDoc);
        OplogDeleteEntryArgs deleteArgs;
        deleteArgs.fromMigrate = fromMigrate;

        const auto invokeDelete = [&] {
            opObserver.onDelete(opCtx, *autoColl, StmtId(), preImageDoc, documentKey, deleteArgs);
        };

        boost::optional<DirectClientSetter> directClient;
        if (useDirectClient)
            directClient.emplace(opCtx->getClient());
        if (shouldSucceed) {
            ASSERT_DOES_NOT_THROW(invokeDelete());
        } else {
            ASSERT_THROWS_CODE(invokeDelete(), AssertionException, ErrorCodes::UserWritesBlocked);
        }
    }

private:
    repl::ReplSettings createReplSettings() {
        repl::ReplSettings settings;
        settings.setOplogSizeBytes(5 * 1024 * 1024);
        settings.setReplSetString("mySet/node1:12345");
        return settings;
    }
};

TEST_F(ReplicaSetWriteBlockOpObserverTest, ReplicaSetWriteAndDeletionBlockingDisabledNoBypass) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    // Disable blocking and ensure bypass is disabled.
    auto* rsBlock = ReplicaSetWriteBlockState::get(opCtx.get());
    rsBlock->disableReplicaSetWriteBlocking();
    rsBlock->disableReplicaSetDeletionsBlocking();
    ASSERT(!ReplicaSetWriteBlockBypass::get(opCtx.get()).isEnabled());

    const auto runWrites = [&](const NamespaceString& nss) {
        runInsertAndUpdate(opCtx.get(), nss, false /* fromMigrate */, true /* shouldSucceded */);
        runDelete(opCtx.get(),
                  nss,
                  false /* useDirectClient */,
                  false /* fromMigrate */,
                  true /* shouldSucceded */);
    };

    // Ensure writes succeed
    runWrites(NamespaceString::createNamespaceString_forTest("userDB.coll"));
    runWrites(NamespaceString::createNamespaceString_forTest("admin.coll"));
    runWrites(NamespaceString::createNamespaceString_forTest("local.coll"));
    runWrites(NamespaceString::createNamespaceString_forTest("config.coll"));

    // Ensure range deletetions and TTL deletions succeed.
    const auto userColl = NamespaceString::createNamespaceString_forTest("userDB.coll");
    runDelete(opCtx.get(), userColl, false, true /* fromMigrate */, true /* shouldSucceded */);
    runDelete(opCtx.get(),
              userColl,
              true /* useDirectClient */,
              false /* fromMigrate */,
              true /* shouldSucceded */);
}

// Write blocking disabled but bypass on: all tested namespaces should still succeed.
TEST_F(ReplicaSetWriteBlockOpObserverTest, ReplicaSetWriteAndDeletionBlockingDisabledWithBypass) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    // Disable blocking and ensure bypass is enabled
    auto* rsBlock = ReplicaSetWriteBlockState::get(opCtx.get());
    rsBlock->disableReplicaSetWriteBlocking();
    rsBlock->disableReplicaSetDeletionsBlocking();
    auto authSession = AuthorizationSession::get(opCtx->getClient());
    authSession->grantInternalAuthorization();
    ASSERT(authSession->mayBypassReplicaSetWriteBlocking());

    ReplicaSetWriteBlockBypass::get(opCtx.get()).setFromMetadata(opCtx.get(), {});
    ASSERT(ReplicaSetWriteBlockBypass::get(opCtx.get()).isEnabled());

    const auto runWrites = [&](const NamespaceString& nss) {
        runInsertAndUpdate(opCtx.get(), nss, false /* fromMigrate */, true /* shouldSucceded */);
        runDelete(opCtx.get(),
                  nss,
                  false /* useDirectClient */,
                  false /* fromMigrate */,
                  true /* shouldSucceded */);
    };

    // Ensure writes succeed.
    runWrites(NamespaceString::createNamespaceString_forTest("userDB.coll"));
    runWrites(NamespaceString::createNamespaceString_forTest("admin.coll"));
    runWrites(NamespaceString::createNamespaceString_forTest("local.coll"));
    runWrites(NamespaceString::createNamespaceString_forTest("config.coll"));

    // Ensure range deletetions and TTL deletions succeed.
    const auto userColl = NamespaceString::createNamespaceString_forTest("userDB.coll");
    runDelete(opCtx.get(), userColl, false, true /* fromMigrate */, true /* shouldSucceded */);
    runDelete(opCtx.get(),
              userColl,
              true /* useDirectClient */,
              false /* fromMigrate */,
              true /* shouldSucceded */);
}

TEST_F(ReplicaSetWriteBlockOpObserverTest, ReplicaSetWriteAndDeletionBlockingEnabledNoBypass) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    // Enable write blocking and ensure bypass is disabled.
    auto* rsBlock = ReplicaSetWriteBlockState::get(opCtx.get());
    rsBlock->enableReplicaSetWriteBlocking(ReplicaSetWritesBlockReasonEnum::kInsufficientDiskSpace);
    ReplicaSetWriteBlockBypass::get(opCtx.get()).set(false);
    ASSERT(!ReplicaSetWriteBlockBypass::get(opCtx.get()).isEnabled());

    const auto userColl = NamespaceString::createNamespaceString_forTest("userDB.coll");
    const auto profileNss = NamespaceString::createNamespaceString_forTest("userDB.system.profile");

    // User collection: insert/update are blocked while deletes are allowed.
    runInsertAndUpdate(opCtx.get(), userColl, false /* fromMigrate */, false /* shouldSucceded */);
    runDelete(opCtx.get(),
              userColl,
              false /* useDirectClient */,
              false /* fromMigrate */,
              true /* shouldSucceded */);

    // admin/local/config: exempt from replica set write blocking; insert/update/delete allowed.
    runInsertAndUpdate(opCtx.get(),
                       NamespaceString::createNamespaceString_forTest("admin.coll"),
                       false /* fromMigrate */,
                       true /* shouldSucceded */);
    runDelete(opCtx.get(),
              NamespaceString::createNamespaceString_forTest("admin.coll"),
              false /* useDirectClient */,
              false /* fromMigrate */,
              true /* shouldSucceded */);
    runInsertAndUpdate(opCtx.get(),
                       NamespaceString::createNamespaceString_forTest("local.coll"),
                       false /* fromMigrate */,
                       true /* shouldSucceded */);
    runDelete(opCtx.get(),
              NamespaceString::createNamespaceString_forTest("local.coll"),
              false /* useDirectClient */,
              false /* fromMigrate */,
              true /* shouldSucceded */);
    runInsertAndUpdate(opCtx.get(),
                       NamespaceString::createNamespaceString_forTest("config.coll"),
                       false /* fromMigrate */,
                       true /* shouldSucceded */);
    runDelete(opCtx.get(),
              NamespaceString::createNamespaceString_forTest("config.coll"),
              false /* useDirectClient */,
              false /* fromMigrate */,
              true /* shouldSucceded */);

    // Ensure operations on system.profile on user DB are exempt from write blocking.
    runInsertAndUpdate(opCtx.get(), profileNss, false /* fromMigrate */, true /* shouldSucceded */);
    runDelete(opCtx.get(),
              profileNss,
              false /* useDirectClient */,
              false /* fromMigrate */,
              true /* shouldSucceded */);

    // Ensure range deletetions and TTL deletions are allowed.
    runDelete(opCtx.get(), userColl, false, true /* fromMigrate */, true /* shouldSucceded */);
    runDelete(opCtx.get(),
              userColl,
              true /* useDirectClient */,
              false /* fromMigrate */,
              true /* shouldSucceded */);

    // Enable deletions blocking.
    rsBlock->enableReplicaSetDeletionsBlocking();

    // Ensure range deletions and TTL deletions are blocked.
    runDelete(opCtx.get(),
              userColl,
              false /* useDirectClient */,
              true /* fromMigrate */,
              false /* shouldSucceded */);
    runDelete(opCtx.get(),
              userColl,
              true /* useDirectClient */,
              false /* fromMigrate */,
              false /* shouldSucceded */);

    // Ensure operations on system.profile on user DB are still exempt.
    runInsertAndUpdate(opCtx.get(), profileNss, false /* fromMigrate */, true /* shouldSucceded */);
    runDelete(opCtx.get(),
              profileNss,
              false /* useDirectClient */,
              false /* fromMigrate */,
              true /* shouldSucceded */);
}

TEST_F(ReplicaSetWriteBlockOpObserverTest, ReplicaSetWriteAndDeletionBlockingEnabledWithBypass) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    // Enable write blocking and ensure bypass is enabled.
    auto* rsBlock = ReplicaSetWriteBlockState::get(opCtx.get());
    rsBlock->enableReplicaSetWriteBlocking(ReplicaSetWritesBlockReasonEnum::kInsufficientDiskSpace);
    rsBlock->enableReplicaSetDeletionsBlocking();
    auto authSession = AuthorizationSession::get(opCtx->getClient());
    authSession->grantInternalAuthorization();
    ASSERT(authSession->mayBypassReplicaSetWriteBlocking());
    ReplicaSetWriteBlockBypass::get(opCtx.get()).setFromMetadata(opCtx.get(), {});
    ASSERT(ReplicaSetWriteBlockBypass::get(opCtx.get()).isEnabled());

    const auto runWrites = [&](const NamespaceString& nss) {
        runInsertAndUpdate(opCtx.get(), nss, false /* fromMigrate */, true /* shouldSucceded */);
        runDelete(opCtx.get(),
                  nss,
                  false /* useDirectClient */,
                  false /* fromMigrate */,
                  true /* shouldSucceded */);
    };

    // Ensure writes succeed
    runWrites(NamespaceString::createNamespaceString_forTest("userDB.coll"));
    runWrites(NamespaceString::createNamespaceString_forTest("admin.coll"));
    runWrites(NamespaceString::createNamespaceString_forTest("local.coll"));
    runWrites(NamespaceString::createNamespaceString_forTest("config.coll"));

    // Ensure range deletetions and TTL deletions succeed.
    const auto userColl = NamespaceString::createNamespaceString_forTest("userDB.coll");
    runDelete(opCtx.get(),
              userColl,
              false /* useDirectClient */,
              true /* fromMigrate */,
              true /* shouldSucceded */);
    runDelete(opCtx.get(),
              userColl,
              true /* useDirectClient */,
              false /* fromMigrate */,
              true /* shouldSucceded */);
}

TEST_F(ReplicaSetWriteBlockOpObserverTest,
       ReplicaSetWriteBlockingEnabledAndDeletionBlockingDisabledNoBypass) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    // Enable write blocking, disable deletions blocking, and ensure bypass is disabled.
    auto* rsBlock = ReplicaSetWriteBlockState::get(opCtx.get());
    rsBlock->enableReplicaSetWriteBlocking(ReplicaSetWritesBlockReasonEnum::kInsufficientDiskSpace);
    rsBlock->disableReplicaSetDeletionsBlocking();
    ReplicaSetWriteBlockBypass::get(opCtx.get()).set(false);
    ASSERT(!ReplicaSetWriteBlockBypass::get(opCtx.get()).isEnabled());

    const auto userColl = NamespaceString::createNamespaceString_forTest("userDB.coll");

    // Ensure user writes fail.
    runInsertAndUpdate(opCtx.get(), userColl, false /* fromMigrate */, false /* shouldSucceded */);

    // Ensure user deletions succeded.
    runDelete(opCtx.get(),
              userColl,
              false /* useDirectClient */,
              false /* fromMigrate */,
              true /* shouldSucceded */);

    // Enruse TTL deletions succeded.
    runDelete(opCtx.get(),
              userColl,
              true /* useDirectClient */,
              false /* fromMigrate */,
              true /* shouldSucceded */);

    // Ensure range deletions succeded.
    runDelete(opCtx.get(),
              userColl,
              false /* useDirectClient */,
              true /* fromMigrate */,
              true /* shouldSucceded */);
}

TEST_F(ReplicaSetWriteBlockOpObserverTest,
       ReplicaSetWriteBlockingEnabledAndDeletionBlockingDisabledWithBypass) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    // Enable write blocking, disable deletions blocking, and ensure bypass is enabled.
    auto* rsBlock = ReplicaSetWriteBlockState::get(opCtx.get());
    rsBlock->enableReplicaSetWriteBlocking(ReplicaSetWritesBlockReasonEnum::kInsufficientDiskSpace);
    rsBlock->disableReplicaSetDeletionsBlocking();
    ReplicaSetWriteBlockBypass::get(opCtx.get()).set(true);
    ASSERT(ReplicaSetWriteBlockBypass::get(opCtx.get()).isEnabled());

    const auto userColl = NamespaceString::createNamespaceString_forTest("userDB.coll");

    // Ensure user writes suceeded.
    runInsertAndUpdate(opCtx.get(), userColl, false /* fromMigrate */, true /* shouldSucceded */);

    // Ensure user deletions succeded.
    runDelete(opCtx.get(),
              userColl,
              false /* useDirectClient */,
              false /* fromMigrate */,
              true /* shouldSucceded */);

    // Enruse TTL deletions succeded.
    runDelete(opCtx.get(),
              userColl,
              true /* useDirectClient */,
              false /* fromMigrate */,
              true /* shouldSucceded */);

    // Ensure range deletions succeded.
    runDelete(opCtx.get(),
              userColl,
              false /* useDirectClient */,
              true /* fromMigrate */,
              true /* shouldSucceded */);
}

}  // namespace
}  // namespace mongo
