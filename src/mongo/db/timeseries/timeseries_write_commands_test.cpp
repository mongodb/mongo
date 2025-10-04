/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/durable_catalog.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/storage/mdb_catalog.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/timeseries_test_fixture.h"

#include "src/mongo/rpc/get_status_from_command_result_write_util.h"

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

            auto catalogId = CollectionCatalog::get(_opCtx)
                                 ->lookupCollectionByNamespace(
                                     _opCtx, _nsNoMeta.makeTimeseriesBucketsNamespace())
                                 ->getCatalogId();
            auto metadata =
                durable_catalog::getParsedCatalogEntry(_opCtx, catalogId, MDBCatalog::get(_opCtx))
                    ->metadata;
            makeInvalidMetadataFn(metadata);

            CollectionWriter writer{_opCtx, _nsNoMeta.makeTimeseriesBucketsNamespace()};
            WriteUnitOfWork wuow(_opCtx);
            auto collection = writer.getWritableCollection(_opCtx);
            collection->replaceMetadata(_opCtx, std::move(metadata));
            wuow.commit();
        }

        DBDirectClient client(_opCtx);
        auto insertResponse =
            client.insertAcknowledged(_nsNoMeta, {BSON(_timeField << DATENOW << "a" << 1)});
        ASSERT_EQUALS(ErrorCodes::InvalidOptions, getStatusFromWriteCommandReply(insertResponse));
        auto updateResponse =
            client.updateAcknowledged(_nsNoMeta, fromjson("{a: 1}"), fromjson("{$set: {a: 2}}"));
        ASSERT_EQUALS(ErrorCodes::InvalidOptions, getStatusFromWriteCommandReply(updateResponse));
        auto deleteResponse = client.removeAcknowledged(_nsNoMeta, fromjson("{a: 1}"));
        ASSERT_EQUALS(ErrorCodes::InvalidOptions, getStatusFromWriteCommandReply(deleteResponse));
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
