// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/oid.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_metadata.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <cstdint>

[[MONGO_MOD_PUBLIC]];
namespace mongo::timeseries::bucket_catalog {

/**
 * Key to lookup open Bucket for collection and metadata, with pre-computed hash.
 */
struct BucketKey {
    using Hash = std::size_t;
    using Signature = std::uint32_t;

    BucketKey() = delete;
    BucketKey(const UUID& collectionUUID, BucketMetadata meta);

    UUID collectionUUID;
    BucketMetadata metadata;
    Hash hash;

    bool operator==(const BucketKey& other) const {
        return collectionUUID == other.collectionUUID && metadata == other.metadata;
    }
    bool operator!=(const BucketKey& other) const {
        return !(*this == other);
    }

    Signature signature() const {
        return signature(hash);
    }

    static Signature signature(BucketKey::Hash keyHash) {
        return static_cast<Signature>(keyHash & 0xFFFFFFFFull);
    }

    template <typename H>
    friend H AbslHashValue(H h, const BucketKey& key) {
        return H::combine(std::move(h), key.collectionUUID, key.metadata);
    }
};

/**
 * Unique identifier for a bucket, with pre-computed hash.
 */
struct BucketId {
    using Hash = std::size_t;

    BucketId() = delete;
    BucketId(const UUID& collectionUUID, const OID& oid, BucketKey::Signature keySignature);

    UUID collectionUUID;
    OID oid;
    BucketKey::Signature keySignature;
    Hash hash;

    bool operator==(const BucketId& other) const {
        return oid == other.oid && keySignature == other.keySignature &&
            collectionUUID == other.collectionUUID;
    }
    bool operator!=(const BucketId& other) const {
        return !(*this == other);
    }
    bool operator<(const BucketId& other) const {
        return (oid < other.oid) || ((oid == other.oid) && (keySignature < other.keySignature)) ||
            ((oid == other.oid) && (keySignature == other.keySignature) &&
             (collectionUUID < other.collectionUUID));
    }

    template <typename H>
    friend H AbslHashValue(H h, const BucketId& bucketId) {
        return H::combine(
            std::move(h), bucketId.oid, bucketId.keySignature, bucketId.collectionUUID);
    }
};

/**
 * Hasher to support pre-computed hash lookup for BucketKey.
 */
struct BucketHasher {
    std::size_t operator()(const BucketKey& key) const;
    std::size_t operator()(const BucketId& bucketId) const;
    std::size_t operator()(const BucketKey::Hash& key) const;
};

}  // namespace mongo::timeseries::bucket_catalog
