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

namespace mongo::optimizer {

template <bool isConjunction, typename... Args>
ABT generateConjunctionOrDisjunction(Args&... args) {
    ABTVector elements;
    (elements.emplace_back(args), ...);

    if (elements.size() == 0) {
        return Constant::boolean(isConjunction);
    }

    ABT result = std::move(elements.at(0));
    for (size_t i = 1; i < elements.size(); i++) {
        result = make<BinaryOp>(isConjunction ? Operations::And : Operations::Or,
                                std::move(elements.at(i)),
                                std::move(result));
    }
    return result;
}

std::pair<sbe::value::TypeTags, sbe::value::Value> convertFrom(const Value val) {
    // TODO: Either make this conversion unnecessary by changing the value representation in
    // ExpressionConstant, or provide a nicer way to convert directly from Document/Value to
    // sbe::Value.
    BSONObjBuilder bob;
    val.addToBsonObj(&bob, ""_sd);
    auto obj = bob.done();
    auto be = obj.objdata();
    auto end = be + ConstDataView(be).read<LittleEndian<uint32_t>>();
    return sbe::bson::convertFrom<false>(be + 4, end, 0);
}

ABT translateFieldPath(const FieldPath& fieldPath,
                       ABT initial,
                       const ABTFieldNameFn& fieldNameFn,
                       const size_t skipFromStart) {
    ABT result = std::move(initial);

    const size_t fieldPathLength = fieldPath.getPathLength();
    bool isLastElement = true;
    for (size_t i = fieldPathLength; i-- > skipFromStart;) {
        result =
            fieldNameFn(fieldPath.getFieldName(i).toString(), isLastElement, std::move(result));
        isLastElement = false;
    }

    return result;
}

std::pair<boost::optional<ABT>, bool> getMinMaxBoundForType(const bool isMin,
                                                            const sbe::value::TypeTags& tag) {
    switch (tag) {
        case sbe::value::TypeTags::NumberInt32:
        case sbe::value::TypeTags::NumberInt64:
        case sbe::value::TypeTags::NumberDouble:
        case sbe::value::TypeTags::NumberDecimal:
            if (isMin) {
                return {Constant::fromDouble(std::numeric_limits<double>::quiet_NaN()), true};
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
            const auto [tag1, val1] = convertFrom(Value(BSONUndefined));
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
                return {Constant::emptyArray(), true};
            } else {
                const auto [tag1, val1] = convertFrom(Value(BSONBinData()));
                return {make<Constant>(sbe::value::TypeTags::bsonBinData, val1), false};
            }

        case sbe::value::TypeTags::bsonBinData:
            if (isMin) {
                const auto [tag1, val1] = convertFrom(Value(BSONBinData()));
                return {make<Constant>(sbe::value::TypeTags::bsonBinData, val1), true};
            } else {
                auto [tag1, val1] = convertFrom(Value(OID()));
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
                const auto [tag1, val1] = convertFrom(Value(OID()));
                return {make<Constant>(sbe::value::TypeTags::ObjectId, val1), true};
            } else {
                const auto [tag1, val1] = convertFrom(Value(OID::max()));
                return {make<Constant>(sbe::value::TypeTags::ObjectId, val1), true};
            }

        case sbe::value::TypeTags::bsonRegex:
            if (isMin) {
                const auto [tag1, val1] = convertFrom(Value(BSONRegEx("", "")));
                return {make<Constant>(sbe::value::TypeTags::bsonRegex, val1), true};
            } else {
                const auto [tag1, val1] = convertFrom(Value(BSONDBRef()));
                return {make<Constant>(sbe::value::TypeTags::bsonDBPointer, val1), false};
            }

        case sbe::value::TypeTags::bsonDBPointer:
            if (isMin) {
                const auto [tag1, val1] = convertFrom(Value(BSONDBRef()));
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
                const auto [tag1, val1] = convertFrom(Value(BSONCodeWScope()));
                return {make<Constant>(sbe::value::TypeTags::bsonCodeWScope, val1), false};
            }

        case sbe::value::TypeTags::bsonCodeWScope:
            if (isMin) {
                const auto [tag1, val1] = convertFrom(Value(BSONCodeWScope()));
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

    ResultType transport(const ABT& /*n*/, const PathArr& /*node*/) {
        auto [lowBound, lowInclusive] =
            getMinMaxBoundForType(true /*isMin*/, sbe::value::TypeTags::Array);
        invariant(lowBound);

        auto [highBound, highInclusive] =
            getMinMaxBoundForType(false /*isMin*/, sbe::value::TypeTags::Array);
        invariant(highBound);

        return IntervalReqExpr::makeSingularDNF(IntervalRequirement{
            {lowInclusive, std::move(*lowBound)}, {highInclusive, std::move(*highBound)}});
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
