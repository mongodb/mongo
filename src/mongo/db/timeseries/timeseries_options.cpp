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

Status isTimeseriesGranularityValidAndUnchanged(const TimeseriesOptions& currentOptions,
                                                const CollModTimeseries& targetOptions,
                                                bool* shouldUpdateOptions) {
    auto currentGranularity = currentOptions.getGranularity();
    auto targetGranularity = targetOptions.getGranularity();
    bool allowSecondsParameters = feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
        serverGlobalParams.featureCompatibility);

    if (shouldUpdateOptions) {
        *shouldUpdateOptions = true;
    }

    int32_t targetMaxSpanSeconds = 0;
    int32_t targetRoundingSeconds = 0;

    if (targetGranularity) {
        // If maxSpanSeconds or roundingSeconds was given along granularity it must be the default
        // value.
        if (targetOptions.getBucketMaxSpanSeconds() &&
            targetOptions.getBucketMaxSpanSeconds() !=
                getMaxSpanSecondsFromGranularity(targetGranularity.get())) {
            return Status{ErrorCodes::InvalidOptions,
                          "Timeseries 'bucketMaxSpanSeconds' is not configurable to a value "
                          "other than the default for the provided granularity"};
        }
        if (targetOptions.getBucketRoundingSeconds() &&
            targetOptions.getBucketRoundingSeconds() !=
                getBucketRoundingSecondsFromGranularity(targetGranularity.get())) {
            return Status{ErrorCodes::InvalidOptions,
                          "Timeseries 'bucketRoundingSeconds' is not configurable to a value "
                          "other than the default for the provided granularity"};
        }

        if (currentGranularity) {
            if (currentGranularity == targetGranularity) {
                if (shouldUpdateOptions)
                    *shouldUpdateOptions = false;
                return Status::OK();
            }
            switch (currentGranularity.get()) {
                case BucketGranularityEnum::Seconds: {
                    return Status::OK();
                }
                case BucketGranularityEnum::Minutes: {
                    if (targetGranularity != BucketGranularityEnum::Hours) {
                        return Status{
                            ErrorCodes::InvalidOptions,
                            "Invalid transition for timeseries.granularity. Can only transition "
                            "from 'seconds' to 'minutes' or 'minutes' to 'hours'."};
                    }
                    return Status::OK();
                }
                case BucketGranularityEnum::Hours: {
                    return Status{
                        ErrorCodes::InvalidOptions,
                        "Invalid transition for timeseries.granularity. Can only transition "
                        "from 'seconds' to 'minutes' or 'minutes' to 'hours'."};
                }
            }
        }

        targetMaxSpanSeconds = getMaxSpanSecondsFromGranularity(targetGranularity.get());
        targetRoundingSeconds = getBucketRoundingSecondsFromGranularity(targetGranularity.get());
    } else if (allowSecondsParameters) {
        if (!targetOptions.getBucketMaxSpanSeconds() || !targetOptions.getBucketRoundingSeconds()) {
            return Status{
                ErrorCodes::InvalidOptions,
                "Timeseries 'bucketMaxSpanSeconds' and 'bucketRoundingSeconds' need to be "
                "set alongside each other"};
        }

        targetMaxSpanSeconds = targetOptions.getBucketMaxSpanSeconds().get();
        targetRoundingSeconds = targetOptions.getBucketRoundingSeconds().get();
    } else {
        return Status{ErrorCodes::InvalidOptions, "No timeseries parameters were given"};
    }

    // Check if we are trying to set the bucketing parameters to the existing values. If so, we do
    // not need to update any existing options.
    if (currentOptions.getBucketMaxSpanSeconds() == targetMaxSpanSeconds) {
        if (!allowSecondsParameters ||
            currentOptions.getBucketRoundingSeconds() == targetRoundingSeconds) {
            if (shouldUpdateOptions)
                *shouldUpdateOptions = false;
            return Status::OK();
        }

        if (allowSecondsParameters && currentGranularity &&
            (targetRoundingSeconds ==
             timeseries::getBucketRoundingSecondsFromGranularity(*currentGranularity))) {
            if (shouldUpdateOptions)
                *shouldUpdateOptions = false;
            return Status::OK();
        }
    }

    if (targetOptions.getBucketMaxSpanSeconds() != targetOptions.getBucketRoundingSeconds()) {
        return Status{ErrorCodes::InvalidOptions,
                      "Timeseries 'bucketRoundingSeconds' needs to be equal to "
                      "'bucketMaxSpanSeconds'"};
    }

    if (currentOptions.getBucketMaxSpanSeconds() > targetMaxSpanSeconds) {
        return Status{
            ErrorCodes::InvalidOptions,
            "Timeseries 'bucketMaxSpanSeconds' needs to be equal or greater to transition"};
    }
    if (allowSecondsParameters &&
        (currentOptions.getBucketRoundingSeconds() > targetRoundingSeconds)) {
        return Status{
            ErrorCodes::InvalidOptions,
            "Timeseries 'bucketRoundingSeconds' needs to be equal or greater to transition"};
    }

    return Status::OK();
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
    bool shouldUpdateOptions = true;
    bool updated = false;
    auto targetGranularity = mod.getGranularity();

    auto isValidTransition =
        isTimeseriesGranularityValidAndUnchanged(currentOptions, mod, &shouldUpdateOptions);
    if (!isValidTransition.isOK()) {
        return isValidTransition;
    }

    if (shouldUpdateOptions) {
        int32_t targetMaxSpanSeconds = 0;
        boost::optional<int32_t> targetRoundingSeconds = boost::none;

        if (targetGranularity) {
            targetMaxSpanSeconds = getMaxSpanSecondsFromGranularity(targetGranularity.get());
            newOptions.setGranularity(targetGranularity.get());
        } else {
            targetMaxSpanSeconds = mod.getBucketMaxSpanSeconds().get();
            targetRoundingSeconds = mod.getBucketRoundingSeconds();
            newOptions.setGranularity(boost::none);
        }

        newOptions.setBucketMaxSpanSeconds(targetMaxSpanSeconds);
        newOptions.setBucketRoundingSeconds(targetRoundingSeconds);
        updated = true;
    }

    return std::make_pair(newOptions, updated);
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

        // If the granularity is specified, do not set the 'bucketRoundingSeconds' field.
        timeseriesOptions.setBucketRoundingSeconds(boost::none);
    } else if (allowSecondsParameters && (maxSpanSeconds || roundingSeconds)) {
        if (!maxSpanAndRoundingSecondsSpecified) {
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
    } else {
        if (maxSpanSeconds && maxSpanSecondsFromGranularity != maxSpanSeconds) {
            return Status{
                ErrorCodes::InvalidOptions,
                fmt::format("Timeseries 'bucketMaxSpanSeconds' is not configurable to a value "
                            "other than the default of {} for the provided granularity",
                            maxSpanSecondsFromGranularity)};
        }
        if (!granularity) {
            timeseriesOptions.setGranularity(BucketGranularityEnum::Seconds);
        }

        if (!maxSpanSeconds) {
            timeseriesOptions.setBucketMaxSpanSeconds(maxSpanSecondsFromGranularity);
        }
    }

    return Status::OK();
}

}  // namespace timeseries
}  // namespace mongo
