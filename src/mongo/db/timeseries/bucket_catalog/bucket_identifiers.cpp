// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/timeseries/bucket_catalog/bucket_identifiers.h"

#include <absl/hash/hash.h>

namespace mongo::timeseries::bucket_catalog {

BucketId::BucketId(const UUID& u, const OID& o, BucketKey::Signature k)
    : collectionUUID{u}, oid{o}, keySignature{k}, hash{absl::Hash<BucketId>{}(*this)} {}

BucketKey::BucketKey(const UUID& u, BucketMetadata m)
    : collectionUUID(u), metadata(std::move(m)), hash(absl::Hash<BucketKey>{}(*this)) {}

std::size_t BucketHasher::operator()(const BucketKey& key) const {
    return key.hash;
}

std::size_t BucketHasher::operator()(const BucketId& bucketId) const {
    return bucketId.hash;
}

std::size_t BucketHasher::operator()(const BucketKey::Hash& key) const {
    return key;
}

}  // namespace mongo::timeseries::bucket_catalog
