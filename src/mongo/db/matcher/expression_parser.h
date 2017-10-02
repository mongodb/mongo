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
#include "mongo/db/matcher/extensions_callback.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
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

    static constexpr StringData kAggExpression = "$expr"_sd;

    /**
     * Parses PathAcceptingKeyword from 'typeElem'. Returns 'defaultKeyword' if 'typeElem'
     * doesn't represent a known type, or represents PathAcceptingKeyword::EQUALITY which is not
     * handled by this parser (see SERVER-19565).
     */
    static boost::optional<PathAcceptingKeyword> parsePathAcceptingKeyword(
        BSONElement typeElem, boost::optional<PathAcceptingKeyword> defaultKeyword = boost::none);

    /**
     * caller has to maintain ownership obj
     * the tree has views (BSONElement) into obj
     */
    static StatusWithMatchExpression parse(
        const BSONObj& obj,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const ExtensionsCallback& extensionsCallback = ExtensionsCallbackNoop(),
        AllowedFeatureSet allowedFeatures = kDefaultSpecialFeatures) {
        invariant(expCtx.get());
        const bool topLevelCall = true;
        return MatchExpressionParser(&extensionsCallback)
            ._parse(obj, expCtx, allowedFeatures, topLevelCall);
    }

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

private:
    MatchExpressionParser(const ExtensionsCallback* extensionsCallback)
        : _extensionsCallback(extensionsCallback) {}

    /**
     * 5 = false
     * { a : 5 } = false
     * { $lt : 5 } = true
     * { $ref: "s", $id: "x" } = false
     * { $ref: "s", $id: "x", $db: "mydb" } = false
     * { $ref : "s" } = false (if incomplete DBRef is allowed)
     * { $id : "x" } = false (if incomplete DBRef is allowed)
     * { $db : "mydb" } = false (if incomplete DBRef is allowed)
     */
    bool _isExpressionDocument(const BSONElement& e, bool allowIncompleteDBRef);

    /**
     * { $ref: "s", $id: "x" } = true
     * { $ref : "s" } = true (if incomplete DBRef is allowed)
     * { $id : "x" } = true (if incomplete DBRef is allowed)
     * { $db : "x" } = true (if incomplete DBRef is allowed)
     */
    bool _isDBRefDocument(const BSONObj& obj, bool allowIncompleteDBRef);

    /**
     * Parse 'obj' and return either a MatchExpression or an error.
     *
     * 'topLevel' indicates whether or not the we are at the top level of the tree across recursive
     * class to this function. This is used to apply special logic at the top level.
     */
    StatusWithMatchExpression _parse(const BSONObj& obj,
                                     const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                     AllowedFeatureSet allowedFeatures,
                                     bool topLevel);

    /**
     * parses a field in a sub expression
     * if the query is { x : { $gt : 5, $lt : 8 } }
     * e is { $gt : 5, $lt : 8 }
     */
    Status _parseSub(const char* name,
                     const BSONObj& obj,
                     AndMatchExpression* root,
                     const boost::intrusive_ptr<ExpressionContext>& expCtx,
                     AllowedFeatureSet allowedFeatures,
                     bool topLevel);

    /**
     * parses a single field in a sub expression
     * if the query is { x : { $gt : 5, $lt : 8 } }
     * e is $gt : 5
     */
    StatusWithMatchExpression _parseSubField(const BSONObj& context,
                                             const AndMatchExpression* andSoFar,
                                             const char* name,
                                             const BSONElement& e,
                                             const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                             AllowedFeatureSet allowedFeatures,
                                             bool topLevel);

    StatusWithMatchExpression _parseComparison(
        const char* name,
        ComparisonMatchExpression* cmp,
        const BSONElement& e,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        AllowedFeatureSet allowedFeatures);

    StatusWithMatchExpression _parseMOD(const char* name, const BSONElement& e);

    StatusWithMatchExpression _parseRegexElement(const char* name, const BSONElement& e);

    StatusWithMatchExpression _parseRegexDocument(const char* name, const BSONObj& doc);

    Status _parseInExpression(InMatchExpression* entries,
                              const BSONObj& theArray,
                              const boost::intrusive_ptr<ExpressionContext>& expCtx);

    template <class T>
    StatusWithMatchExpression _parseType(const char* name, const BSONElement& elt);

    StatusWithMatchExpression _parseGeo(const char* name,
                                        PathAcceptingKeyword type,
                                        const BSONObj& section,
                                        AllowedFeatureSet allowedFeatures);

    StatusWithMatchExpression _parseExpr(BSONElement elem,
                                         AllowedFeatureSet allowedFeatures,
                                         const boost::intrusive_ptr<ExpressionContext>& expCtx);

    // arrays

    StatusWithMatchExpression _parseElemMatch(const char* name,
                                              const BSONElement& e,
                                              const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                              AllowedFeatureSet allowedFeatures,
                                              bool topLevel);

    StatusWithMatchExpression _parseAll(const char* name,
                                        const BSONElement& e,
                                        const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                        AllowedFeatureSet allowedFeatures,
                                        bool topLevel);

    // tree

    Status _parseTreeList(const BSONObj& arr,
                          ListOfMatchExpression* out,
                          const boost::intrusive_ptr<ExpressionContext>& expCtx,
                          AllowedFeatureSet allowedFeatures,
                          bool topLevel);

    StatusWithMatchExpression _parseNot(const char* name,
                                        const BSONElement& e,
                                        const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                        AllowedFeatureSet allowedFeatures,
                                        bool topLevel);

    /**
     * Parses 'e' into a BitTestMatchExpression.
     */
    template <class T>
    StatusWithMatchExpression _parseBitTest(const char* name, const BSONElement& e);

    /**
     * Converts 'theArray', a BSONArray of integers, into a std::vector of integers.
     */
    StatusWith<std::vector<uint32_t>> _parseBitPositionsArray(const BSONObj& theArray);

    StatusWithMatchExpression _parseInternalSchemaFmod(const char* name, const BSONElement& e);

    /**
     * Parses a MatchExpression which takes a fixed-size array of MatchExpressions as arguments.
     */
    template <class T>
    StatusWithMatchExpression _parseInternalSchemaFixedArityArgument(
        StringData name,
        const BSONElement& elem,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        AllowedFeatureSet allowedFeatures);

    /**
     * Parses the given BSONElement into a single integer argument and creates a MatchExpression
     * of type 'T' that gets initialized with the resulting integer.
     */
    template <class T>
    StatusWithMatchExpression _parseInternalSchemaSingleIntegerArgument(
        const char* name, const BSONElement& elem) const;

    /**
     * Same as the  _parseInternalSchemaSingleIntegerArgument function, but for top-level
     * operators which don't have paths.
     */
    template <class T>
    StatusWithMatchExpression _parseTopLevelInternalSchemaSingleIntegerArgument(
        const BSONElement& elem) const;

    /**
     * Parses 'elem' into an InternalSchemaMatchArrayIndexMatchExpression.
     */
    StatusWithMatchExpression _parseInternalSchemaMatchArrayIndex(
        const char* path,
        const BSONElement& elem,
        const boost::intrusive_ptr<ExpressionContext>& expCtx);

    StatusWithMatchExpression _parseInternalSchemaAllowedProperties(
        const BSONElement& elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    // Performs parsing for the match extensions. We do not own this pointer - it has to live
    // as long as the parser is active.
    const ExtensionsCallback* _extensionsCallback;
};
}  // namespace mongo
