// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <string_view>
#include <variant>

namespace mongo {

// Sets the sample size of the persistent samples of a collection.
struct [[MONGO_MOD_PUBLIC]] SetSampleSize {
    std::int32_t value;
};

// Removes the sample size of the persistent samples of a collection.
struct [[MONGO_MOD_PUBLIC]] RemoveSampleSize {};

// The value of a sample size field in a 'persistentStatsConfig' update: either a new sample size
// or the request to remove it. IDL cannot distinguish an explicit null from an omitted field, so
// the field is a custom IDL type; the enclosing boost::optional tracks whether it was specified.
using SampleSizeUpdate = std::variant<SetSampleSize, RemoveSampleSize>;

[[MONGO_MOD_PUBLIC]]
void sampleSizeUpdateSerializeToBSON(const SampleSizeUpdate& sampleSize,
                                     std::string_view fieldName,
                                     BSONObjBuilder* builder);

[[MONGO_MOD_PUBLIC]]
SampleSizeUpdate sampleSizeUpdateParseFromBSON(const BSONElement& elem);

}  // namespace mongo
