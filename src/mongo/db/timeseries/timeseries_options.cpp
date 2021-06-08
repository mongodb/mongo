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

#include "mongo/platform/basic.h"

#include "mongo/db/timeseries/timeseries_options.h"

#include "mongo/db/catalog/collection_catalog.h"

namespace mongo {

namespace timeseries {

boost::optional<TimeseriesOptions> getTimeseriesOptions(OperationContext* opCtx,
                                                        const NamespaceString& nss) {
    auto bucketsNs = nss.makeTimeseriesBucketsNamespace();
    auto bucketsColl =
        CollectionCatalog::get(opCtx)->lookupCollectionByNamespaceForRead(opCtx, bucketsNs);
    if (!bucketsColl) {
        return boost::none;
    }
    return bucketsColl->getTimeseriesOptions();
}

int getMaxSpanSecondsFromGranularity(BucketGranularityEnum granularity) {
    switch (granularity) {
        case BucketGranularityEnum::Seconds:
            // 3600 seconds in an hour
            return 60 * 60;
        case BucketGranularityEnum::Minutes:
            // 1440 minutes in a day
            return 60 * 60 * 24;
        case BucketGranularityEnum::Hours:
            // 720 hours in an average month. Note that this only affects internal bucketing and
            // query optimizations, but users should not depend on or be aware of this estimation.
            return 60 * 60 * 24 * 30;
    }
    MONGO_UNREACHABLE;
}

int getBucketRoundingSecondsFromGranularity(BucketGranularityEnum granularity) {
    switch (granularity) {
        case BucketGranularityEnum::Seconds:
            // Round down to nearest minute.
            return 60;
        case BucketGranularityEnum::Minutes:
            // Round down to nearest hour.
            return 60 * 60;
        case BucketGranularityEnum::Hours:
            // Round down to hearest day.
            return 60 * 60 * 24;
    }
    MONGO_UNREACHABLE;
}
bool optionsAreEqual(const TimeseriesOptions& option1, const TimeseriesOptions& option2) {
    const auto option1BucketSpan = option1.getBucketMaxSpanSeconds()
        ? *option1.getBucketMaxSpanSeconds()
        : getMaxSpanSecondsFromGranularity(option1.getGranularity());
    const auto option2BucketSpan = option2.getBucketMaxSpanSeconds()
        ? *option2.getBucketMaxSpanSeconds()
        : getMaxSpanSecondsFromGranularity(option2.getGranularity());
    return option1.getTimeField() == option1.getTimeField() &&
        option1.getMetaField() == option2.getMetaField() &&
        option1.getGranularity() == option2.getGranularity() &&
        option1BucketSpan == option2BucketSpan;
}

}  // namespace timeseries
}  // namespace mongo
