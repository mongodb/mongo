// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/database.h"
#include "mongo/db/shard_role/shard_catalog/database_holder.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

class ImplicitCollectionCreationTest : public ShardServerTestFixture {};

TEST_F(ImplicitCollectionCreationTest, ImplicitCreateDisallowedByDefault) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        "ImplicitCreateDisallowedByDefaultDB.TestColl");
    auto acq = acquireCollection(operationContext(),
                                 CollectionAcquisitionRequest::fromOpCtx(
                                     operationContext(), nss, AcquisitionPrerequisites::kWrite),
                                 MODE_IX);
    auto db = DatabaseHolder::get(operationContext())->openDb(operationContext(), nss.dbName());
    WriteUnitOfWork wuow(operationContext());
    ASSERT_THROWS_CODE(
        uassertStatusOK(db->userCreateNS(operationContext(), nss, CollectionOptions{})),
        DBException,
        ErrorCodes::CannotImplicitlyCreateCollection);
    wuow.commit();
}

TEST_F(ImplicitCollectionCreationTest, AllowImplicitCollectionCreate) {
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("AllowImplicitCollectionCreateDB.TestColl");
    OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE unsafeCreateCollection(
        operationContext(), nss);
    auto acq = acquireCollection(operationContext(),
                                 CollectionAcquisitionRequest::fromOpCtx(
                                     operationContext(), nss, AcquisitionPrerequisites::kWrite),
                                 MODE_IX);
    auto db = DatabaseHolder::get(operationContext())->openDb(operationContext(), nss.dbName());
    WriteUnitOfWork wuow(operationContext());
    ASSERT_OK(db->userCreateNS(operationContext(), nss, CollectionOptions{}));
    wuow.commit();

    const auto scopedCsr =
        CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(operationContext(), nss);
    ASSERT_TRUE(scopedCsr->getCurrentMetadataIfKnown());
}

TEST_F(ImplicitCollectionCreationTest, AllowImplicitCollectionCreateWithSetCSRAsUnknown) {
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("AllowImplicitCollectionCreateDB.TestColl");
    OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE unsafeCreateCollection(
        operationContext(), nss, /* forceCSRAsUnknownAfterCollectionCreation */ true);
    auto acq = acquireCollection(operationContext(),
                                 CollectionAcquisitionRequest::fromOpCtx(
                                     operationContext(), nss, AcquisitionPrerequisites::kWrite),
                                 MODE_IX);
    auto db = DatabaseHolder::get(operationContext())->openDb(operationContext(), nss.dbName());
    WriteUnitOfWork wuow(operationContext());
    ASSERT_OK(db->userCreateNS(operationContext(), nss, CollectionOptions{}));
    wuow.commit();

    const auto scopedCsr =
        CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(operationContext(), nss);
    ASSERT_FALSE(scopedCsr->getCurrentMetadataIfKnown());
}

}  // namespace
}  // namespace mongo
