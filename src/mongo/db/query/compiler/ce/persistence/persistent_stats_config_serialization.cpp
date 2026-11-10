// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/persistence/persistent_stats_config_serialization.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {

void sampleSizeUpdateSerializeToBSON(const SampleSizeUpdate& sampleSize,
                                     std::string_view fieldName,
                                     BSONObjBuilder* builder) {
    if (const auto* setSampleSize = std::get_if<SetSampleSize>(&sampleSize)) {
        builder->appendNumber(fieldName, setSampleSize->value);
    } else {
        builder->appendNull(fieldName);
    }
}

SampleSizeUpdate sampleSizeUpdateParseFromBSON(const BSONElement& elem) {
    if (elem.type() == BSONType::null || elem.type() == BSONType::undefined) {
        return RemoveSampleSize{};
    }

    const std::int32_t sampleSize = elem.safeNumberInt();
    uassert(ErrorCodes::BadValue,
            str::stream() << "'persistentStatsConfig.samples." << elem.fieldNameStringData()
                          << "' must be greater than 0",
            sampleSize > 0);

    return SetSampleSize{sampleSize};
}

}  // namespace mongo
