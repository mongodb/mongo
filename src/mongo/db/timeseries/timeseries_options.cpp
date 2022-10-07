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

    // TODO SERVER-67599 run tests for changing between custom granularity to a default one.
    if (mod.getGranularity() && currentOptions.getGranularity()) {
        auto granularity = mod.getGranularity();
        BucketGranularityEnum target = granularity.get();
        auto currentGranularity = currentOptions.getGranularity().get();

        if (target != currentGranularity) {
            if (!isValidTimeseriesGranularityTransition(currentGranularity, target)) {
                return Status{ErrorCodes::InvalidOptions,
                              "Invalid transition for timeseries.granularity. Can only transition "
                              "from 'seconds' to 'minutes' or 'minutes' to 'hours'."};
            }
            newOptions.setGranularity(target);
            newOptions.setBucketMaxSpanSeconds(
                timeseries::getMaxSpanSecondsFromGranularity(target));

            if (feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
                    serverGlobalParams.featureCompatibility))
                newOptions.setBucketRoundingSeconds(
                    timeseries::getBucketRoundingSecondsFromGranularity(target));

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
    // The time field for both options must match.
    if (option1.getTimeField() != option2.getTimeField()) {
        return false;
    }

    // The meta field for both options must match.
    if (option1.getMetaField() != option2.getMetaField()) {
        return false;
    }

    auto const granularity1 = option1.getGranularity();
    auto const granularity2 = option2.getGranularity();

    // We accept granularity as equal if they are the same or if one is
    // BucketGranularityEnum::Seconds while the other one is boost::none
    if (granularity1 != granularity2 &&
        ((!granularity1 && granularity2 != BucketGranularityEnum::Seconds) ||
         (granularity1 != BucketGranularityEnum::Seconds && !granularity2))) {
        return false;
    }

    const auto option1BucketSpan =
        option1.getBucketMaxSpanSeconds().get_value_or(getMaxSpanSecondsFromGranularity(
            granularity1.get_value_or(BucketGranularityEnum::Seconds)));

    const auto option2BucketSpan =
        option2.getBucketMaxSpanSeconds().get_value_or(getMaxSpanSecondsFromGranularity(
            granularity2.get_value_or(BucketGranularityEnum::Seconds)));

    if (option1BucketSpan != option2BucketSpan) {
        return false;
    }

    const auto option1BucketRounding =
        option1.getBucketRoundingSeconds().get_value_or(getBucketRoundingSecondsFromGranularity(
            granularity1.get_value_or(BucketGranularityEnum::Seconds)));

    const auto option2BucketRounding =
        option2.getBucketRoundingSeconds().get_value_or(getBucketRoundingSecondsFromGranularity(
            granularity2.get_value_or(BucketGranularityEnum::Seconds)));

    if (option1BucketRounding != option2BucketRounding) {
        return false;
    }

    return true;
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
    long long roundingSeconds = 0;
    auto granularity = options.getGranularity();
    if (granularity) {
        roundingSeconds = getBucketRoundingSecondsFromGranularity(granularity.get());
    } else {
        roundingSeconds = options.getBucketRoundingSeconds().get_value_or(
            getBucketRoundingSecondsFromGranularity(BucketGranularityEnum::Seconds));
    }

    long long timeSeconds = durationCount<Seconds>(time.toDurationSinceEpoch());
    long long roundedTimeSeconds = (timeSeconds - (timeSeconds % roundingSeconds));
    return Date_t::fromDurationSinceEpoch(Seconds{roundedTimeSeconds});
}

Status validateAndSetBucketingParameters(TimeseriesOptions& timeseriesOptions) {
    auto roundingSeconds = timeseriesOptions.getBucketRoundingSeconds();
    auto maxSpanSeconds = timeseriesOptions.getBucketMaxSpanSeconds();
    auto granularity = timeseriesOptions.getGranularity();

    bool allowSecondsParameters = feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
        serverGlobalParams.featureCompatibility);
    bool maxSpanAndRoundingSecondsSpecified = maxSpanSeconds && roundingSeconds;
    auto maxSpanSecondsFromGranularity =
        getMaxSpanSecondsFromGranularity(granularity.get_value_or(BucketGranularityEnum::Seconds));
    auto roundingSecondsFromGranularity = getBucketRoundingSecondsFromGranularity(
        granularity.get_value_or(BucketGranularityEnum::Seconds));

    if (allowSecondsParameters) {
        if (granularity) {
            if (maxSpanSeconds && maxSpanSeconds != maxSpanSecondsFromGranularity) {
                return Status{
                    ErrorCodes::InvalidOptions,
                    fmt::format("Timeseries 'bucketMaxSpanSeconds' is not configurable to a value "
                                "other than the default of {} for the provided granularity",
                                maxSpanSecondsFromGranularity)};
            }

            if (roundingSeconds && roundingSeconds != roundingSecondsFromGranularity) {
                return Status{
                    ErrorCodes::InvalidOptions,
                    fmt::format("Timeseries 'bucketRoundingSeconds' is not configurable to a value "
                                "other than the default of {} for the provided granularity",
                                roundingSecondsFromGranularity)};
            }

            if (!maxSpanSeconds)
                timeseriesOptions.setBucketMaxSpanSeconds(maxSpanSecondsFromGranularity);

            if (!roundingSeconds)
                timeseriesOptions.setBucketRoundingSeconds(roundingSecondsFromGranularity);

            return Status::OK();
        }

        if (!maxSpanAndRoundingSecondsSpecified && (maxSpanSeconds || roundingSeconds)) {
            return Status{
                ErrorCodes::InvalidOptions,
                "Timeseries 'bucketMaxSpanSeconds' and 'bucketRoundingSeconds' need to be "
                "set alongside each other"};
        }

        if (roundingSeconds != maxSpanSeconds) {
            return Status{ErrorCodes::InvalidOptions,
                          "Timeseries 'bucketRoundingSeconds' needs to be equal to "
                          "'bucketMaxSpanSeconds'"};
        }

        if (!maxSpanSeconds) {
            timeseriesOptions.setBucketMaxSpanSeconds(maxSpanSecondsFromGranularity);
            timeseriesOptions.setBucketRoundingSeconds(roundingSecondsFromGranularity);
            timeseriesOptions.setGranularity(BucketGranularityEnum::Seconds);
        }
    } else {
        if (maxSpanSeconds && maxSpanSecondsFromGranularity != maxSpanSeconds) {
            return Status{
                ErrorCodes::InvalidOptions,
                fmt::format("Timeseries 'bucketMaxSpanSeconds' is not configurable to a value "
                            "other than the default of {} for the provided granularity",
                            maxSpanSecondsFromGranularity)};
        }
        if (!granularity)
            timeseriesOptions.setGranularity(BucketGranularityEnum::Seconds);

        if (!maxSpanSeconds)
            timeseriesOptions.setBucketMaxSpanSeconds(maxSpanSecondsFromGranularity);
    }

    return Status::OK();
}
}  // namespace timeseries
}  // namespace mongo
