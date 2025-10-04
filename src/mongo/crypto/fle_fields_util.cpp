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

#include "mongo/crypto/fle_fields_util.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <limits>

#include <boost/optional/optional.hpp>

namespace mongo {
void validateIDLFLE2EncryptionPlaceholder(const FLE2EncryptionPlaceholder* placeholder) {
    if (placeholder->getAlgorithm() == Fle2AlgorithmInt::kRange) {
        if (placeholder->getType() == Fle2PlaceholderType::kFind) {
            auto val = placeholder->getValue().getElement();
            uassert(6720200, "Range Find placeholder value must be an object.", val.isABSONObj());
            auto obj = val.Obj();
            FLE2RangeFindSpec::parse(obj, IDLParserContext("v"));
            uassert(6832501,
                    "Sparsity must be defined for range placeholders.",
                    placeholder->getSparsity());
        } else if (placeholder->getType() == Fle2PlaceholderType::kInsert) {
            auto val = placeholder->getValue().getElement();
            uassert(6775321, "Range Insert placeholder value must be an object.", val.isABSONObj());
            auto obj = val.Obj();
            FLE2RangeInsertSpec::parse(obj, IDLParserContext("v"));
            uassert(6775322,
                    "Sparsity must be defined for range placeholders.",
                    placeholder->getSparsity());
        }
    } else if (placeholder->getAlgorithm() == Fle2AlgorithmInt::kTextSearch) {
        auto val = placeholder->getValue().getElement();
        uassert(9783505, "Text Search placeholder value must be an object.", val.isABSONObj());
        FLE2TextSearchInsertSpec::parse(val.Obj(), IDLParserContext("FLE2TextSearchInsertSpec"));
    }

    uassert(6832500,
            "Hypergraph sparsity can only be set for range placeholders.",
            placeholder->getAlgorithm() == Fle2AlgorithmInt::kRange || !placeholder->getSparsity());
}

bool isInfinite(ImplicitValue val) {
    constexpr auto inf = std::numeric_limits<double>::infinity();
    if (val.getType() != BSONType::numberDouble) {
        return false;
    }
    auto num = val.getDouble();
    return num == inf || num == -inf;
}
namespace {
bool isWithinInt(int64_t num) {
    return num <= std::numeric_limits<int32_t>::max() && num >= std::numeric_limits<int32_t>::min();
}
}  // namespace

void validateQueryBounds(BSONType indexType, ImplicitValue lb, ImplicitValue ub) {
    // Bounds of any type might have an infinite endpoint because open-ended bounds are represented
    // with the undefined endpoint as infinity or -infinity.
    switch (indexType) {
        case BSONType::numberInt:
            uassert(
                6901306,
                "If the index type is NumberInt, then lower bound for query must be an int or be a "
                "long that is within the range of int.",
                isInfinite(lb) || lb.getType() == BSONType::numberInt ||
                    (lb.getType() == BSONType::numberLong && isWithinInt(lb.getLong())));
            uassert(
                6901307,
                "If the index type is NumberInt, then upper bound for query must be an int or be a "
                "long that is within the range of int.",
                isInfinite(ub) || ub.getType() == BSONType::numberInt ||
                    (ub.getType() == BSONType::numberLong && isWithinInt(ub.getLong())));
            break;
        case BSONType::numberLong:
            uassert(
                6901308,
                "Lower bound for query over NumberLong must be either a NumberLong or NumberInt.",
                isInfinite(lb) || lb.getType() == BSONType::numberLong ||
                    lb.getType() == BSONType::numberInt);
            uassert(
                6901309,
                "Upper bound for query over NumberLong must be either a NumberLong or NumberInt.",
                isInfinite(ub) || ub.getType() == BSONType::numberLong ||
                    ub.getType() == BSONType::numberInt);
            break;
        case BSONType::date:
            uassert(6901310,
                    "Lower bound for query over Date must be a Date.",
                    isInfinite(lb) || lb.getType() == BSONType::date);
            uassert(6901311,
                    "Upper bound for query over Date must be a Date.",
                    isInfinite(ub) || ub.getType() == BSONType::date);
            break;
        case BSONType::numberDouble:
            uassert(6901312,
                    "Lower bound for query over NumberDouble must be a NumberDouble.",
                    lb.getType() == BSONType::numberDouble);
            uassert(6901313,
                    "Upper bound for query over NumberDouble must be a NumberDouble.",
                    ub.getType() == BSONType::numberDouble);
            break;
        case BSONType::numberDecimal:
            uassert(6901314,
                    "Lower bound for query over NumberDecimal must be a NumberDecimal.",
                    isInfinite(lb) || lb.getType() == BSONType::numberDecimal);
            uassert(6901315,
                    "Upper bound for query over NumberDecimal must be a NumberDecimal.",
                    isInfinite(ub) || ub.getType() == BSONType::numberDecimal);
            break;
        default:
            uasserted(6901305,
                      str::stream() << "Index type must be a numeric or date, not: " << indexType);
    }
}

void validateIDLFLE2RangeFindSpec(const FLE2RangeFindSpec* placeholder) {
    if (!placeholder->getEdgesInfo()) {
        return;
    }

    auto& edgesInfo = placeholder->getEdgesInfo().get();

    auto min = edgesInfo.getIndexMin().getElement();
    auto max = edgesInfo.getIndexMax().getElement();
    uassert(6901304, "Range min and range max must be the same type.", min.type() == max.type());

    if (edgesInfo.getPrecision().has_value()) {
        uassert(6967102,
                "Precision can only be set if type is floating point",
                min.type() == BSONType::numberDecimal || min.type() == BSONType::numberDouble);
    }

    if (edgesInfo.getTrimFactor().has_value()) {
        auto tf = edgesInfo.getTrimFactor().value();
        uassert(8574100,
                "Trim factor must be less than the number of bits used to represent the domain.",
                tf == 0 ||
                    static_cast<uint32_t>(tf) <
                        getNumberOfBitsInDomain(
                            min.type(), min, max, edgesInfo.getPrecision().map([](std::int32_t m) {
                                return static_cast<uint32_t>(m);
                            })));
    }

    auto lb = edgesInfo.getLowerBound().getElement();
    auto ub = edgesInfo.getUpperBound().getElement();
    validateQueryBounds(min.type(), lb, ub);
}

void validateIDLFLE2RangeInsertSpec(const FLE2RangeInsertSpec* spec) {
    auto valueType = spec->getValue().getElement().type();
    if (spec->getMinBound().has_value() && spec->getMaxBound().has_value()) {
        auto min = spec->getMinBound()->getElement();
        auto max = spec->getMaxBound()->getElement();
        uassert(
            8574101, "Range min and range max must be the same type.", min.type() == max.type());
        uassert(8574109,
                "Range min and range max must match the type of the element to be inserted.",
                min.type() == valueType);
    }

    if (spec->getTrimFactor().has_value()) {
        uint32_t tf = spec->getTrimFactor().value();
        auto optMin = spec->getMinBound().map([](const auto& e) { return e.getElement(); });
        auto optMax = spec->getMaxBound().map([](const auto& e) { return e.getElement(); });
        uassert(8574103,
                "Trim factor must be less than the number of bits used to represent the domain.",
                tf == 0 ||
                    tf <
                        getNumberOfBitsInDomain(
                            valueType, optMin, optMax, spec->getPrecision().map([](std::int32_t m) {
                                return static_cast<uint32_t>(m);
                            })));
    }

    if (spec->getPrecision().has_value()) {
        uassert(8574102,
                "Precision can only be set if type is floating point",
                valueType == BSONType::numberDecimal || valueType == BSONType::numberDouble);
    }
}

void validateIDLFLE2TextSearchInsertSpec(const FLE2TextSearchInsertSpec* spec) {
    uassert(9783500,
            "Text search insert placeholder must have a substring, suffix, or prefix index "
            "specification",
            spec->getSuffixSpec() || spec->getSubstringSpec() || spec->getPrefixSpec());

    if (spec->getSubstringSpec()) {
        auto subspec = spec->getSubstringSpec().value();
        uassert(9783501,
                "Substring query upper bound length cannot be less than the lower bound",
                subspec.getMinQueryLength() <= subspec.getMaxQueryLength());
        uassert(9783502,
                "Substring maximum indexed length cannot be less than the upper bound",
                subspec.getMaxQueryLength() <= subspec.getMaxLength());
    }
    if (spec->getSuffixSpec()) {
        auto subspec = spec->getSuffixSpec().value();
        uassert(9783503,
                "Suffix query upper bound length cannot be less than the lower bound",
                subspec.getMinQueryLength() <= subspec.getMaxQueryLength());
    }
    if (spec->getPrefixSpec()) {
        auto subspec = spec->getPrefixSpec().value();
        uassert(9783504,
                "Prefix query upper bound length cannot be less than the lower bound",
                subspec.getMinQueryLength() <= subspec.getMaxQueryLength());
    }
}

void validateIDLFLE2FindTextPayload(const FLE2FindTextPayload* spec) {
    auto& ts = spec->getTokenSets();
    int field_count = (ts.getExactTokens() ? 1 : 0) + (ts.getSubstringTokens() ? 1 : 0) +
        (ts.getSuffixTokens() ? 1 : 0) + (ts.getPrefixTokens() ? 1 : 0);
    uassert(
        10163701,
        "FLE2FindTextPayload must have exactly one of exact match, substring, suffix, or prefix "
        "token set",
        field_count == 1);
}
}  // namespace mongo
