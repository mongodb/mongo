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

#include "mongo/db/global_catalog/type_chunk_range.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

namespace {

bool allElementsAreMaxKey(const BSONObj& obj) {
    for (auto&& elem : obj) {
        if (elem.type() != BSONType::maxKey) {
            return false;
        }
    }
    return true;
}

}  // namespace

ChunkRange::ChunkRange(BSONObj minKey, BSONObj maxKey)
    : ChunkRangeBase(std::move(minKey), std::move(maxKey)) {
    dassert(SimpleBSONObjComparator::kInstance.evaluate(getMin() < getMax()),
            str::stream() << "Illegal chunk range: " << getMin().toString() << ", "
                          << getMax().toString());
}

ChunkRange ChunkRange::parse(const BSONObj& bsonObject,
                             const IDLParserContext& ctxt,
                             DeserializationContext* dctx) {
    auto object = mongo::idl::preparsedValue<ChunkRange>();
    object.parseProtected(bsonObject, ctxt, dctx);
    uassertStatusOK(validate(object));
    return object;
}

ChunkRange ChunkRange::fromBSON(const BSONObj& obj) {
    return parse(obj, IDLParserContext("ChunkRange"));
}

Status ChunkRange::validate(const ChunkRange& range) {
    return validate(range.getMin(), range.getMax());
}

Status ChunkRange::validate(const BSONObj& minKey, const BSONObj& maxKey) {
    if (minKey.isEmpty()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << ChunkRange::kMinFieldName << " field is empty");
    }

    if (maxKey.isEmpty()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << ChunkRange::kMaxFieldName << " field is empty");
    }

    if (SimpleBSONObjComparator::kInstance.evaluate(minKey >= maxKey)) {
        return {ErrorCodes::BadValue,
                str::stream() << "min: " << minKey << " should be less than max: " << maxKey};
    }

    return Status::OK();
}

Status ChunkRange::validate(const std::vector<BSONObj>& bounds) {
    if (bounds.size() == 0) {
        return Status(ErrorCodes::BadValue, "no bounds were specified");
    }

    if (bounds.size() != 2) {
        return Status(ErrorCodes::BadValue, "only a min and max bound may be specified");
    }

    BSONObj minKey = bounds[0];
    BSONObj maxKey = bounds[1];

    return validate(minKey, maxKey);
}

Status ChunkRange::validateStrict(const ChunkRange& range) {
    auto basicValidation = validate(range);
    if (!basicValidation.isOK()) {
        return basicValidation;
    }

    BSONObjIterator minIt(range.getMin());
    BSONObjIterator maxIt(range.getMax());
    while (minIt.more() && maxIt.more()) {
        BSONElement minElem = minIt.next();
        BSONElement maxElem = maxIt.next();
        if (strcmp(minElem.fieldName(), maxElem.fieldName()) != 0) {
            return {ErrorCodes::BadValue,
                    str::stream() << "min and max don't have matching keys: " << range.getMin()
                                  << ", " << range.getMax()};
        }
    }

    // 'min' and 'max' must share the same fields.
    if (minIt.more() || maxIt.more())
        return {ErrorCodes::BadValue,
                str::stream() << "min and max don't have the same number of keys: "
                              << range.getMin() << ", " << range.getMax()};

    return Status::OK();
}

bool ChunkRange::containsKey(const BSONObj& key) const {
    return isKeyInRange(key, getMin(), getMax());
}

std::string ChunkRange::toString() const {
    return str::stream() << "[" << getMin() << ", " << getMax() << ")";
}

bool ChunkRange::operator==(const ChunkRange& other) const {
    return getMin().woCompare(other.getMin()) == 0 && getMax().woCompare(other.getMax()) == 0;
}

bool ChunkRange::operator!=(const ChunkRange& other) const {
    return !(*this == other);
}

bool ChunkRange::operator<(const ChunkRange& other) const {
    auto minCompare = getMin().woCompare(other.getMin());
    if (minCompare < 0) {
        return true;
    } else if (minCompare == 0 && getMax().woCompare(other.getMax()) < 0) {
        return true;
    }
    return false;
}

bool ChunkRange::covers(ChunkRange const& other) const {
    auto le = [](auto const& a, auto const& b) {
        return a.woCompare(b) <= 0;
    };
    return le(getMin(), other.getMin()) && le(other.getMax(), getMax());
}

boost::optional<ChunkRange> ChunkRange::overlapWith(ChunkRange const& other) const {
    auto le = [](auto const& a, auto const& b) {
        return a.woCompare(b) <= 0;
    };
    if (le(other.getMax(), getMin()) || le(getMax(), other.getMin())) {
        return boost::none;
    }
    return ChunkRange(le(getMin(), other.getMin()) ? other.getMin() : getMin(),
                      le(getMax(), other.getMax()) ? getMax() : other.getMax());
}

bool ChunkRange::overlaps(const ChunkRange& other) const {
    return getMin().woCompare(other.getMax()) < 0 && getMax().woCompare(other.getMin()) > 0;
}

ChunkRange ChunkRange::unionWith(ChunkRange const& other) const {
    auto le = [](auto const& a, auto const& b) {
        return a.woCompare(b) <= 0;
    };
    return ChunkRange(le(getMin(), other.getMin()) ? getMin() : other.getMin(),
                      le(getMax(), other.getMax()) ? other.getMax() : getMax());
}

bool isDocumentKeyInRange(const BSONObj& obj,
                          const BSONObj& min,
                          const BSONObj& max,
                          const BSONObj& shardKeyPattern) {
    ShardKeyPattern shardKey(shardKeyPattern);
    return isDocumentKeyInRange(obj, min, max, shardKey);
}

bool isDocumentKeyInRange(const BSONObj& obj,
                          const BSONObj& min,
                          const BSONObj& max,
                          const ShardKeyPattern& shardKeyPattern) {
    return isKeyInRange(shardKeyPattern.extractShardKeyFromDoc(obj), min, max);
}

bool isKeyInRange(const BSONObj& key, const BSONObj& rangeMin, const BSONObj& rangeMax) {
    return (rangeMin.woCompare(key) <= 0 && key.woCompare(rangeMax) < 0) ||
        MONGO_unlikely(allElementsAreMaxKey(key) && key.binaryEqual(rangeMax));
}

}  // namespace mongo
