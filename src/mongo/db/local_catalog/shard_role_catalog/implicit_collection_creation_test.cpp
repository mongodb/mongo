/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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


#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/clustered_collection_options_gen.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/database.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_runtime.h"
#include "mongo/db/local_catalog/shard_role_catalog/operation_sharding_state.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <memory>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

class ImplicitCollectionCreationTest : public ShardServerTestFixture {};

TEST_F(ImplicitCollectionCreationTest, ImplicitCreateDisallowedByDefault) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        "ImplicitCreateDisallowedByDefaultDB.TestColl");
    AutoGetCollection autoColl(operationContext(), nss, MODE_IX);
    auto db = autoColl.ensureDbExists(operationContext());
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
    AutoGetCollection autoColl(operationContext(), nss, MODE_IX);
    auto db = autoColl.ensureDbExists(operationContext());
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
    AutoGetCollection autoColl(operationContext(), nss, MODE_IX);
    auto db = autoColl.ensureDbExists(operationContext());
    WriteUnitOfWork wuow(operationContext());
    ASSERT_OK(db->userCreateNS(operationContext(), nss, CollectionOptions{}));
    wuow.commit();

    const auto scopedCsr =
        CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(operationContext(), nss);
    ASSERT_FALSE(scopedCsr->getCurrentMetadataIfKnown());
}

}  // namespace
}  // namespace mongo
