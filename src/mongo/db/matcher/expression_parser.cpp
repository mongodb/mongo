// expression_parser.cpp

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

#include "mongo/platform/basic.h"

#include "mongo/db/matcher/expression_parser.h"

#include <boost/container/flat_set.hpp>
#include <pcrecpp.h>

#include "mongo/base/init.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/matcher/expression_with_placeholder.h"
#include "mongo/db/matcher/schema/expression_internal_schema_all_elem_match_from_index.h"
#include "mongo/db/matcher/schema/expression_internal_schema_allowed_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_cond.h"
#include "mongo/db/matcher/schema/expression_internal_schema_fmod.h"
#include "mongo/db/matcher/schema/expression_internal_schema_match_array_index.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_object_match.h"
#include "mongo/db/matcher/schema/expression_internal_schema_unique_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_xor.h"
#include "mongo/db/matcher/schema/json_schema_parser.h"
#include "mongo/db/namespace_string.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/string_map.h"

namespace {

using namespace mongo;

/**
 * Returns true if subtree contains MatchExpression 'type'.
 */
bool hasNode(const MatchExpression* root, MatchExpression::MatchType type) {
    if (type == root->matchType()) {
        return true;
    }
    for (size_t i = 0; i < root->numChildren(); ++i) {
        if (hasNode(root->getChild(i), type)) {
            return true;
        }
    }
    return false;
}

}  // namespace

namespace mongo {

constexpr StringData MatchExpressionParser::kAggExpression;

using std::string;
using stdx::make_unique;

const double MatchExpressionParser::kLongLongMaxPlusOneAsDouble =
    scalbn(1, std::numeric_limits<long long>::digits);

StatusWithMatchExpression MatchExpressionParser::_parseComparison(
    const char* name,
    ComparisonMatchExpression* cmp,
    const BSONElement& e,
    const CollatorInterface* collator,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    AllowedFeatureSet allowedFeatures) {
    std::unique_ptr<ComparisonMatchExpression> temp(cmp);

    // Non-equality comparison match expressions cannot have
    // a regular expression as the argument (e.g. {a: {$gt: /b/}} is illegal).
    if (MatchExpression::EQ != cmp->matchType() && RegEx == e.type()) {
        mongoutils::str::stream ss;
        ss << "Can't have RegEx as arg to predicate over field '" << name << "'.";
        return {Status(ErrorCodes::BadValue, ss)};
    }

    if (_isAggExpression(e, expCtx)) {
        auto expr = _parseAggExpression(e, expCtx, allowedFeatures);
        if (!expr.isOK()) {
            return expr.getStatus();
        }
        auto s = temp->init(name, expr.getValue());
        if (!s.isOK()) {
            return s;
        }
    } else {
        auto s = temp->init(name, e);
        if (!s.isOK()) {
            return s;
        }
    }

    temp->setCollator(collator);

    return {std::move(temp)};
}

StatusWithMatchExpression MatchExpressionParser::_parseSubField(
    const BSONObj& context,
    const AndMatchExpression* andSoFar,
    const char* name,
    const BSONElement& e,
    const CollatorInterface* collator,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    AllowedFeatureSet allowedFeatures,
    bool topLevel) {
    if (mongoutils::str::equals("$eq", e.fieldName()))
        return _parseComparison(
            name, new EqualityMatchExpression(), e, collator, expCtx, allowedFeatures);

    if (mongoutils::str::equals("$not", e.fieldName())) {
        return _parseNot(name, e, collator, expCtx, allowedFeatures, topLevel);
    }

    auto parseExpMatchType = MatchExpressionParser::parsePathAcceptingKeyword(e);
    if (!parseExpMatchType) {
        // $where cannot be a sub-expression because it works on top-level documents only.
        if (mongoutils::str::equals("$where", e.fieldName())) {
            return {Status(ErrorCodes::BadValue, "$where cannot be applied to a field")};
        }

        return {Status(ErrorCodes::BadValue,
                       mongoutils::str::stream() << "unknown operator: " << e.fieldName())};
    }

    switch (*parseExpMatchType) {
        case PathAcceptingKeyword::LESS_THAN:
            return _parseComparison(
                name, new LTMatchExpression(), e, collator, expCtx, allowedFeatures);
        case PathAcceptingKeyword::LESS_THAN_OR_EQUAL:
            return _parseComparison(
                name, new LTEMatchExpression(), e, collator, expCtx, allowedFeatures);
        case PathAcceptingKeyword::GREATER_THAN:
            return _parseComparison(
                name, new GTMatchExpression(), e, collator, expCtx, allowedFeatures);
        case PathAcceptingKeyword::GREATER_THAN_OR_EQUAL:
            return _parseComparison(
                name, new GTEMatchExpression(), e, collator, expCtx, allowedFeatures);
        case PathAcceptingKeyword::NOT_EQUAL: {
            if (RegEx == e.type()) {
                // Just because $ne can be rewritten as the negation of an
                // equality does not mean that $ne of a regex is allowed. See SERVER-1705.
                return {Status(ErrorCodes::BadValue, "Can't have regex as arg to $ne.")};
            }
            StatusWithMatchExpression s = _parseComparison(
                name, new EqualityMatchExpression(), e, collator, expCtx, allowedFeatures);
            if (!s.isOK())
                return s;
            std::unique_ptr<NotMatchExpression> n = stdx::make_unique<NotMatchExpression>();
            Status s2 = n->init(s.getValue().release());
            if (!s2.isOK())
                return s2;
            return {std::move(n)};
        }
        case PathAcceptingKeyword::EQUALITY:
            return _parseComparison(
                name, new EqualityMatchExpression(), e, collator, expCtx, allowedFeatures);

        case PathAcceptingKeyword::IN_EXPR: {
            if (e.type() != Array)
                return {Status(ErrorCodes::BadValue, "$in needs an array")};
            std::unique_ptr<InMatchExpression> temp = stdx::make_unique<InMatchExpression>();
            Status s = temp->init(name);
            if (!s.isOK())
                return s;
            s = _parseInExpression(temp.get(), e.Obj(), collator, expCtx);
            if (!s.isOK())
                return s;
            return {std::move(temp)};
        }

        case PathAcceptingKeyword::NOT_IN: {
            if (e.type() != Array)
                return {Status(ErrorCodes::BadValue, "$nin needs an array")};
            std::unique_ptr<InMatchExpression> temp = stdx::make_unique<InMatchExpression>();
            Status s = temp->init(name);
            if (!s.isOK())
                return s;
            s = _parseInExpression(temp.get(), e.Obj(), collator, expCtx);
            if (!s.isOK())
                return s;

            std::unique_ptr<NotMatchExpression> temp2 = stdx::make_unique<NotMatchExpression>();
            s = temp2->init(temp.release());
            if (!s.isOK())
                return s;

            return {std::move(temp2)};
        }

        case PathAcceptingKeyword::SIZE: {
            int size = 0;
            if (e.type() == NumberInt) {
                size = e.numberInt();
            } else if (e.type() == NumberLong) {
                if (e.numberInt() == e.numberLong()) {
                    size = e.numberInt();
                } else {
                    return {Status(ErrorCodes::BadValue,
                                   "$size must be representable as a 32-bit integer")};
                }
            } else if (e.type() == NumberDouble) {
                if (e.numberInt() == e.numberDouble()) {
                    size = e.numberInt();
                } else {
                    return {Status(ErrorCodes::BadValue, "$size must be a whole number")};
                }
            } else {
                return {Status(ErrorCodes::BadValue, "$size needs a number")};
            }
            if (size < 0) {
                return {Status(ErrorCodes::BadValue, "$size may not be negative")};
            }

            std::unique_ptr<SizeMatchExpression> temp = stdx::make_unique<SizeMatchExpression>();
            Status s = temp->init(name, size);
            if (!s.isOK())
                return s;
            return {std::move(temp)};
        }

        case PathAcceptingKeyword::EXISTS: {
            if (e.eoo())
                return {Status(ErrorCodes::BadValue, "$exists can't be eoo")};
            std::unique_ptr<ExistsMatchExpression> temp =
                stdx::make_unique<ExistsMatchExpression>();
            Status s = temp->init(name);
            if (!s.isOK())
                return s;
            if (e.trueValue())
                return {std::move(temp)};
            std::unique_ptr<NotMatchExpression> temp2 = stdx::make_unique<NotMatchExpression>();
            s = temp2->init(temp.release());
            if (!s.isOK())
                return s;
            return {std::move(temp2)};
        }

        case PathAcceptingKeyword::TYPE:
            return _parseType<TypeMatchExpression>(name, e, expCtx);

        case PathAcceptingKeyword::MOD:
            return _parseMOD(name, e, expCtx);

        case PathAcceptingKeyword::OPTIONS: {
            // TODO: try to optimize this
            // we have to do this since $options can be before or after a $regex
            // but we validate here
            BSONObjIterator i(context);
            while (i.more()) {
                BSONElement temp = i.next();
                if (MatchExpressionParser::parsePathAcceptingKeyword(temp) ==
                    PathAcceptingKeyword::REGEX)
                    return {nullptr};
            }

            return {Status(ErrorCodes::BadValue, "$options needs a $regex")};
        }

        case PathAcceptingKeyword::REGEX: {
            return _parseRegexDocument(name, context, expCtx);
        }

        case PathAcceptingKeyword::ELEM_MATCH:
            return _parseElemMatch(name, e, collator, expCtx, allowedFeatures, topLevel);

        case PathAcceptingKeyword::ALL:
            return _parseAll(name, e, collator, expCtx, allowedFeatures, topLevel);

        case PathAcceptingKeyword::WITHIN:
        case PathAcceptingKeyword::GEO_INTERSECTS:
            return _parseGeo(name, *parseExpMatchType, context, allowedFeatures);

        case PathAcceptingKeyword::GEO_NEAR:
            return {Status(ErrorCodes::BadValue,
                           mongoutils::str::stream() << "near must be first in: " << context)};


        // Handles bitwise query operators.
        case PathAcceptingKeyword::BITS_ALL_SET: {
            return _parseBitTest<BitsAllSetMatchExpression>(name, e, expCtx);
        }

        case PathAcceptingKeyword::BITS_ALL_CLEAR: {
            return _parseBitTest<BitsAllClearMatchExpression>(name, e, expCtx);
        }

        case PathAcceptingKeyword::BITS_ANY_SET: {
            return _parseBitTest<BitsAnySetMatchExpression>(name, e, expCtx);
        }

        case PathAcceptingKeyword::BITS_ANY_CLEAR: {
            return _parseBitTest<BitsAnyClearMatchExpression>(name, e, expCtx);
        }

        case PathAcceptingKeyword::INTERNAL_SCHEMA_FMOD:
            return _parseInternalSchemaFmod(name, e);

        case PathAcceptingKeyword::INTERNAL_SCHEMA_MIN_ITEMS: {
            return _parseInternalSchemaSingleIntegerArgument<InternalSchemaMinItemsMatchExpression>(
                name, e);
        }

        case PathAcceptingKeyword::INTERNAL_SCHEMA_MAX_ITEMS: {
            return _parseInternalSchemaSingleIntegerArgument<InternalSchemaMaxItemsMatchExpression>(
                name, e);
        }

        case PathAcceptingKeyword::INTERNAL_SCHEMA_OBJECT_MATCH: {
            if (e.type() != BSONType::Object) {
                return Status(ErrorCodes::FailedToParse,
                              str::stream() << "$_internalSchemaObjectMatch must be an object");
            }

            auto parsedSubObjExpr = _parse(e.Obj(), collator, expCtx, allowedFeatures, topLevel);
            if (!parsedSubObjExpr.isOK()) {
                return parsedSubObjExpr;
            }

            auto expr = stdx::make_unique<InternalSchemaObjectMatchExpression>();
            auto status = expr->init(std::move(parsedSubObjExpr.getValue()), name);
            if (!status.isOK()) {
                return status;
            }
            return {std::move(expr)};
        }

        case PathAcceptingKeyword::INTERNAL_SCHEMA_UNIQUE_ITEMS: {
            if (!e.isBoolean() || !e.boolean()) {
                return {ErrorCodes::FailedToParse,
                        str::stream() << name << " must be a boolean of value true"};
            }

            auto expr = stdx::make_unique<InternalSchemaUniqueItemsMatchExpression>();
            auto status = expr->init(name);
            if (!status.isOK()) {
                return status;
            }
            return {std::move(expr)};
        }

        case PathAcceptingKeyword::INTERNAL_SCHEMA_MIN_LENGTH: {
            return _parseInternalSchemaSingleIntegerArgument<
                InternalSchemaMinLengthMatchExpression>(name, e);
        }

        case PathAcceptingKeyword::INTERNAL_SCHEMA_MAX_LENGTH: {
            return _parseInternalSchemaSingleIntegerArgument<
                InternalSchemaMaxLengthMatchExpression>(name, e);
        }

        case PathAcceptingKeyword::INTERNAL_SCHEMA_MATCH_ARRAY_INDEX: {
            return _parseInternalSchemaMatchArrayIndex(name, e, collator);
        }

        case PathAcceptingKeyword::INTERNAL_SCHEMA_ALL_ELEM_MATCH_FROM_INDEX: {
            if (e.type() != BSONType::Array) {
                return Status(ErrorCodes::FailedToParse,
                              str::stream()
                                  << InternalSchemaAllElemMatchFromIndexMatchExpression::kName
                                  << " must be an array");
            }
            auto elemMatchObj = e.embeddedObject();
            auto iter = elemMatchObj.begin();
            if (!iter.more()) {
                return Status(ErrorCodes::FailedToParse,
                              str::stream()
                                  << InternalSchemaAllElemMatchFromIndexMatchExpression::kName
                                  << " must be an array of size 2");
            }
            auto first = iter.next();
            auto parsedIndex = parseIntegerElementToNonNegativeLong(first);
            if (!parsedIndex.isOK()) {
                return Status(ErrorCodes::TypeMismatch,
                              str::stream()
                                  << "first element of "
                                  << InternalSchemaAllElemMatchFromIndexMatchExpression::kName
                                  << " must be a non-negative integer");
            }
            if (!iter.more()) {
                return Status(ErrorCodes::FailedToParse,
                              str::stream()
                                  << InternalSchemaAllElemMatchFromIndexMatchExpression::kName
                                  << " must be an array of size 2");
            }
            auto second = iter.next();
            if (iter.more()) {
                return Status(ErrorCodes::FailedToParse,
                              str::stream()
                                  << InternalSchemaAllElemMatchFromIndexMatchExpression::kName
                                  << " has too many elements, must be an array of size 2");
            }
            if (second.type() != BSONType::Object) {
                return Status(ErrorCodes::TypeMismatch,
                              str::stream()
                                  << "second element of "
                                  << InternalSchemaAllElemMatchFromIndexMatchExpression::kName
                                  << "must be an object");
            }
            StatusWithMatchExpression query =
                _parse(second.embeddedObject(), collator, expCtx, allowedFeatures, topLevel);
            if (!query.isOK()) {
                return query.getStatus();
            }
            auto expr = stdx::make_unique<InternalSchemaAllElemMatchFromIndexMatchExpression>();
            auto status = expr->init(name, parsedIndex.getValue(), std::move(query.getValue()));
            if (!status.isOK()) {
                return status;
            }
            return {std::move(expr)};
        }

        case PathAcceptingKeyword::INTERNAL_SCHEMA_TYPE: {
            return _parseType<InternalSchemaTypeExpression>(name, e, expCtx);
        }
    }

    return {Status(ErrorCodes::BadValue,
                   mongoutils::str::stream() << "not handled: " << e.fieldName())};
}

StatusWithMatchExpression MatchExpressionParser::_parse(
    const BSONObj& obj,
    const CollatorInterface* collator,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    AllowedFeatureSet allowedFeatures,
    bool topLevel) {
    std::unique_ptr<AndMatchExpression> root = stdx::make_unique<AndMatchExpression>();

    const bool childIsTopLevel = false;
    BSONObjIterator i(obj);
    while (i.more()) {
        BSONElement e = i.next();
        if (e.fieldName()[0] == '$') {
            const char* rest = e.fieldName() + 1;

            if (mongoutils::str::equals("or", rest)) {
                if (e.type() != Array)
                    return {Status(ErrorCodes::BadValue, "$or must be an array")};
                std::unique_ptr<OrMatchExpression> temp = stdx::make_unique<OrMatchExpression>();
                Status s = _parseTreeList(
                    e.Obj(), temp.get(), collator, expCtx, allowedFeatures, childIsTopLevel);
                if (!s.isOK())
                    return s;
                root->add(temp.release());
            } else if (mongoutils::str::equals("and", rest)) {
                if (e.type() != Array)
                    return {Status(ErrorCodes::BadValue, "$and must be an array")};
                std::unique_ptr<AndMatchExpression> temp = stdx::make_unique<AndMatchExpression>();
                Status s = _parseTreeList(
                    e.Obj(), temp.get(), collator, expCtx, allowedFeatures, childIsTopLevel);
                if (!s.isOK())
                    return s;
                root->add(temp.release());
            } else if (mongoutils::str::equals("nor", rest)) {
                if (e.type() != Array)
                    return {Status(ErrorCodes::BadValue, "$nor must be an array")};
                std::unique_ptr<NorMatchExpression> temp = stdx::make_unique<NorMatchExpression>();
                Status s = _parseTreeList(
                    e.Obj(), temp.get(), collator, expCtx, allowedFeatures, childIsTopLevel);
                if (!s.isOK())
                    return s;
                root->add(temp.release());
            } else if (mongoutils::str::equals("atomic", rest) ||
                       mongoutils::str::equals("isolated", rest)) {
                if (!topLevel)
                    return {Status(ErrorCodes::BadValue,
                                   "$atomic/$isolated has to be at the top level")};
                // Don't do anything with the expression; CanonicalQuery::init() will look through
                // the BSONObj again for a $atomic/$isolated.
            } else if (mongoutils::str::equals("where", rest)) {
                if ((allowedFeatures & AllowedFeatures::kJavascript) == 0u) {
                    return {Status(ErrorCodes::BadValue, "$where is not allowed in this context")};
                }

                StatusWithMatchExpression s = _extensionsCallback->parseWhere(e);
                if (!s.isOK())
                    return s;
                root->add(s.getValue().release());
            } else if (mongoutils::str::equals("text", rest)) {
                if ((allowedFeatures & AllowedFeatures::kText) == 0u) {
                    return {Status(ErrorCodes::BadValue, "$text is not allowed in this context")};
                }

                StatusWithMatchExpression s = _extensionsCallback->parseText(e);
                if (!s.isOK()) {
                    return s;
                }
                root->add(s.getValue().release());
            } else if (mongoutils::str::equals("comment", rest)) {
            } else if (mongoutils::str::equals("ref", rest) ||
                       mongoutils::str::equals("id", rest) || mongoutils::str::equals("db", rest)) {
                // DBRef fields.
                std::unique_ptr<ComparisonMatchExpression> eq =
                    stdx::make_unique<EqualityMatchExpression>();
                Status s = eq->init(e.fieldName(), e);
                if (!s.isOK())
                    return s;
                // 'id' is collation-aware. 'ref' and 'db' are compared using binary comparison.
                eq->setCollator(str::equals("id", rest) ? collator : nullptr);

                root->add(eq.release());
            } else if (mongoutils::str::equals("_internalSchemaAllowedProperties", rest)) {
                auto allowedProperties = _parseInternalSchemaAllowedProperties(e, collator);
                if (!allowedProperties.isOK()) {
                    return allowedProperties.getStatus();
                }
                root->add(allowedProperties.getValue().release());
            } else if (mongoutils::str::equals("_internalSchemaCond", rest)) {
                auto condExpr =
                    _parseInternalSchemaFixedArityArgument<InternalSchemaCondMatchExpression>(
                        InternalSchemaCondMatchExpression::kName,
                        e,
                        collator,
                        expCtx,
                        allowedFeatures);
                if (!condExpr.isOK()) {
                    return condExpr.getStatus();
                }
                root->add(condExpr.getValue().release());

            } else if (mongoutils::str::equals("_internalSchemaXor", rest)) {
                if (e.type() != BSONType::Array)
                    return {
                        Status(ErrorCodes::TypeMismatch, "$_internalSchemaXor must be an array")};
                auto xorExpr = stdx::make_unique<InternalSchemaXorMatchExpression>();
                Status s = _parseTreeList(
                    e.Obj(), xorExpr.get(), collator, expCtx, allowedFeatures, childIsTopLevel);
                if (!s.isOK())
                    return s;
                root->add(xorExpr.release());
            } else if (mongoutils::str::equals("_internalSchemaMinProperties", rest)) {
                return _parseTopLevelInternalSchemaSingleIntegerArgument<
                    InternalSchemaMinPropertiesMatchExpression>(e);
            } else if (mongoutils::str::equals("_internalSchemaMaxProperties", rest)) {
                return _parseTopLevelInternalSchemaSingleIntegerArgument<
                    InternalSchemaMaxPropertiesMatchExpression>(e);
            } else if (mongoutils::str::equals("jsonSchema", rest)) {
                if (e.type() != BSONType::Object) {
                    return {Status(ErrorCodes::TypeMismatch, "$jsonSchema must be an object")};
                }
                return JSONSchemaParser::parse(e.Obj());
            } else if (mongoutils::str::equals("alwaysFalse", rest)) {
                auto statusWithLong = MatchExpressionParser::parseIntegerElementToLong(e);
                if (!statusWithLong.isOK()) {
                    return statusWithLong.getStatus();
                }

                if (statusWithLong.getValue() != 1) {
                    return {Status(ErrorCodes::FailedToParse,
                                   "$alwaysFalse must be an integer value of 1")};
                }

                return {stdx::make_unique<AlwaysFalseMatchExpression>()};
            } else if (mongoutils::str::equals("alwaysTrue", rest)) {
                auto statusWithLong = MatchExpressionParser::parseIntegerElementToLong(e);
                if (!statusWithLong.isOK()) {
                    return statusWithLong.getStatus();
                }

                if (statusWithLong.getValue() != 1) {
                    return {Status(ErrorCodes::FailedToParse,
                                   "$alwaysTrue must be an integer value of 1")};
                }

                return {stdx::make_unique<AlwaysTrueMatchExpression>()};
            } else {
                return {Status(ErrorCodes::BadValue,
                               mongoutils::str::stream() << "unknown top level operator: "
                                                         << e.fieldName())};
            }

            continue;
        }

        if (_isExpressionDocument(e, false, expCtx)) {
            Status s = _parseSub(e.fieldName(),
                                 e.Obj(),
                                 root.get(),
                                 collator,
                                 expCtx,
                                 allowedFeatures,
                                 childIsTopLevel);
            if (!s.isOK())
                return s;
            continue;
        }

        if (e.type() == RegEx) {
            StatusWithMatchExpression result = _parseRegexElement(e.fieldName(), e, expCtx);
            if (!result.isOK())
                return result;
            root->add(result.getValue().release());
            continue;
        }

        auto eq = _parseComparison(
            e.fieldName(), new EqualityMatchExpression(), e, collator, expCtx, allowedFeatures);
        if (!eq.isOK())
            return eq;

        root->add(eq.getValue().release());
    }

    if (root->numChildren() == 1) {
        std::unique_ptr<MatchExpression> real(root->getChild(0));
        root->clearAndRelease();
        return {std::move(real)};
    }

    return {std::move(root)};
}

Status MatchExpressionParser::_parseSub(const char* name,
                                        const BSONObj& sub,
                                        AndMatchExpression* root,
                                        const CollatorInterface* collator,
                                        const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                        AllowedFeatureSet allowedFeatures,
                                        bool topLevel) {
    // The one exception to {field : {fully contained argument} } is, of course, geo.  Example:
    // sub == { field : {$near[Sphere]: [0,0], $maxDistance: 1000, $minDistance: 10 } }
    // We peek inside of 'sub' to see if it's possibly a $near.  If so, we can't iterate over
    // its subfields and parse them one at a time (there is no $maxDistance without $near), so
    // we hand the entire object over to the geo parsing routines.

    // Special case parsing for geoNear. This is necessary in order to support query formats like
    // {$near: <coords>, $maxDistance: <distance>}. No other query operators allow $-prefixed
    // modifiers as sibling BSON elements.
    BSONObjIterator geoIt(sub);
    if (geoIt.more()) {
        BSONElement firstElt = geoIt.next();
        if (firstElt.isABSONObj()) {
            if (MatchExpressionParser::parsePathAcceptingKeyword(firstElt) ==
                PathAcceptingKeyword::GEO_NEAR) {
                StatusWithMatchExpression s =
                    _parseGeo(name, PathAcceptingKeyword::GEO_NEAR, sub, allowedFeatures);
                if (s.isOK()) {
                    root->add(s.getValue().release());
                }

                // Propagate geo parsing result to caller.
                return s.getStatus();
            }
        }
    }

    BSONObjIterator j(sub);
    while (j.more()) {
        BSONElement deep = j.next();

        const bool childIsTopLevel = false;
        StatusWithMatchExpression s = _parseSubField(
            sub, root, name, deep, collator, expCtx, allowedFeatures, childIsTopLevel);
        if (!s.isOK())
            return s.getStatus();

        if (s.getValue())
            root->add(s.getValue().release());
    }

    return Status::OK();
}

bool MatchExpressionParser::_isExpressionDocument(
    const BSONElement& e,
    bool allowIncompleteDBRef,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    if (e.type() != Object)
        return false;

    BSONObj o = e.Obj();
    if (o.isEmpty())
        return false;

    const char* name = o.firstElement().fieldName();
    if (name[0] != '$')
        return false;

    if (_isDBRefDocument(o, allowIncompleteDBRef)) {
        return false;
    }

    if (_isAggExpression(e, expCtx)) {
        return false;
    }

    return true;
}

/**
 * DBRef fields are ordered in the collection.
 * In the query, we consider an embedded object a query on
 * a DBRef as long as it contains $ref and $id.
 * Required fields: $ref and $id (if incomplete DBRefs are not allowed)
 *
 * If incomplete DBRefs are allowed, we accept the BSON object as long as it
 * contains $ref, $id or $db.
 *
 * Field names are checked but not field types.
 */
bool MatchExpressionParser::_isDBRefDocument(const BSONObj& obj, bool allowIncompleteDBRef) {
    bool hasRef = false;
    bool hasID = false;
    bool hasDB = false;

    BSONObjIterator i(obj);
    while (i.more() && !(hasRef && hasID)) {
        BSONElement element = i.next();
        const char* fieldName = element.fieldName();
        // $ref
        if (!hasRef && mongoutils::str::equals("$ref", fieldName)) {
            hasRef = true;
        }
        // $id
        else if (!hasID && mongoutils::str::equals("$id", fieldName)) {
            hasID = true;
        }
        // $db
        else if (!hasDB && mongoutils::str::equals("$db", fieldName)) {
            hasDB = true;
        }
    }

    if (allowIncompleteDBRef) {
        return hasRef || hasID || hasDB;
    }

    return hasRef && hasID;
}

StatusWithMatchExpression MatchExpressionParser::_parseMOD(
    const char* name, const BSONElement& e, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    if (e.type() != Array)
        return {Status(ErrorCodes::BadValue, "malformed mod, needs to be an array")};

    BSONObjIterator i(e.Obj());

    if (!i.more())
        return {Status(ErrorCodes::BadValue, "malformed mod, not enough elements")};
    BSONElement d = i.next();
    if (!d.isNumber())
        return {Status(ErrorCodes::BadValue, "malformed mod, divisor not a number")};

    if (!i.more())
        return {Status(ErrorCodes::BadValue, "malformed mod, not enough elements")};
    BSONElement r = i.next();
    if (!d.isNumber())
        return {Status(ErrorCodes::BadValue, "malformed mod, remainder not a number")};

    if (i.more())
        return {Status(ErrorCodes::BadValue, "malformed mod, too many elements")};

    std::unique_ptr<ModMatchExpression> temp = stdx::make_unique<ModMatchExpression>();
    Status s = temp->init(name, d.numberInt(), r.numberInt());
    if (!s.isOK())
        return s;
    return {std::move(temp)};
}

StatusWithMatchExpression MatchExpressionParser::_parseRegexElement(
    const char* name, const BSONElement& e, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    if (e.type() != RegEx)
        return {Status(ErrorCodes::BadValue, "not a regex")};

    std::unique_ptr<RegexMatchExpression> temp = stdx::make_unique<RegexMatchExpression>();
    Status s = temp->init(name, e.regex(), e.regexFlags());
    if (!s.isOK())
        return s;
    return {std::move(temp)};
}

StatusWithMatchExpression MatchExpressionParser::_parseRegexDocument(
    const char* name, const BSONObj& doc, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    string regex;
    string regexOptions;

    BSONObjIterator i(doc);
    while (i.more()) {
        BSONElement e = i.next();
        auto matchType = MatchExpressionParser::parsePathAcceptingKeyword(e);
        if (!matchType) {
            continue;
        }

        switch (*matchType) {
            case PathAcceptingKeyword::REGEX:
                if (e.type() == String) {
                    regex = e.String();
                } else if (e.type() == RegEx) {
                    regex = e.regex();
                    regexOptions = e.regexFlags();
                } else {
                    return {Status(ErrorCodes::BadValue, "$regex has to be a string")};
                }

                break;
            case PathAcceptingKeyword::OPTIONS:
                if (e.type() != String)
                    return {Status(ErrorCodes::BadValue, "$options has to be a string")};
                regexOptions = e.String();
                break;
            default:
                break;
        }
    }

    std::unique_ptr<RegexMatchExpression> temp = stdx::make_unique<RegexMatchExpression>();
    Status s = temp->init(name, regex, regexOptions);
    if (!s.isOK())
        return s;
    return {std::move(temp)};
}

Status MatchExpressionParser::_parseInExpression(
    InMatchExpression* inExpression,
    const BSONObj& theArray,
    const CollatorInterface* collator,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    inExpression->setCollator(collator);
    std::vector<BSONElement> equalities;
    BSONObjIterator i(theArray);
    while (i.more()) {
        BSONElement e = i.next();

        // Allow DBRefs, but reject all fields with names starting with $.
        if (_isExpressionDocument(e, false, expCtx)) {
            return Status(ErrorCodes::BadValue, "cannot nest $ under $in");
        }

        if (e.type() == RegEx) {
            std::unique_ptr<RegexMatchExpression> r = stdx::make_unique<RegexMatchExpression>();
            Status s = r->init("", e);
            if (!s.isOK())
                return s;
            s = inExpression->addRegex(std::move(r));
            if (!s.isOK())
                return s;
        } else {
            if (_isAggExpression(e, expCtx)) {
                return Status(ErrorCodes::BadValue, "$expr not supported for $in");
            }

            equalities.push_back(e);
        }
    }
    return inExpression->setEqualities(std::move(equalities));
}

template <class T>
StatusWithMatchExpression MatchExpressionParser::_parseType(
    const char* name,
    const BSONElement& elt,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto typeSet = MatcherTypeSet::parse(elt, MatcherTypeSet::kTypeAliasMap);
    if (!typeSet.isOK()) {
        return typeSet.getStatus();
    }

    auto typeExpr = stdx::make_unique<T>();

    if (typeSet.getValue().isEmpty()) {
        return {Status(ErrorCodes::FailedToParse,
                       str::stream() << typeExpr->name() << " must match at least one type")};
    }

    auto status = typeExpr->init(name, std::move(typeSet.getValue()));
    if (!status.isOK()) {
        return status;
    }

    return {std::move(typeExpr)};
}

StatusWithMatchExpression MatchExpressionParser::_parseElemMatch(
    const char* name,
    const BSONElement& e,
    const CollatorInterface* collator,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    AllowedFeatureSet allowedFeatures,
    bool topLevel) {
    if (e.type() != Object)
        return {Status(ErrorCodes::BadValue, "$elemMatch needs an Object")};

    BSONObj obj = e.Obj();

    // $elemMatch value case applies when the children all
    // work on the field 'name'.
    // This is the case when:
    //     1) the argument is an expression document; and
    //     2) expression is not a AND/NOR/OR logical operator. Children of
    //        these logical operators are initialized with field names.
    //     3) expression is not a WHERE operator. WHERE works on objects instead
    //        of specific field.
    bool isElemMatchValue = false;
    if (_isExpressionDocument(e, true, expCtx)) {
        BSONObj o = e.Obj();
        BSONElement elt = o.firstElement();
        invariant(!elt.eoo());

        isElemMatchValue = !mongoutils::str::equals("$and", elt.fieldName()) &&
            !mongoutils::str::equals("$nor", elt.fieldName()) &&
            !mongoutils::str::equals("$_internalSchemaXor", elt.fieldName()) &&
            !mongoutils::str::equals("$or", elt.fieldName()) &&
            !mongoutils::str::equals("$where", elt.fieldName()) &&
            !mongoutils::str::equals("$_internalSchemaMinProperties", elt.fieldName()) &&
            !mongoutils::str::equals("$_internalSchemaMaxProperties", elt.fieldName()) &&
            !mongoutils::str::equals("$_internalSchemaAllowedProperties", elt.fieldName());
    }

    if (isElemMatchValue) {
        // value case

        AndMatchExpression theAnd;
        Status s = _parseSub("", obj, &theAnd, collator, expCtx, allowedFeatures, topLevel);
        if (!s.isOK())
            return s;

        std::unique_ptr<ElemMatchValueMatchExpression> temp =
            stdx::make_unique<ElemMatchValueMatchExpression>();
        s = temp->init(name);
        if (!s.isOK())
            return s;

        for (size_t i = 0; i < theAnd.numChildren(); i++) {
            temp->add(theAnd.getChild(i));
        }
        theAnd.clearAndRelease();

        return {std::move(temp)};
    }

    // DBRef value case
    // A DBRef document under a $elemMatch should be treated as an object case
    // because it may contain non-DBRef fields in addition to $ref, $id and $db.

    // object case

    StatusWithMatchExpression subRaw = _parse(obj, collator, expCtx, allowedFeatures, topLevel);
    if (!subRaw.isOK())
        return subRaw;
    std::unique_ptr<MatchExpression> sub = std::move(subRaw.getValue());

    // $where is not supported under $elemMatch because $where
    // applies to top-level document, not array elements in a field.
    if (hasNode(sub.get(), MatchExpression::WHERE)) {
        return {Status(ErrorCodes::BadValue, "$elemMatch cannot contain $where expression")};
    }

    std::unique_ptr<ElemMatchObjectMatchExpression> temp =
        stdx::make_unique<ElemMatchObjectMatchExpression>();
    Status status = temp->init(name, sub.release());
    if (!status.isOK())
        return status;

    return {std::move(temp)};
}

StatusWithMatchExpression MatchExpressionParser::_parseAll(
    const char* name,
    const BSONElement& e,
    const CollatorInterface* collator,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    AllowedFeatureSet allowedFeatures,
    bool topLevel) {
    if (e.type() != Array)
        return {Status(ErrorCodes::BadValue, "$all needs an array")};

    BSONObj arr = e.Obj();
    std::unique_ptr<AndMatchExpression> myAnd = stdx::make_unique<AndMatchExpression>();
    BSONObjIterator i(arr);

    if (arr.firstElement().type() == Object &&
        mongoutils::str::equals("$elemMatch",
                                arr.firstElement().Obj().firstElement().fieldName())) {
        // $all : [ { $elemMatch : {} } ... ]

        while (i.more()) {
            BSONElement hopefullyElemMatchElement = i.next();

            if (hopefullyElemMatchElement.type() != Object) {
                // $all : [ { $elemMatch : ... }, 5 ]
                return {Status(ErrorCodes::BadValue, "$all/$elemMatch has to be consistent")};
            }

            BSONObj hopefullyElemMatchObj = hopefullyElemMatchElement.Obj();
            if (!mongoutils::str::equals("$elemMatch",
                                         hopefullyElemMatchObj.firstElement().fieldName())) {
                // $all : [ { $elemMatch : ... }, { x : 5 } ]
                return {Status(ErrorCodes::BadValue, "$all/$elemMatch has to be consistent")};
            }

            const bool childIsTopLevel = false;
            StatusWithMatchExpression inner = _parseElemMatch(name,
                                                              hopefullyElemMatchObj.firstElement(),
                                                              collator,
                                                              expCtx,
                                                              allowedFeatures,
                                                              childIsTopLevel);
            if (!inner.isOK())
                return inner;
            myAnd->add(inner.getValue().release());
        }

        return {std::move(myAnd)};
    }

    while (i.more()) {
        BSONElement e = i.next();

        if (e.type() == RegEx) {
            std::unique_ptr<RegexMatchExpression> r = stdx::make_unique<RegexMatchExpression>();
            Status s = r->init(name, e);
            if (!s.isOK())
                return s;
            myAnd->add(r.release());
        } else if (e.type() == Object &&
                   MatchExpressionParser::parsePathAcceptingKeyword(e.Obj().firstElement())) {
            return {Status(ErrorCodes::BadValue, "no $ expressions in $all")};
        } else {
            std::unique_ptr<EqualityMatchExpression> x =
                stdx::make_unique<EqualityMatchExpression>();
            Status s = x->init(name, e);
            if (!s.isOK())
                return s;
            x->setCollator(collator);
            myAnd->add(x.release());
        }
    }

    if (myAnd->numChildren() == 0) {
        return {stdx::make_unique<AlwaysFalseMatchExpression>()};
    }

    return {std::move(myAnd)};
}

template <class T>
StatusWithMatchExpression MatchExpressionParser::_parseBitTest(
    const char* name, const BSONElement& e, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    std::unique_ptr<BitTestMatchExpression> bitTestMatchExpression = stdx::make_unique<T>();

    if (e.type() == BSONType::Array) {
        // Array of bit positions provided as value.
        auto statusWithBitPositions = _parseBitPositionsArray(e.Obj());
        if (!statusWithBitPositions.isOK()) {
            return statusWithBitPositions.getStatus();
        }

        std::vector<uint32_t> bitPositions = statusWithBitPositions.getValue();
        Status s = bitTestMatchExpression->init(name, bitPositions);
        if (!s.isOK()) {
            return s;
        }
    } else if (e.isNumber()) {
        // Integer bitmask provided as value.
        auto bitMask = parseIntegerElementToNonNegativeLong(e);
        if (!bitMask.isOK()) {
            return bitMask.getStatus();
        }

        Status s = bitTestMatchExpression->init(name, bitMask.getValue());
        if (!s.isOK()) {
            return s;
        }
    } else if (e.type() == BSONType::BinData) {
        // Binary bitmask provided as value.

        int eBinaryLen;
        const char* eBinary = e.binData(eBinaryLen);

        Status s = bitTestMatchExpression->init(name, eBinary, eBinaryLen);
        if (!s.isOK()) {
            return s;
        }
    } else {
        mongoutils::str::stream ss;
        ss << name << " takes an Array, a number, or a BinData but received: " << e;
        return Status(ErrorCodes::BadValue, ss);
    }

    return {std::move(bitTestMatchExpression)};
}

StatusWith<std::vector<uint32_t>> MatchExpressionParser::_parseBitPositionsArray(
    const BSONObj& theArray) {
    std::vector<uint32_t> bitPositions;

    // Fill temporary bit position array with integers read from the BSON array.
    for (const BSONElement& e : theArray) {
        if (!e.isNumber()) {
            mongoutils::str::stream ss;
            ss << "bit positions must be an integer but got: " << e;
            return Status(ErrorCodes::BadValue, ss);
        }

        if (e.type() == BSONType::NumberDouble) {
            double eDouble = e.numberDouble();

            // NaN doubles are rejected.
            if (std::isnan(eDouble)) {
                mongoutils::str::stream ss;
                ss << "bit positions cannot take a NaN: " << e;
                return Status(ErrorCodes::BadValue, ss);
            }

            // This makes sure e does not overflow a 32-bit integer container.
            if (eDouble > std::numeric_limits<int>::max() ||
                eDouble < std::numeric_limits<int>::min()) {
                mongoutils::str::stream ss;
                ss << "bit positions cannot be represented as a 32-bit signed integer: " << e;
                return Status(ErrorCodes::BadValue, ss);
            }

            // This checks if e is integral.
            if (eDouble != static_cast<double>(static_cast<long long>(eDouble))) {
                mongoutils::str::stream ss;
                ss << "bit positions must be an integer but got: " << e;
                return Status(ErrorCodes::BadValue, ss);
            }
        }

        if (e.type() == BSONType::NumberLong) {
            long long eLong = e.numberLong();

            // This makes sure e does not overflow a 32-bit integer container.
            if (eLong > std::numeric_limits<int>::max() ||
                eLong < std::numeric_limits<int>::min()) {
                mongoutils::str::stream ss;
                ss << "bit positions cannot be represented as a 32-bit signed integer: " << e;
                return Status(ErrorCodes::BadValue, ss);
            }
        }

        int eValue = e.numberInt();

        // No negatives.
        if (eValue < 0) {
            mongoutils::str::stream ss;
            ss << "bit positions must be >= 0 but got: " << e;
            return Status(ErrorCodes::BadValue, ss);
        }

        bitPositions.push_back(eValue);
    }

    return bitPositions;
}

StatusWith<long long> MatchExpressionParser::parseIntegerElementToNonNegativeLong(
    BSONElement elem) {
    auto number = parseIntegerElementToLong(elem);
    if (!number.isOK()) {
        return number;
    }

    if (number.getValue() < 0) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "Expected a positive number in: " << elem);
    }

    return number;
}

StatusWith<long long> MatchExpressionParser::parseIntegerElementToLong(BSONElement elem) {
    if (!elem.isNumber()) {
        return Status(ErrorCodes::FailedToParse, str::stream() << "Expected a number in: " << elem);
    }

    long long number = 0;
    if (elem.type() == BSONType::NumberDouble) {
        double eDouble = elem.numberDouble();

        // NaN doubles are rejected.
        if (std::isnan(eDouble)) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream() << "Expected an integer, but found NaN in: " << elem);
        }

        // No integral doubles that are too large to be represented as a 64 bit signed integer.
        // We use 'kLongLongMaxAsDouble' because if we just did eDouble > 2^63-1, it would be
        // compared against 2^63. eDouble=2^63 would not get caught that way.
        if (eDouble >= MatchExpressionParser::kLongLongMaxPlusOneAsDouble ||
            eDouble < std::numeric_limits<long long>::min()) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream() << "Cannot represent as a 64-bit integer: " << elem);
        }

        // This checks if elem is an integral double.
        if (eDouble != static_cast<double>(static_cast<long long>(eDouble))) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream() << "Expected an integer: " << elem);
        }

        number = elem.numberLong();
    } else if (elem.type() == BSONType::NumberDecimal) {
        uint32_t signalingFlags = Decimal128::kNoFlag;
        number = elem.numberDecimal().toLongExact(&signalingFlags);
        if (signalingFlags != Decimal128::kNoFlag) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream() << "Cannot represent as a 64-bit integer: " << elem);
        }
    } else {
        number = elem.numberLong();
    }

    return number;
}

StatusWithMatchExpression MatchExpressionParser::_parseInternalSchemaFmod(const char* name,
                                                                          const BSONElement& elem) {
    StringData path(name);
    if (elem.type() != Array)
        return {ErrorCodes::BadValue,
                str::stream() << path << " must be an array, but got type " << elem.type()};

    BSONObjIterator i(elem.embeddedObject());

    if (!i.more())
        return {ErrorCodes::BadValue, str::stream() << path << " does not have enough elements"};
    BSONElement d = i.next();
    if (!d.isNumber())
        return {ErrorCodes::TypeMismatch,
                str::stream() << path << " does not have a numeric divisor"};

    if (!i.more())
        return {ErrorCodes::BadValue, str::stream() << path << " does not have enough elements"};
    BSONElement r = i.next();
    if (!d.isNumber())
        return {ErrorCodes::TypeMismatch,
                str::stream() << path << " does not have a numeric remainder"};

    if (i.more())
        return {ErrorCodes::BadValue, str::stream() << path << " has too many elements"};

    std::unique_ptr<InternalSchemaFmodMatchExpression> result =
        stdx::make_unique<InternalSchemaFmodMatchExpression>();
    Status s = result->init(name, d.numberDecimal(), r.numberDecimal());
    if (!s.isOK())
        return s;
    return {std::move(result)};
}


template <class T>
StatusWithMatchExpression MatchExpressionParser::_parseInternalSchemaFixedArityArgument(
    StringData name,
    const BSONElement& input,
    const CollatorInterface* collator,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    AllowedFeatureSet allowedFeatures) {
    constexpr auto arity = T::arity();
    if (input.type() != BSONType::Array) {
        return {ErrorCodes::FailedToParse,
                str::stream() << name << " must be an array of " << arity << " MatchExpressions"};
    }

    auto inputObj = input.embeddedObject();
    if (static_cast<size_t>(inputObj.nFields()) != arity) {
        return {ErrorCodes::FailedToParse,
                str::stream() << name << " requires exactly " << arity
                              << " MatchExpressions, but got "
                              << inputObj.nFields()};
    }

    // Fill out 'expressions' with all of the parsed subexpressions contained in the array, tracking
    // our location in the array with 'position'.
    std::array<std::unique_ptr<MatchExpression>, arity> expressions;
    auto position = expressions.begin();

    for (const auto& elem : inputObj) {
        if (elem.type() != BSONType::Object) {
            return {ErrorCodes::FailedToParse,
                    str::stream() << name
                                  << " must be an array of objects, but found an element of type "
                                  << elem.type()};
        }

        const bool isTopLevel = false;
        auto subexpr = _parse(elem.embeddedObject(), collator, expCtx, allowedFeatures, isTopLevel);
        if (!subexpr.isOK()) {
            return subexpr.getStatus();
        }
        *position = std::move(subexpr.getValue());
        ++position;
    }

    auto parsedExpression = stdx::make_unique<T>();
    parsedExpression->init(std::move(expressions));
    return {std::move(parsedExpression)};
}

template <class T>
StatusWithMatchExpression MatchExpressionParser::_parseInternalSchemaSingleIntegerArgument(
    const char* name, const BSONElement& elem) const {
    auto parsedInt = parseIntegerElementToNonNegativeLong(elem);
    if (!parsedInt.isOK()) {
        return parsedInt.getStatus();
    }

    auto matchExpression = stdx::make_unique<T>();
    auto status = matchExpression->init(name, parsedInt.getValue());
    if (!status.isOK()) {
        return status;
    }

    return {std::move(matchExpression)};
}

template <class T>
StatusWithMatchExpression MatchExpressionParser::_parseTopLevelInternalSchemaSingleIntegerArgument(
    const BSONElement& elem) const {
    auto parsedInt = parseIntegerElementToNonNegativeLong(elem);
    if (!parsedInt.isOK()) {
        return parsedInt.getStatus();
    }
    auto matchExpression = stdx::make_unique<T>();
    auto status = matchExpression->init(parsedInt.getValue());
    if (!status.isOK()) {
        return status;
    }
    return {std::move(matchExpression)};
}

namespace {
/**
 * Looks at the field named 'namePlaceholderFieldName' within 'containingObject' and parses a name
 * placeholder from that element. 'expressionName' is the name of the expression that requires the
 * name placeholder and is used to generate helpful error messages.
 */
StatusWith<StringData> parseNamePlaceholder(const BSONObj& containingObject,
                                            StringData namePlaceholderFieldName,
                                            StringData expressionName) {
    auto namePlaceholderElem = containingObject[namePlaceholderFieldName];
    if (!namePlaceholderElem) {
        return {ErrorCodes::FailedToParse,
                str::stream() << expressionName << " requires a '" << namePlaceholderFieldName
                              << "'"};
    } else if (namePlaceholderElem.type() != BSONType::String) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << expressionName << " requires '" << namePlaceholderFieldName
                              << "' to be a string, not "
                              << namePlaceholderElem.type()};
    }
    return {namePlaceholderElem.valueStringData()};
}

/**
 * Looks at the field named 'exprWithPlaceholderFieldName' within 'containingObject' and parses an
 * ExpressionWithPlaceholder from that element. Fails if an error occurs during parsing, or if the
 * ExpressionWithPlaceholder has a different name placeholder than 'expectedPlaceholder'.
 * 'expressionName' is the name of the expression that requires the ExpressionWithPlaceholder and is
 * used to generate helpful error messages.
 */
StatusWith<std::unique_ptr<ExpressionWithPlaceholder>> parseExprWithPlaceholder(
    const BSONObj& containingObject,
    StringData exprWithPlaceholderFieldName,
    StringData expressionName,
    StringData expectedPlaceholder,
    const CollatorInterface* collator) {
    auto exprWithPlaceholderElem = containingObject[exprWithPlaceholderFieldName];
    if (!exprWithPlaceholderElem) {
        return {ErrorCodes::FailedToParse,
                str::stream() << expressionName << " requires '" << exprWithPlaceholderFieldName
                              << "'"};
    } else if (exprWithPlaceholderElem.type() != BSONType::Object) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << expressionName << " found '" << exprWithPlaceholderFieldName
                              << "', which is an incompatible type: "
                              << exprWithPlaceholderElem.type()};
    }

    auto result =
        ExpressionWithPlaceholder::parse(exprWithPlaceholderElem.embeddedObject(), collator);
    if (!result.isOK()) {
        return result.getStatus();
    }

    auto placeholder = result.getValue()->getPlaceholder();
    if (placeholder && (*placeholder != expectedPlaceholder)) {
        return {ErrorCodes::FailedToParse,
                str::stream() << expressionName << " expected a name placeholder of "
                              << expectedPlaceholder
                              << ", but '"
                              << exprWithPlaceholderElem.fieldName()
                              << "' has a mismatching placeholder '"
                              << *placeholder
                              << "'"};
    }
    return result;
}

StatusWith<std::vector<InternalSchemaAllowedPropertiesMatchExpression::PatternSchema>>
parsePatternProperties(BSONElement patternPropertiesElem,
                       StringData expectedPlaceholder,
                       const CollatorInterface* collator) {
    if (!patternPropertiesElem) {
        return {ErrorCodes::FailedToParse,
                str::stream() << InternalSchemaAllowedPropertiesMatchExpression::kName
                              << " requires 'patternProperties'"};
    } else if (patternPropertiesElem.type() != BSONType::Array) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << InternalSchemaAllowedPropertiesMatchExpression::kName
                              << " requires 'patternProperties' to be an array, not "
                              << patternPropertiesElem.type()};
    }

    std::vector<InternalSchemaAllowedPropertiesMatchExpression::PatternSchema> patternProperties;
    for (auto&& constraintElem : patternPropertiesElem.embeddedObject()) {
        if (constraintElem.type() != BSONType::Object) {
            return {ErrorCodes::TypeMismatch,
                    str::stream() << InternalSchemaAllowedPropertiesMatchExpression::kName
                                  << " requires 'patternProperties' to be an array of objects"};
        }

        auto constraint = constraintElem.embeddedObject();
        if (constraint.nFields() != 2) {
            return {ErrorCodes::FailedToParse,
                    str::stream() << InternalSchemaAllowedPropertiesMatchExpression::kName
                                  << " requires 'patternProperties' to be an array of objects "
                                     "containing exactly two fields, 'regex' and 'expression'"};
        }

        auto expressionWithPlaceholder =
            parseExprWithPlaceholder(constraint,
                                     "expression"_sd,
                                     InternalSchemaAllowedPropertiesMatchExpression::kName,
                                     expectedPlaceholder,
                                     collator);
        if (!expressionWithPlaceholder.isOK()) {
            return expressionWithPlaceholder.getStatus();
        }

        auto regexElem = constraint["regex"];
        if (!regexElem) {
            return {
                ErrorCodes::FailedToParse,
                str::stream() << InternalSchemaAllowedPropertiesMatchExpression::kName
                              << " requires each object in 'patternProperties' to have a 'regex'"};
        }
        if (regexElem.type() != BSONType::RegEx) {
            return {ErrorCodes::TypeMismatch,
                    str::stream() << InternalSchemaAllowedPropertiesMatchExpression::kName
                                  << " requires 'patternProperties' to be an array of objects, "
                                     "where 'regex' is a regular expression"};
        } else if (*regexElem.regexFlags() != '\0') {
            return {
                ErrorCodes::BadValue,
                str::stream()
                    << InternalSchemaAllowedPropertiesMatchExpression::kName
                    << " does not accept regex flags for pattern schemas in 'patternProperties'"};
        }

        patternProperties.emplace_back(
            InternalSchemaAllowedPropertiesMatchExpression::Pattern(regexElem.regex()),
            std::move(expressionWithPlaceholder.getValue()));
    }

    return std::move(patternProperties);
}

StatusWith<boost::container::flat_set<StringData>> parseProperties(BSONElement propertiesElem) {
    if (!propertiesElem) {
        return {ErrorCodes::FailedToParse,
                str::stream() << InternalSchemaAllowedPropertiesMatchExpression::kName
                              << " requires 'properties' to be present"};
    } else if (propertiesElem.type() != BSONType::Array) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << InternalSchemaAllowedPropertiesMatchExpression::kName
                              << " requires 'properties' to be an array, not "
                              << propertiesElem.type()};
    }

    std::vector<StringData> properties;
    for (auto&& property : propertiesElem.embeddedObject()) {
        if (property.type() != BSONType::String) {
            return {
                ErrorCodes::TypeMismatch,
                str::stream() << InternalSchemaAllowedPropertiesMatchExpression::kName
                              << " requires 'properties' to be an array of strings, but found a "
                              << property.type()};
        }
        properties.push_back(property.valueStringData());
    }

    return boost::container::flat_set<StringData>(properties.begin(), properties.end());
}
}  // namespace

StatusWithMatchExpression MatchExpressionParser::_parseInternalSchemaMatchArrayIndex(
    const char* path, const BSONElement& elem, const CollatorInterface* collator) {
    if (elem.type() != BSONType::Object) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << InternalSchemaMatchArrayIndexMatchExpression::kName
                              << " must be an object"};
    }

    auto subobj = elem.embeddedObject();
    if (subobj.nFields() != 3) {
        return {ErrorCodes::FailedToParse,
                str::stream() << InternalSchemaMatchArrayIndexMatchExpression::kName
                              << " requires exactly three fields: 'index', "
                                 "'namePlaceholder' and 'expression'"};
    }

    auto index = parseIntegerElementToNonNegativeLong(subobj["index"]);
    if (!index.isOK()) {
        return index.getStatus();
    }

    auto namePlaceholder = parseNamePlaceholder(
        subobj, "namePlaceholder"_sd, InternalSchemaMatchArrayIndexMatchExpression::kName);
    if (!namePlaceholder.isOK()) {
        return namePlaceholder.getStatus();
    }

    auto expressionWithPlaceholder =
        parseExprWithPlaceholder(subobj,
                                 "expression"_sd,
                                 InternalSchemaMatchArrayIndexMatchExpression::kName,
                                 namePlaceholder.getValue(),
                                 collator);
    if (!expressionWithPlaceholder.isOK()) {
        return expressionWithPlaceholder.getStatus();
    }

    auto matchArrayIndexExpr = stdx::make_unique<InternalSchemaMatchArrayIndexMatchExpression>();
    auto initStatus = matchArrayIndexExpr->init(
        path, index.getValue(), std::move(expressionWithPlaceholder.getValue()));
    if (!initStatus.isOK()) {
        return initStatus;
    }
    return {std::move(matchArrayIndexExpr)};
}

StatusWithMatchExpression MatchExpressionParser::_parseInternalSchemaAllowedProperties(
    const BSONElement& elem, const CollatorInterface* collator) {
    if (elem.type() != BSONType::Object) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << InternalSchemaAllowedPropertiesMatchExpression::kName
                              << " must be an object"};
    }

    auto subobj = elem.embeddedObject();
    if (subobj.nFields() != 4) {
        return {ErrorCodes::FailedToParse,
                str::stream() << InternalSchemaAllowedPropertiesMatchExpression::kName
                              << " requires exactly four fields: 'properties', 'namePlaceholder', "
                                 "'patternProperties' and 'otherwise'"};
    }

    auto namePlaceholder = parseNamePlaceholder(
        subobj, "namePlaceholder"_sd, InternalSchemaAllowedPropertiesMatchExpression::kName);
    if (!namePlaceholder.isOK()) {
        return namePlaceholder.getStatus();
    }

    auto patternProperties =
        parsePatternProperties(subobj["patternProperties"], namePlaceholder.getValue(), collator);
    if (!patternProperties.isOK()) {
        return patternProperties.getStatus();
    }

    auto otherwise = parseExprWithPlaceholder(subobj,
                                              "otherwise"_sd,
                                              InternalSchemaAllowedPropertiesMatchExpression::kName,
                                              namePlaceholder.getValue(),
                                              collator);
    if (!otherwise.isOK()) {
        return otherwise.getStatus();
    }

    auto properties = parseProperties(subobj["properties"]);
    if (!properties.isOK()) {
        return properties.getStatus();
    }

    auto allowedPropertiesExpr =
        stdx::make_unique<InternalSchemaAllowedPropertiesMatchExpression>();
    allowedPropertiesExpr->init(std::move(properties.getValue()),
                                namePlaceholder.getValue(),
                                std::move(patternProperties.getValue()),
                                std::move(otherwise.getValue()));
    return {std::move(allowedPropertiesExpr)};
}

StatusWithMatchExpression MatchExpressionParser::_parseGeo(const char* name,
                                                           PathAcceptingKeyword type,
                                                           const BSONObj& section,
                                                           AllowedFeatureSet allowedFeatures) {
    if (PathAcceptingKeyword::WITHIN == type || PathAcceptingKeyword::GEO_INTERSECTS == type) {
        std::unique_ptr<GeoExpression> gq = stdx::make_unique<GeoExpression>(name);
        Status parseStatus = gq->parseFrom(section);

        if (!parseStatus.isOK())
            return StatusWithMatchExpression(parseStatus);

        std::unique_ptr<GeoMatchExpression> e = stdx::make_unique<GeoMatchExpression>();

        Status s = e->init(name, gq.release(), section);
        if (!s.isOK())
            return StatusWithMatchExpression(s);
        return {std::move(e)};
    } else {
        invariant(PathAcceptingKeyword::GEO_NEAR == type);

        if ((allowedFeatures & AllowedFeatures::kGeoNear) == 0u) {
            return {Status(ErrorCodes::BadValue,
                           "$geoNear, $near, and $nearSphere are not allowed in this context")};
        }

        std::unique_ptr<GeoNearExpression> nq = stdx::make_unique<GeoNearExpression>(name);
        Status s = nq->parseFrom(section);
        if (!s.isOK()) {
            return StatusWithMatchExpression(s);
        }
        std::unique_ptr<GeoNearMatchExpression> e = stdx::make_unique<GeoNearMatchExpression>();
        s = e->init(name, nq.release(), section);
        if (!s.isOK())
            return StatusWithMatchExpression(s);
        return {std::move(e)};
    }
}

bool MatchExpressionParser::_isAggExpression(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    if (!expCtx) {
        return false;
    }

    if (BSONType::Object != elem.type()) {
        return false;
    }

    auto obj = elem.embeddedObject();
    if (obj.nFields() != 1) {
        return false;
    }

    return obj.firstElementFieldName() == kAggExpression;
}

StatusWith<boost::intrusive_ptr<Expression>> MatchExpressionParser::_parseAggExpression(
    BSONElement elem,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    AllowedFeatureSet allowedFeatures) {
    invariant(expCtx);

    if ((allowedFeatures & AllowedFeatures::kExpr) == 0u) {
        return {Status(ErrorCodes::BadValue, "$expr is not allowed in this context")};
    }

    auto expr = Expression::parseOperand(
        expCtx, elem.embeddedObject().firstElement(), expCtx->variablesParseState);
    return expr->optimize();
}

namespace {
// Maps from query operator string name to operator PathAcceptingKeyword.
std::unique_ptr<StringMap<PathAcceptingKeyword>> queryOperatorMap;

MONGO_INITIALIZER(MatchExpressionParser)(InitializerContext* context) {
    queryOperatorMap =
        stdx::make_unique<StringMap<PathAcceptingKeyword>>(StringMap<PathAcceptingKeyword>{
            // TODO: SERVER-19565 Add $eq after auditing callers.
            {"_internalSchemaAllElemMatchFromIndex",
             PathAcceptingKeyword::INTERNAL_SCHEMA_ALL_ELEM_MATCH_FROM_INDEX},
            {"_internalSchemaFmod", PathAcceptingKeyword::INTERNAL_SCHEMA_FMOD},
            {"_internalSchemaMatchArrayIndex",
             PathAcceptingKeyword::INTERNAL_SCHEMA_MATCH_ARRAY_INDEX},
            {"_internalSchemaMaxItems", PathAcceptingKeyword::INTERNAL_SCHEMA_MAX_ITEMS},
            {"_internalSchemaMaxLength", PathAcceptingKeyword::INTERNAL_SCHEMA_MAX_LENGTH},
            {"_internalSchemaMaxLength", PathAcceptingKeyword::INTERNAL_SCHEMA_MAX_LENGTH},
            {"_internalSchemaMinItems", PathAcceptingKeyword::INTERNAL_SCHEMA_MIN_ITEMS},
            {"_internalSchemaMinItems", PathAcceptingKeyword::INTERNAL_SCHEMA_MIN_ITEMS},
            {"_internalSchemaMinLength", PathAcceptingKeyword::INTERNAL_SCHEMA_MIN_LENGTH},
            {"_internalSchemaObjectMatch", PathAcceptingKeyword::INTERNAL_SCHEMA_OBJECT_MATCH},
            {"_internalSchemaType", PathAcceptingKeyword::INTERNAL_SCHEMA_TYPE},
            {"_internalSchemaUniqueItems", PathAcceptingKeyword::INTERNAL_SCHEMA_UNIQUE_ITEMS},
            {"all", PathAcceptingKeyword::ALL},
            {"bitsAllClear", PathAcceptingKeyword::BITS_ALL_CLEAR},
            {"bitsAllSet", PathAcceptingKeyword::BITS_ALL_SET},
            {"bitsAnyClear", PathAcceptingKeyword::BITS_ANY_CLEAR},
            {"bitsAnySet", PathAcceptingKeyword::BITS_ANY_SET},
            {"elemMatch", PathAcceptingKeyword::ELEM_MATCH},
            {"exists", PathAcceptingKeyword::EXISTS},
            {"geoIntersects", PathAcceptingKeyword::GEO_INTERSECTS},
            {"geoNear", PathAcceptingKeyword::GEO_NEAR},
            {"geoWithin", PathAcceptingKeyword::WITHIN},
            {"gt", PathAcceptingKeyword::GREATER_THAN},
            {"gte", PathAcceptingKeyword::GREATER_THAN_OR_EQUAL},
            {"in", PathAcceptingKeyword::IN_EXPR},
            {"lt", PathAcceptingKeyword::LESS_THAN},
            {"lte", PathAcceptingKeyword::LESS_THAN_OR_EQUAL},
            {"mod", PathAcceptingKeyword::MOD},
            {"ne", PathAcceptingKeyword::NOT_EQUAL},
            {"near", PathAcceptingKeyword::GEO_NEAR},
            {"nearSphere", PathAcceptingKeyword::GEO_NEAR},
            {"nin", PathAcceptingKeyword::NOT_IN},
            {"options", PathAcceptingKeyword::OPTIONS},
            {"regex", PathAcceptingKeyword::REGEX},
            {"size", PathAcceptingKeyword::SIZE},
            {"type", PathAcceptingKeyword::TYPE},
            {"within", PathAcceptingKeyword::WITHIN},
        });
    return Status::OK();
}
}  // anonymous namespace

boost::optional<PathAcceptingKeyword> MatchExpressionParser::parsePathAcceptingKeyword(
    BSONElement typeElem, boost::optional<PathAcceptingKeyword> defaultKeyword) {
    auto fieldName = typeElem.fieldName();
    if (fieldName[0] == '$' && fieldName[1]) {
        auto opName = typeElem.fieldNameStringData().substr(1);
        auto queryOp = queryOperatorMap->find(opName);

        if (queryOp == queryOperatorMap->end()) {
            return defaultKeyword;
        }
        return queryOp->second;
    }
    return defaultKeyword;
}
}  // namespace mongo
