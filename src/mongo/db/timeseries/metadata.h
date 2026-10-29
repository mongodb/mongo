// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/optional/optional.hpp>

[[MONGO_MOD_PUBLIC]];

namespace mongo::timeseries::metadata {

/**
 * Normalize metaField value (i.e. sort object keys) for a time-series measurement so that we can
 * more effectively match a measurement to an existing bucket. If 'as' is specified, the normalized
 * value will be added to 'builder' with the specified field name; otherwise it will be added with
 * its original field name.
 */
template <class Allocator>
void normalize(const BSONElement& elem,
               allocator_aware::BSONObjBuilder<Allocator>& builder,
               boost::optional<std::string_view> as = boost::none);
/**
 * Returns whether two BSONElement metadata values are equal to each other, ignoring field order.
 * Field names will be ignored when comparing two BSONArrays. This is for compatibility with current
 * normalize() behavior.
 */
bool areMetadataEqual(const BSONElement& elem1, const BSONElement& elem2);

}  // namespace mongo::timeseries::metadata
