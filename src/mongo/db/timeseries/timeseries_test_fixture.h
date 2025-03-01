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

#pragma once

#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/service_context.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/util/uuid.h"

namespace mongo::timeseries {
class TimeseriesTestFixture : public CatalogTestFixture {
public:
    static constexpr uint64_t kDefaultStorageCacheSizeBytes = 1024 * 1024 * 1024;
    static constexpr uint64_t kLimitedStorageCacheSizeBytes = 1024;

protected:
    void setUp() override;
    void tearDown() override;

    virtual std::vector<NamespaceString> getNamespaceStrings();

    virtual BSONObj _makeTimeseriesOptionsForCreate() const;

    TimeseriesOptions _getTimeseriesOptions(const NamespaceString& ns) const;

    const CollatorInterface* _getCollator(const NamespaceString& ns) const;

    struct MeasurementsWithRolloverReasonOptions {
        const bucket_catalog::RolloverReason reason;
        size_t numMeasurements = static_cast<size_t>(gTimeseriesBucketMaxCount);
        size_t idxWithDiffMeasurement = static_cast<size_t>(numMeasurements - 1);
        StringData metaValue = _metaValue;
        Date_t timeValue = Date_t::now();
    };

    std::vector<BSONObj> _generateMeasurementsWithRolloverReason(
        const MeasurementsWithRolloverReasonOptions& options) const;

    uint64_t _getStorageCacheSizeBytes() const;

    std::vector<BSONObj> _getFlattenedVector(const std::vector<std::vector<BSONObj>>& vectors);

    OperationContext* _opCtx;
    bucket_catalog::BucketCatalog* _bucketCatalog;

    static constexpr StringData _timeField = "time";
    static constexpr StringData _metaField = "tag";
    static constexpr StringData _metaValue = "a";
    static constexpr StringData _metaValue2 = "b";
    static constexpr StringData _metaValue3 = "c";
    uint64_t _storageCacheSizeBytes = kDefaultStorageCacheSizeBytes;

    // Strings used to simulate kSize/kCachePressure rollover reason.
    std::string _bigStr = std::string(1000, 'a');

    NamespaceString _ns1 =
        NamespaceString::createNamespaceString_forTest("timeseries_test_fixture_1", "t_1");
    NamespaceString _ns2 =
        NamespaceString::createNamespaceString_forTest("timeseries_test_fixture_1", "t_2");
    NamespaceString _ns3 =
        NamespaceString::createNamespaceString_forTest("timeseries_test_fixture_2", "t_1");

    UUID _uuid1 = UUID::gen();
    UUID _uuid2 = UUID::gen();
    UUID _uuid3 = UUID::gen();

    BSONObj _measurement = BSON(_timeField << Date_t::now() << _metaField << _metaValue);
};
}  // namespace mongo::timeseries
