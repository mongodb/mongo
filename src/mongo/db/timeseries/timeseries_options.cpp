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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/timeseries_gen.h"

namespace mongo {

namespace timeseries {

namespace {

BSONObj wrapInArrayIf(bool doWrap, BSONObj&& obj) {
    if (doWrap) {
        return (::mongo::BSONArrayBuilder() << std::move(obj)).arr();
    }
    return std::move(obj);
}

}  // namespace

bool isValidTimeseriesGranularityTransition(BucketGranularityEnum current,
                                            BucketGranularityEnum target) {
    bool validTransition = true;
    if (current == target) {
        return validTransition;
    }

    switch (current) {
        case BucketGranularityEnum::Seconds: {
            // Both minutes and hours are allowed.
            break;
        }
        case BucketGranularityEnum::Minutes: {
            if (target != BucketGranularityEnum::Hours) {
                validTransition = false;
            }
            break;
        }
        case BucketGranularityEnum::Hours: {
            validTransition = false;
            break;
        }
    }

    return validTransition;
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

StatusWith<std::pair<TimeseriesOptions, bool>> applyTimeseriesOptionsModifications(
    const TimeseriesOptions& currentOptions, const CollModTimeseries& mod) {
    TimeseriesOptions newOptions = currentOptions;
    bool changed = false;

    if (auto granularity = mod.getGranularity()) {
        BucketGranularityEnum target = *granularity;
        if (target != currentOptions.getGranularity()) {
            if (!isValidTimeseriesGranularityTransition(currentOptions.getGranularity(), target)) {
                return Status{ErrorCodes::InvalidOptions,
                              "Invalid transition for timeseries.granularity. Can only transition "
                              "from 'seconds' to 'minutes' or 'minutes' to 'hours'."};
            }
            newOptions.setGranularity(target);
            newOptions.setBucketMaxSpanSeconds(
                timeseries::getMaxSpanSecondsFromGranularity(target));
            changed = true;
        }
    }

    return std::make_pair(newOptions, changed);
}

BSONObj generateViewPipeline(const TimeseriesOptions& options, bool asArray) {
    if (options.getMetaField()) {
        return wrapInArrayIf(
            asArray,
            BSON("$_internalUnpackBucket" << BSON(
                     "timeField" << options.getTimeField() << "metaField" << *options.getMetaField()
                                 << "bucketMaxSpanSeconds" << *options.getBucketMaxSpanSeconds())));
    }
    return wrapInArrayIf(asArray,
                         BSON("$_internalUnpackBucket" << BSON(
                                  "timeField" << options.getTimeField() << "bucketMaxSpanSeconds"
                                              << *options.getBucketMaxSpanSeconds())));
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

int getBucketRoundingSecondsFromGranularity(BucketGranularityEnum granularity) {
    switch (granularity) {
        case BucketGranularityEnum::Seconds:
            // Round down to nearest minute.
            return 60;
        case BucketGranularityEnum::Minutes:
            // Round down to nearest hour.
            return 60 * 60;
        case BucketGranularityEnum::Hours:
            // Round down to nearest day.
            return 60 * 60 * 24;
    }
    MONGO_UNREACHABLE;
}

Date_t roundTimestampToGranularity(const Date_t& time, const TimeseriesOptions& options) {
    auto roundingSeconds = feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
                               serverGlobalParams.featureCompatibility) &&
            options.getBucketRoundingSeconds()
        ? options.getBucketRoundingSeconds().value()
        : getBucketRoundingSecondsFromGranularity(options.getGranularity());
    long long timeSeconds = durationCount<Seconds>(time.toDurationSinceEpoch());
    long long roundedTimeSeconds = (timeSeconds - (timeSeconds % roundingSeconds));
    return Date_t::fromDurationSinceEpoch(Seconds{roundedTimeSeconds});
}
}  // namespace timeseries
}  // namespace mongo
