/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/matcher/expression.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/matcher/expression_parameterization.h"
#include "mongo/db/matcher/schema/json_schema_parser.h"

namespace mongo {

/**
 * Enabling the disableMatchExpressionOptimization fail point will stop match expressions from
 * being optimized.
 */
MONGO_FAIL_POINT_DEFINE(disableMatchExpressionOptimization);

namespace {
/**
 * Comparator for MatchExpression nodes.  Returns an integer less than, equal to, or greater
 * than zero if 'lhs' is less than, equal to, or greater than 'rhs', respectively.
 *
 * Sorts by:
 * 1) operator type (MatchExpression::MatchType)
 * 2) path name (MatchExpression::path())
 * 3) sort order of children
 * 4) number of children (MatchExpression::numChildren())
 *
 * The third item is needed to ensure that match expression trees which should have the same
 * cache key always sort the same way. If you're wondering when the tuple (operator type, path
 * name) could ever be equal, consider this query:
 *
 * {$and:[{$or:[{a:1},{a:2}]},{$or:[{a:1},{b:2}]}]}
 *
 * The two OR nodes would compare as equal in this case were it not for tuple item #3 (sort
 * order of children).
 */
int matchExpressionComparator(const MatchExpression* lhs, const MatchExpression* rhs) {
    MatchExpression::MatchType lhsMatchType = lhs->matchType();
    MatchExpression::MatchType rhsMatchType = rhs->matchType();
    if (lhsMatchType != rhsMatchType) {
        return lhsMatchType < rhsMatchType ? -1 : 1;
    }

    StringData lhsPath = lhs->path();
    StringData rhsPath = rhs->path();
    int pathsCompare = lhsPath.compare(rhsPath);
    if (pathsCompare != 0) {
        return pathsCompare;
    }

    const size_t numChildren = std::min(lhs->numChildren(), rhs->numChildren());
    for (size_t childIdx = 0; childIdx < numChildren; ++childIdx) {
        int childCompare =
            matchExpressionComparator(lhs->getChild(childIdx), rhs->getChild(childIdx));
        if (childCompare != 0) {
            return childCompare;
        }
    }

    if (lhs->numChildren() != rhs->numChildren()) {
        return lhs->numChildren() < rhs->numChildren() ? -1 : 1;
    }

    // They're equal!
    return 0;
}

bool matchExpressionLessThan(const MatchExpression* lhs, const MatchExpression* rhs) {
    return matchExpressionComparator(lhs, rhs) < 0;
}

}  // namespace

MatchExpression::MatchExpression(MatchType type, clonable_ptr<ErrorAnnotation> annotation)
    : _errorAnnotation(std::move(annotation)), _matchType(type) {}

// static
void MatchExpression::sortTree(MatchExpression* tree) {
    for (size_t i = 0; i < tree->numChildren(); ++i) {
        sortTree(tree->getChild(i));
    }
    if (auto&& children = tree->getChildVector()) {
        std::stable_sort(children->begin(), children->end(), [](auto&& lhs, auto&& rhs) {
            return matchExpressionLessThan(lhs.get(), rhs.get());
        });
    }
}

// static
std::vector<const MatchExpression*> MatchExpression::parameterize(
    MatchExpression* tree, boost::optional<size_t> maxParameterCount) {
    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    MatchExpressionParameterizationWalker walker{&visitor};
    tree_walker::walk<false, MatchExpression>(tree, &walker);

    // If the number of parameters exceed the maxParameterCount limit, we need to clear all ParamIds
    // that were set on expression nodes.
    //
    // The alternative could be to count the parameters first and then set the ParamIds, but that
    // would result in performing always two passes, rather than just one pass in a happy case.
    if (maxParameterCount && context.inputParamIdToExpressionMap.size() > *maxParameterCount) {
        context.revertMode = true;
        context.inputParamIdToExpressionMap.clear();
        tree_walker::walk<false, MatchExpression>(tree, &walker);
    }

    return std::move(context.inputParamIdToExpressionMap);
}

std::string MatchExpression::toString() const {
    return serialize().toString();
}

std::string MatchExpression::debugString() const {
    StringBuilder builder;
    debugString(builder, 0);
    return builder.str();
}

void MatchExpression::_debugAddSpace(StringBuilder& debug, int indentationLevel) const {
    for (int i = 0; i < indentationLevel; i++)
        debug << "    ";
}

bool MatchExpression::matchesBSON(const BSONObj& doc, MatchDetails* details) const {
    BSONMatchableDocument mydoc(doc);
    return matches(&mydoc, details);
}

bool MatchExpression::matchesBSONElement(BSONElement elem, MatchDetails* details) const {
    BSONElementViewMatchableDocument matchableDoc(elem);
    return matches(&matchableDoc, details);
}

void MatchExpression::setCollator(const CollatorInterface* collator) {
    for (size_t i = 0; i < numChildren(); ++i) {
        getChild(i)->setCollator(collator);
    }

    _doSetCollator(collator);
}

bool MatchExpression::isInternalNodeWithPath(MatchType m) {
    switch (m) {
        case ELEM_MATCH_OBJECT:
        case ELEM_MATCH_VALUE:
        case INTERNAL_SCHEMA_OBJECT_MATCH:
        case INTERNAL_SCHEMA_MATCH_ARRAY_INDEX:
            // This node generates a child expression with a field that isn't prefixed by the path
            // of the node.
        case INTERNAL_SCHEMA_ALL_ELEM_MATCH_FROM_INDEX:
            // This node generates a child expression with a field that isn't prefixed by the path
            // of the node.
            return true;

        case AND:
        case OR:
        case SIZE:
        case EQ:
        case LTE:
        case LT:
        case GT:
        case GTE:
        case REGEX:
        case MOD:
        case EXISTS:
        case MATCH_IN:
        case BITS_ALL_SET:
        case BITS_ALL_CLEAR:
        case BITS_ANY_SET:
        case BITS_ANY_CLEAR:
        case NOT:
        case NOR:
        case TYPE_OPERATOR:
        case GEO:
        case WHERE:
        case EXPRESSION:
        case ALWAYS_FALSE:
        case ALWAYS_TRUE:
        case GEO_NEAR:
        case TEXT:
        case INTERNAL_2D_POINT_IN_ANNULUS:
        case INTERNAL_BUCKET_GEO_WITHIN:
        case INTERNAL_EXPR_EQ:
        case INTERNAL_EXPR_GT:
        case INTERNAL_EXPR_GTE:
        case INTERNAL_EXPR_LT:
        case INTERNAL_EXPR_LTE:
        case INTERNAL_EQ_HASHED_KEY:
        case INTERNAL_SCHEMA_ALLOWED_PROPERTIES:
        case INTERNAL_SCHEMA_BIN_DATA_ENCRYPTED_TYPE:
        case INTERNAL_SCHEMA_BIN_DATA_FLE2_ENCRYPTED_TYPE:
        case INTERNAL_SCHEMA_BIN_DATA_SUBTYPE:
        case INTERNAL_SCHEMA_COND:
        case INTERNAL_SCHEMA_EQ:
        case INTERNAL_SCHEMA_FMOD:
        case INTERNAL_SCHEMA_MAX_ITEMS:
        case INTERNAL_SCHEMA_MAX_LENGTH:
        case INTERNAL_SCHEMA_MAX_PROPERTIES:
        case INTERNAL_SCHEMA_MIN_ITEMS:
        case INTERNAL_SCHEMA_MIN_LENGTH:
        case INTERNAL_SCHEMA_MIN_PROPERTIES:
        case INTERNAL_SCHEMA_ROOT_DOC_EQ:
        case INTERNAL_SCHEMA_TYPE:
        case INTERNAL_SCHEMA_UNIQUE_ITEMS:
        case INTERNAL_SCHEMA_XOR:
            return false;
    }
    MONGO_UNREACHABLE;
}

MatchExpression::ErrorAnnotation::SchemaAnnotations::SchemaAnnotations(
    const BSONObj& jsonSchemaElement) {
    auto title = jsonSchemaElement[JSONSchemaParser::kSchemaTitleKeyword];
    if (title.type() == BSONType::String) {
        this->title = {title.String()};
    }

    auto description = jsonSchemaElement[JSONSchemaParser::kSchemaDescriptionKeyword];
    if (description.type() == BSONType::String) {
        this->description = {description.String()};
    }
}

void MatchExpression::ErrorAnnotation::SchemaAnnotations::appendElements(
    BSONObjBuilder& builder) const {
    if (title) {
        builder << JSONSchemaParser::kSchemaTitleKeyword << title.value();
    }

    if (description) {
        builder << JSONSchemaParser::kSchemaDescriptionKeyword << description.value();
    }
}
}  // namespace mongo
