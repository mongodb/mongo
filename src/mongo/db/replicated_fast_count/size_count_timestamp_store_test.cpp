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

#include "mongo/db/replicated_fast_count/size_count_timestamp_store.h"

#include "mongo/db/replicated_fast_count/replicated_fast_count_init.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/storage_engine.h"

namespace mongo::replicated_fast_count {
namespace {

enum class Mode { kCollection, kContainer };

// Runs each test case in both collection-backed and container-backed modes.
class SizeCountTimestampStoreTest : public CatalogTestFixture,
                                    public ::testing::WithParamInterface<Mode> {
protected:
    void setUp() override {
        CatalogTestFixture::setUp();
        auto opCtx = operationContext();
        if (GetParam() == Mode::kCollection) {
            ASSERT_OK(createReplicatedFastCountTimestampCollection(storageInterface(), opCtx));
            _store = std::make_unique<CollectionSizeCountTimestampStore>();
            return;
        }

        _ffDurability = std::make_unique<unittest::ServerParameterGuard>(
            "featureFlagReplicatedFastCountDurability", true);
        _ffContainerWrites =
            std::make_unique<unittest::ServerParameterGuard>("featureFlagContainerWrites", true);

        ASSERT_OK(createInternalFastCountContainers(opCtx,
                                                    NamespaceString::kAdminCommandNamespace,
                                                    ident::kFastCountMetadataStore,
                                                    KeyFormat::String,
                                                    ident::kFastCountMetadataStoreTimestamps,
                                                    KeyFormat::Long,
                                                    /*writeToOplog=*/false));

        auto* engine = opCtx->getServiceContext()->getStorageEngine()->getEngine();
        auto recordStore =
            engine->getRecordStore(opCtx,
                                   NamespaceString::kAdminCommandNamespace,
                                   ident::kFastCountMetadataStoreTimestamps,
                                   RecordStore::Options{.keyFormat = KeyFormat::Long},
                                   /*uuid=*/boost::none);
        _store = std::make_unique<ContainerSizeCountTimestampStore>(std::move(recordStore));
    }

    void writeTs(Timestamp timestamp) {
        WriteUnitOfWork wuow(operationContext());
        _store->write(operationContext(), timestamp);
        wuow.commit();
    }

    std::unique_ptr<unittest::ServerParameterGuard> _ffDurability;
    std::unique_ptr<unittest::ServerParameterGuard> _ffContainerWrites;
    std::unique_ptr<SizeCountTimestampStore> _store;
};

TEST_P(SizeCountTimestampStoreTest, WriteMassertsWithoutWriteUnitOfWork) {
    ASSERT_THROWS_CODE(_store->write(operationContext(), Timestamp(10, 1)), DBException, 12280400);
}

TEST_P(SizeCountTimestampStoreTest, ReadReturnsNoneWhenEmpty) {
    EXPECT_FALSE(_store->read(operationContext()).has_value());
}

TEST_P(SizeCountTimestampStoreTest, ReadWriteRoundTripNewEntry) {
    writeTs(Timestamp(10, 1));

    const auto result = _store->read(operationContext());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(Timestamp(10, 1), *result);
}

TEST_P(SizeCountTimestampStoreTest, WriteUpdatesExistingDocument) {
    writeTs(Timestamp(10, 1));
    writeTs(Timestamp(20, 2));

    const auto result = _store->read(operationContext());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(Timestamp(20, 2), *result);
}

TEST_P(SizeCountTimestampStoreTest, WriteWithSameTimestampIsIdempotent) {
    writeTs(Timestamp(10, 1));
    writeTs(Timestamp(10, 1));

    const auto result = _store->read(operationContext());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(Timestamp(10, 1), *result);
}

INSTANTIATE_TEST_SUITE_P(,
                         SizeCountTimestampStoreTest,
                         ::testing::Values(Mode::kCollection, Mode::kContainer),
                         [](const ::testing::TestParamInfo<Mode>& info) {
                             return info.param == Mode::kCollection ? "Collection" : "Container";
                         });

// Collection-only case: container mode provisions must pass an existing RecordStore to the
// SizeCountTimestampStore constructor, so there is no equivalent "backing storage does not exist"
// scenario to exercise.
class SizeCountTimestampStoreCollectionModeTest : public CatalogTestFixture {};

TEST_F(SizeCountTimestampStoreCollectionModeTest, ReadReturnsNoneWhenCollectionDoesNotExist) {
    const CollectionSizeCountTimestampStore store;
    EXPECT_FALSE(store.read(operationContext()).has_value());
}

}  // namespace
}  // namespace mongo::replicated_fast_count
