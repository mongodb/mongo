/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/pipeline/abt/utils.h"

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <limits>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/docval_to_sbeval.h"
#include "mongo/db/query/optimizer/algebra/operator.h"
#include "mongo/db/query/optimizer/algebra/polyvalue.h"
#include "mongo/db/query/optimizer/bool_expression.h"
#include "mongo/db/query/optimizer/syntax/expr.h"
#include "mongo/db/query/optimizer/syntax/path.h"
#include "mongo/db/query/optimizer/utils/path_utils.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"

namespace mongo::optimizer {

ABT translateFieldPath(const FieldPath& fieldPath,
                       ABT initial,
                       const ABTFieldNameFn& fieldNameFn,
                       const size_t skipFromStart) {
    ABT result = std::move(initial);

    const size_t fieldPathLength = fieldPath.getPathLength();
    bool isLastElement = true;
    for (size_t i = fieldPathLength; i-- > skipFromStart;) {
        result = fieldNameFn(
            FieldNameType{fieldPath.getFieldName(i).toString()}, isLastElement, std::move(result));
        isLastElement = false;
    }

    return result;
}

ABT translateFieldRef(const FieldRef& fieldRef, ABT initial) {
    ABT result = std::move(initial);

    const size_t fieldPathLength = fieldRef.numParts();

    // Handle empty field paths separately.
    if (fieldPathLength == 0) {
        return make<PathGet>("", std::move(result));
    }

    for (size_t i = fieldPathLength; i-- > 0;) {
        // A single empty field path will parse to a FieldRef with 0 parts but should
        // logically be considered a single part with an empty string.
        if (i != fieldPathLength - 1) {
            // For field paths with empty elements such as 'x.', we should traverse the
            // array 'x' but not reach into any sub-objects. So a predicate such as {'x.':
            // {$eq: 5}} should match {x: [5]} and {x: {"": 5}} but not {x: [{"": 5}]}.
            const bool trailingEmptyPath =
                (fieldPathLength >= 2u && i == fieldPathLength - 2u) && (fieldRef[i + 1] == ""_sd);
            if (trailingEmptyPath) {
                auto arrCase = make<PathArr>();
                maybeComposePath(arrCase, result.cast<PathGet>()->getPath());
                maybeComposePath<PathComposeA>(result, std::move(arrCase));
            } else {
                result = make<PathTraverse>(PathTraverse::kSingleLevel, std::move(result));
            }
        }
        result = make<PathGet>(FieldNameType{fieldRef[i].toString()}, std::move(result));
    }

    return result;
}

ABT translateShardKeyField(std::string shardKey) {
    auto abt = make<PathIdentity>();
    size_t curPos = 0;
    size_t nextPos = 0;
    while (nextPos != std::string::npos) {
        nextPos = shardKey.find('.', curPos);
        abt =
            make<PathGet>(FieldNameType{shardKey.substr(curPos, nextPos - curPos)}, std::move(abt));
        curPos = nextPos + 1;
    }
    return abt;
}

std::pair<boost::optional<ABT>, bool> getMinMaxBoundForType(const bool isMin,
                                                            const sbe::value::TypeTags& tag) {
    switch (tag) {
        case sbe::value::TypeTags::NumberInt32:
        case sbe::value::TypeTags::NumberInt64:
        case sbe::value::TypeTags::NumberDouble:
        case sbe::value::TypeTags::NumberDecimal:
            if (isMin) {
                return {Constant::fromDouble(std::numeric_limits<double>::quiet_NaN()), false};
            } else {
                return {Constant::str(""), false};
            }

        case sbe::value::TypeTags::StringSmall:
        case sbe::value::TypeTags::StringBig:
        case sbe::value::TypeTags::bsonString:
        case sbe::value::TypeTags::bsonSymbol:
            if (isMin) {
                return {Constant::str(""), true};
            } else {
                return {Constant::emptyObject(), false};
            }

        case sbe::value::TypeTags::Date:
            if (isMin) {
                return {Constant::date(Date_t::min()), true};
            } else {
                return {Constant::date(Date_t::max()), true};
            }

        case sbe::value::TypeTags::Timestamp:
            if (isMin) {
                return {Constant::timestamp(Timestamp::min()), true};
            } else {
                return {Constant::timestamp(Timestamp::max()), true};
            }

        case sbe::value::TypeTags::Null:
            return {Constant::null(), true};

        case sbe::value::TypeTags::bsonUndefined: {
            const auto [tag1, val1] = sbe::value::makeValue(Value(BSONUndefined));
            return {make<Constant>(sbe::value::TypeTags::bsonUndefined, val1), true};
        }

        case sbe::value::TypeTags::Object:
        case sbe::value::TypeTags::bsonObject:
            if (isMin) {
                return {Constant::emptyObject(), true};
            } else {
                return {Constant::emptyArray(), false};
            }

        case sbe::value::TypeTags::Array:
        case sbe::value::TypeTags::ArraySet:
        case sbe::value::TypeTags::bsonArray:
            if (isMin) {
                return {Constant::array(), true};
            } else {
                const auto [tag1, val1] = sbe::value::makeValue(Value(BSONBinData()));
                return {make<Constant>(sbe::value::TypeTags::bsonBinData, val1), false};
            }

        case sbe::value::TypeTags::bsonBinData:
            if (isMin) {
                const auto [tag1, val1] = sbe::value::makeValue(Value(BSONBinData()));
                return {make<Constant>(sbe::value::TypeTags::bsonBinData, val1), true};
            } else {
                auto [tag1, val1] = sbe::value::makeValue(Value(OID()));
                return {make<Constant>(sbe::value::TypeTags::ObjectId, val1), false};
            }

        case sbe::value::TypeTags::Boolean:
            if (isMin) {
                return {Constant::boolean(false), true};
            } else {
                return {Constant::boolean(true), true};
            }

        case sbe::value::TypeTags::ObjectId:
        case sbe::value::TypeTags::bsonObjectId:
            if (isMin) {
                const auto [tag1, val1] = sbe::value::makeValue(Value(OID()));
                return {make<Constant>(sbe::value::TypeTags::ObjectId, val1), true};
            } else {
                const auto [tag1, val1] = sbe::value::makeValue(Value(OID::max()));
                return {make<Constant>(sbe::value::TypeTags::ObjectId, val1), true};
            }

        case sbe::value::TypeTags::bsonRegex:
            if (isMin) {
                const auto [tag1, val1] = sbe::value::makeValue(Value(BSONRegEx("", "")));
                return {make<Constant>(sbe::value::TypeTags::bsonRegex, val1), true};
            } else {
                const auto [tag1, val1] = sbe::value::makeValue(Value(BSONDBRef()));
                return {make<Constant>(sbe::value::TypeTags::bsonDBPointer, val1), false};
            }

        case sbe::value::TypeTags::bsonDBPointer:
            if (isMin) {
                const auto [tag1, val1] = sbe::value::makeValue(Value(BSONDBRef()));
                return {make<Constant>(sbe::value::TypeTags::bsonDBPointer, val1), true};
            } else {
                const auto [tag1, val1] = sbe::value::makeCopyBsonJavascript(StringData(""));
                return {make<Constant>(sbe::value::TypeTags::bsonJavascript, val1), false};
            }

        case sbe::value::TypeTags::bsonJavascript:
            if (isMin) {
                const auto [tag1, val1] = sbe::value::makeCopyBsonJavascript(StringData(""));
                return {make<Constant>(sbe::value::TypeTags::bsonJavascript, val1), true};
            } else {
                const auto [tag1, val1] = sbe::value::makeValue(Value(BSONCodeWScope()));
                return {make<Constant>(sbe::value::TypeTags::bsonCodeWScope, val1), false};
            }

        case sbe::value::TypeTags::bsonCodeWScope:
            if (isMin) {
                const auto [tag1, val1] = sbe::value::makeValue(Value(BSONCodeWScope()));
                return {make<Constant>(sbe::value::TypeTags::bsonCodeWScope, val1), true};
            } else {
                return {Constant::maxKey(), false};
            }

        default:
            return {boost::none, false};
    }

    MONGO_UNREACHABLE;
}

class PathToIntervalTransport {
public:
    using ResultType = boost::optional<IntervalReqExpr::Node>;

    PathToIntervalTransport() {}

    template <sbe::value::TypeTags tag>
    ResultType getBoundsForNode() {
        auto [lowBound, lowInclusive] = getMinMaxBoundForType(true /*isMin*/, tag);
        invariant(lowBound);

        auto [highBound, highInclusive] = getMinMaxBoundForType(false /*isMin*/, tag);
        invariant(highBound);

        return IntervalReqExpr::makeSingularDNF(IntervalRequirement{
            {lowInclusive, std::move(*lowBound)}, {highInclusive, std::move(*highBound)}});
    }

    ResultType transport(const ABT& /*n*/, const PathArr& /*node*/) {
        return getBoundsForNode<sbe::value::TypeTags::Array>();
    }

    ResultType transport(const ABT& /*n*/, const PathObj& /*node*/) {
        return getBoundsForNode<sbe::value::TypeTags::Object>();
    }

    template <typename T, typename... Ts>
    ResultType transport(const ABT& /*n*/, const T& /*node*/, Ts&&...) {
        return {};
    }

    ResultType convert(const ABT& path) {
        return algebra::transport<true>(path, *this);
    }
};

boost::optional<IntervalReqExpr::Node> defaultConvertPathToInterval(const ABT& node) {
    return PathToIntervalTransport{}.convert(node);
}

}  // namespace mongo::optimizer
