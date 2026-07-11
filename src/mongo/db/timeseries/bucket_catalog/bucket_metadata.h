// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"
#include "mongo/util/shared_buffer.h"
#include "mongo/util/tracking/allocator.h"
#include "mongo/util/tracking/context.h"

#include <string_view>

#include <absl/hash/hash.h>
#include <absl/strings/string_view.h>
#include <boost/optional/optional.hpp>

[[MONGO_MOD_PUBLIC]];
namespace mongo::timeseries::bucket_catalog {

struct BucketMetadata {
public:
    BucketMetadata(tracking::Context&,
                   BSONElement elem,
                   boost::optional<std::string_view> trueMetaFieldName);

    bool operator==(const BucketMetadata& other) const;

    BSONObj toBSON() const;
    BSONElement element() const;

    boost::optional<std::string_view> getMetaField() const;

    template <typename H>
    friend H AbslHashValue(H h, const BucketMetadata& metadata) {
        return H::combine(
            std::move(h),
            absl::Hash<absl::string_view>()(absl::string_view(
                metadata._metadataElement.value(), metadata._metadataElement.valuesize())));
    }

private:
    // Empty if metadata field isn't present, owns a copy otherwise.
    allocator_aware::SharedBuffer<tracking::Allocator<void>> _metadata;

    // Only the value of '_metadataElement' is used for hashing and comparison.
    BSONElement _metadataElement;
};

}  // namespace mongo::timeseries::bucket_catalog
