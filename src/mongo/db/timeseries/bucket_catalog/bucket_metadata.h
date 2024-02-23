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
#include <absl/strings/string_view.h>

#include <boost/optional/optional.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/timeseries/timeseries_tracked_types.h"
#include "mongo/db/timeseries/timeseries_tracking_context.h"

namespace mongo::timeseries::bucket_catalog {

struct BucketMetadata {
public:
    BucketMetadata(BSONElement elem,
                   const StringDataComparator* comparator,
                   boost::optional<StringData> trueMetaFieldName);
    BucketMetadata(TrackingContext&,
                   BSONElement elem,
                   const StringDataComparator* comparator,
                   boost::optional<StringData> trueMetaFieldName);

    bool operator==(const BucketMetadata& other) const;
    bool operator!=(const BucketMetadata& other) const;

    BucketMetadata cloneAsUntracked() const;
    BucketMetadata cloneAsTracked(TrackingContext&) const;

    BSONObj toBSON() const;
    BSONElement element() const;

    StringData getMetaField() const;

    const StringDataComparator* getComparator() const;

    template <typename H>
    friend H AbslHashValue(H h, const BucketMetadata& metadata) {
        return H::combine(
            std::move(h),
            absl::Hash<absl::string_view>()(absl::string_view(
                metadata._metadataElement.value(), metadata._metadataElement.valuesize())));
    }

private:
    BucketMetadata(BSONObj metadata, BSONElement metadataElement, const StringDataComparator*);
    BucketMetadata(TrackingContext&,
                   BSONObj metadata,
                   BSONElement metadataElement,
                   const StringDataComparator*);

    // Empty if metadata field isn't present, owns a copy otherwise.
    std::variant<BSONObj, TrackedBSONObj> _metadata;

    // Only the value of '_metadataElement' is used for hashing and comparison.
    BSONElement _metadataElement;

    const StringDataComparator* _comparator = nullptr;
};

}  // namespace mongo::timeseries::bucket_catalog
