// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string>

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Carries parameters for converting index stats on the buckets collection to the time-series
 * collection schema.
 */
struct TimeseriesIndexConversionOptions {
    // The user-supplied timestamp field name specified during time-series collection creation.
    std::string timeField;

    // An optional user-supplied metadata field name specified during time-series collection
    // creation. This field name is used during materialization of metadata fields of a measurement
    // after unpacking.
    boost::optional<std::string> metaField;
};

}  // namespace mongo
