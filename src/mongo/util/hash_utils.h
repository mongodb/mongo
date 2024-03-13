/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <absl/hash/hash.h>
#include <boost/optional.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/hasher.h"

namespace mongo {
/**
 * StringData's hash function compatible with absl::Hash.
 */
template <typename H>
H AbslHashValue(H h, StringData sd) {
    if (sd.empty()) {
        return H::combine(std::move(h), 9081739);
    }

    return H::combine_contiguous(std::move(h), sd.rawData(), sd.size());
}
}  // namespace mongo

namespace boost {
/**
 * boost::optional's hash function compatible with absl::Hash.
 */
template <typename H, typename T>
H AbslHashValue(H h, const optional<T>& value) {
    size_t constexpr seed = 39916801;  // Some arbitrary number representing boost::optional.
    if (value) {
        return H::combine(std::move(h), seed, *value);
    } else {
        return H::combine(std::move(h), seed);
    }
}
}  // namespace boost
