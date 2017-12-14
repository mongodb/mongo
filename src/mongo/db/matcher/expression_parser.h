// expression_parser.h

/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/matcher/expression_with_placeholder.h"
#include "mongo/db/matcher/extensions_callback.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/matcher/schema/expression_internal_schema_allowed_properties.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/stdx/functional.h"

namespace mongo {

class OperationContext;

enum class PathAcceptingKeyword {
    ALL,
    BITS_ALL_CLEAR,
    BITS_ALL_SET,
    BITS_ANY_CLEAR,
    BITS_ANY_SET,
    ELEM_MATCH,
    EQUALITY,
    EXISTS,
    GEO_INTERSECTS,
    GEO_NEAR,
    GREATER_THAN,
    GREATER_THAN_OR_EQUAL,
    INTERNAL_EXPR_EQ,
    INTERNAL_SCHEMA_ALL_ELEM_MATCH_FROM_INDEX,
    INTERNAL_SCHEMA_EQ,
    INTERNAL_SCHEMA_FMOD,
    INTERNAL_SCHEMA_MATCH_ARRAY_INDEX,
    INTERNAL_SCHEMA_MAX_ITEMS,
    INTERNAL_SCHEMA_MAX_LENGTH,
    INTERNAL_SCHEMA_MIN_ITEMS,
    INTERNAL_SCHEMA_MIN_LENGTH,
    INTERNAL_SCHEMA_OBJECT_MATCH,
    INTERNAL_SCHEMA_TYPE,
    INTERNAL_SCHEMA_UNIQUE_ITEMS,
    IN_EXPR,
    LESS_THAN,
    LESS_THAN_OR_EQUAL,
    MOD,
    NOT_EQUAL,
    NOT_IN,
    OPTIONS,
    REGEX,
    SIZE,
    TYPE,
    WITHIN,
};

class MatchExpressionParser {
public:
    /**
     * Features allowed in match expression parsing.
     */
    enum AllowedFeatures {
        kText = 1,
        kGeoNear = 1 << 1,
        kJavascript = 1 << 2,
        kExpr = 1 << 3,
        kJSONSchema = 1 << 4,
        kIsolated = 1 << 5,
    };
    using AllowedFeatureSet = unsigned long long;
    static constexpr AllowedFeatureSet kBanAllSpecialFeatures = 0;
    static constexpr AllowedFeatureSet kAllowAllSpecialFeatures =
        std::numeric_limits<unsigned long long>::max();
    static constexpr AllowedFeatureSet kDefaultSpecialFeatures =
        AllowedFeatures::kExpr | AllowedFeatures::kJSONSchema;

    /**
     * Constant double representation of 2^63.
     */
    static const double kLongLongMaxPlusOneAsDouble;

    /**
     * Parses PathAcceptingKeyword from 'typeElem'. Returns 'defaultKeyword' if 'typeElem'
     * doesn't represent a known type, or represents PathAcceptingKeyword::EQUALITY which is not
     * handled by this parser (see SERVER-19565).
     */
    static boost::optional<PathAcceptingKeyword> parsePathAcceptingKeyword(
        BSONElement typeElem, boost::optional<PathAcceptingKeyword> defaultKeyword = boost::none);

    /**
     * Caller has to maintain ownership of 'obj'.
     * The tree has views (BSONElement) into 'obj'.
     */
    static StatusWithMatchExpression parse(
        const BSONObj& obj,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const ExtensionsCallback& extensionsCallback = ExtensionsCallbackNoop(),
        AllowedFeatureSet allowedFeatures = kDefaultSpecialFeatures);

    /**
     * Parses a BSONElement of any numeric type into a positive long long, failing if the value
     * is any of the following:
     *
     * - NaN.
     * - Negative.
     * - A floating point number which is not integral.
     * - Too large to fit within a 64-bit signed integer.
     */
    static StatusWith<long long> parseIntegerElementToNonNegativeLong(BSONElement elem);

    /**
     * Parses a BSONElement of any numeric type into a long long, failing if the value
     * is any of the following:
     *
     * - NaN.
     * - A floating point number which is not integral.
     * - Too large in the positive or negative direction to fit within a 64-bit signed integer.
     */
    static StatusWith<long long> parseIntegerElementToLong(BSONElement elem);

    /**
     * Parses a BSONElement of any numeric type into an integer, failing if the value is:
     *
     * - NaN
     * - a non-integral number
     * - too large in the positive or negative direction to fit in an int
     */
    static StatusWith<int> parseIntegerElementToInt(BSONElement elem);
};
}  // namespace mongo
