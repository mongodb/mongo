// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/type_chunk_range_base_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Contains the minimum representation of a chunk - its bounds in the format [min, max) along with
 * utilities for parsing and persistence.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ChunkRange : private ChunkRangeBase {
public:
    using ChunkRangeBase::kMaxFieldName;
    using ChunkRangeBase::kMinFieldName;

    using ChunkRangeBase::getMax;
    using ChunkRangeBase::getMin;
    using ChunkRangeBase::serialize;
    using ChunkRangeBase::toBSON;

    ChunkRange(BSONObj minKey, BSONObj maxKey);

    /**
     * Factory function that parses a ChunkRange from a BSONObj.
     */
    static ChunkRange parse(const BSONObj& bsonObject,
                            const IDLParserContext& ctxt,
                            DeserializationContext* dctx = nullptr);

    static ChunkRange fromBSON(const BSONObj& obj);

    static Status validate(const ChunkRange& chunkRange);
    static Status validate(const BSONObj& minKey, const BSONObj& maxKey);
    static Status validate(const std::vector<BSONObj>& bounds);
    static Status validateStrict(const ChunkRange& range);

    /**
     * Checks whether the specified key is within the bounds of this chunk range.
     */
    bool containsKey(const BSONObj& key) const;

    std::string toString() const;

    /**
     * Returns true if two chunk ranges match exactly in terms of the min and max keys (including
     * element order within the keys).
     */
    bool operator==(const ChunkRange& other) const;
    bool operator!=(const ChunkRange& other) const;

    /**
     * Returns true if either min is less than rhs min, or in the case that min == rhs min, true if
     * max is less than rhs max. Otherwise returns false.
     */
    bool operator<(const ChunkRange& rhs) const;

    /**
     * Returns true iff the union of *this and the argument range is the same as *this.
     */
    bool covers(ChunkRange const& other) const;

    /**
     * Returns the range of overlap between *this and other, if any.
     */
    boost::optional<ChunkRange> overlapWith(ChunkRange const& other) const;

    /**
     * Returns true if there is any overlap between the two ranges.
     */
    bool overlaps(const ChunkRange& other) const;

    /**
     * Returns a range that includes *this and other. If the ranges do not overlap, it includes
     * all the space between, as well.
     */
    ChunkRange unionWith(ChunkRange const& other) const;

private:
    /** Does not enforce the non-empty range invariant. */
    ChunkRange() = default;

    friend ChunkRange idlPreparsedValue(std::type_identity<ChunkRange>) {
        return {};
    }
};

/**
 * Checks whether the specified key from the given BSON document is within the bounds of this given
 * range.
 */
[[MONGO_MOD_UNFORTUNATELY_OPEN]] bool isDocumentKeyInRange(const BSONObj& obj,
                                                           const BSONObj& min,
                                                           const BSONObj& max,
                                                           const ShardKeyPattern& shardKeyPattern);

/**
 * Checks whether the specified key from the given BSON document is within the bounds of this given
 * range.
 */
[[MONGO_MOD_UNFORTUNATELY_OPEN]] bool isDocumentKeyInRange(const BSONObj& obj,
                                                           const BSONObj& min,
                                                           const BSONObj& max,
                                                           const BSONObj& shardKeyPattern);

/**
 * Checks whether the specified key is within the bounds of this given range.
 */
[[MONGO_MOD_UNFORTUNATELY_OPEN]] bool isKeyInRange(const BSONObj& key,
                                                   const BSONObj& rangeMin,
                                                   const BSONObj& rangeMax);

}  // namespace mongo
