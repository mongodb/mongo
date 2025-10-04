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

#include "mongo/bson/oid.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_metadata.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <cstdint>

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
