// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/timeseries/bucket_catalog/bucket_metadata.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/timeseries/metadata.h"

#include <string_view>

namespace mongo::timeseries::bucket_catalog {

BucketMetadata::BucketMetadata(tracking::Context& trackingContext,
                               BSONElement elem,
                               boost::optional<std::string_view> trueMetaFieldName)
    : _metadata([&] {
          if (!elem) {
              return allocator_aware::SharedBuffer<tracking::Allocator<void>>{
                  trackingContext.makeAllocator<void>()};
          }

          allocator_aware::BSONObjBuilder<tracking::Allocator<void>> builder{
              trackingContext.makeAllocator<void>()};
          // We will get an object of equal size, just with reordered fields.
          builder.bb().reserveBytes(elem.size());
          metadata::normalize(elem, builder, trueMetaFieldName);
          builder.doneFast();
          return builder.bb().release();
      }()),
      _metadataElement(toBSON().firstElement()) {}

bool BucketMetadata::operator==(const BucketMetadata& other) const {
    return _metadataElement.binaryEqualValues(other._metadataElement);
}

BSONObj BucketMetadata::toBSON() const {
    return _metadata ? BSONObj{_metadata.get()} : BSONObj{};
}

BSONElement BucketMetadata::element() const {
    return _metadataElement;
}

boost::optional<std::string_view> BucketMetadata::getMetaField() const {
    return _metadataElement ? boost::make_optional(_metadataElement.fieldNameStringData())
                            : boost::none;
}

}  // namespace mongo::timeseries::bucket_catalog
