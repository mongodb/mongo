// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_catalog/durable_catalog.h"
#include "mongo/db/storage/mdb_catalog.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/timeseries_test_fixture.h"
#include "mongo/rpc/get_status_from_command_result_write_util.h"

namespace mongo {
namespace {

class TimeseriesWriteCommandsTest : public timeseries::TimeseriesTestFixture {
protected:
    /**
     * Tests that writes fail against a collection which appears to be time-series, but the raw
     * collection metadata is missing required options (e.g. clusteredIndex, granularity).
     */
    void testWritesCheckOptionsAreValid(auto makeInvalidMetadataFn) {
        // Invalidating the collection metadata with low-level catalog manipulation,
        // since user-facing operations should never create invalid metadata.
        {
            Lock::GlobalWrite lk(_opCtx);

            const auto timeseriesNss = _resolveTimeseriesNss(_nsNoMeta);
            auto catalogId = CollectionCatalog::get(_opCtx)
                                 ->lookupCollectionByNamespace(_opCtx, timeseriesNss)
                                 ->getCatalogId();
            auto metadata =
                durable_catalog::getParsedCatalogEntry(_opCtx, catalogId, MDBCatalog::get(_opCtx))
                    ->metadata;
            makeInvalidMetadataFn(metadata);

            CollectionWriter writer{_opCtx, timeseriesNss};
            WriteUnitOfWork wuow(_opCtx);
            auto collection = writer.getWritableCollection(_opCtx);
            collection->replaceMetadata(_opCtx, std::move(metadata));
            wuow.commit();
        }

        DBDirectClient client(_opCtx);
        auto insertResponse =
            client.insertAcknowledged(_nsNoMeta, {BSON(_timeField << DATENOW << "a" << 1)});
        EXPECT_EQ(ErrorCodes::InvalidOptions, getStatusFromWriteCommandReply(insertResponse));
        auto updateResponse =
            client.updateAcknowledged(_nsNoMeta, fromjson("{a: 1}"), fromjson("{$set: {a: 2}}"));
        EXPECT_EQ(ErrorCodes::InvalidOptions, getStatusFromWriteCommandReply(updateResponse));
        auto deleteResponse = client.removeAcknowledged(_nsNoMeta, fromjson("{a: 1}"));
        EXPECT_EQ(ErrorCodes::InvalidOptions, getStatusFromWriteCommandReply(deleteResponse));
    }
};

TEST_F(TimeseriesWriteCommandsTest, TimeseriesWritesChecksHasClusteredIndex) {
    testWritesCheckOptionsAreValid([](auto& md) { md->options.clusteredIndex = {}; });
}

TEST_F(TimeseriesWriteCommandsTest, TimeseriesWritesChecksHasBucketMaxSpanSeconds) {
    testWritesCheckOptionsAreValid(
        [](auto& md) { md->options.timeseries->setBucketMaxSpanSeconds({}); });
}

TEST_F(TimeseriesWriteCommandsTest, TimeseriesWritesChecksHasGranularity) {
    testWritesCheckOptionsAreValid([](auto& md) { md->options.timeseries->setGranularity({}); });
}

}  // namespace
}  // namespace mongo
