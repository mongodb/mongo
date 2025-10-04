/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/type_chunk_range_base_gen.h"
#include "mongo/util/assert_util.h"

namespace mongo {

/**
 * Contains the minimum representation of a chunk - its bounds in the format [min, max) along with
 * utilities for parsing and persistence.
 */
class ChunkRange : private ChunkRangeBase {
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

    friend ChunkRange idlPreparsedValue(stdx::type_identity<ChunkRange>) {
        return {};
    }
};

/**
 * Checks whether the specified key from the given BSON document is within the bounds of this given
 * range.
 */
bool isDocumentKeyInRange(const BSONObj& obj,
                          const BSONObj& min,
                          const BSONObj& max,
                          const ShardKeyPattern& shardKeyPattern);

/**
 * Checks whether the specified key from the given BSON document is within the bounds of this given
 * range.
 */
bool isDocumentKeyInRange(const BSONObj& obj,
                          const BSONObj& min,
                          const BSONObj& max,
                          const BSONObj& shardKeyPattern);

/**
 * Checks whether the specified key is within the bounds of this given range.
 */
bool isKeyInRange(const BSONObj& key, const BSONObj& rangeMin, const BSONObj& rangeMax);

}  // namespace mongo
