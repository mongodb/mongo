/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"

#include <boost/optional/optional.hpp>

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
               boost::optional<StringData> as = boost::none);
/**
 * Returns whether two BSONElement metadata values are equal to each other, ignoring field order.
 * Field names will be ignored when comparing two BSONArrays. This is for compatibility with current
 * normalize() behavior.
 */
bool areMetadataEqual(const BSONElement& elem1, const BSONElement& elem2);

}  // namespace mongo::timeseries::metadata
