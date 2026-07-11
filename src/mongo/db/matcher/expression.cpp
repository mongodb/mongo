// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/expression.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/query/compiler/parsers/matcher/schema/json_schema_parser.h"

#include <algorithm>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Enabling the disableMatchExpressionOptimization fail point will stop match expressions from
 * being optimized.
 */
MONGO_FAIL_POINT_DEFINE(disableMatchExpressionOptimization);

MatchExpression::MatchExpression(MatchType type, clonable_ptr<ErrorAnnotation> annotation)
    : _errorAnnotation(std::move(annotation)), _matchType(type) {}

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
    if (title.type() == BSONType::string) {
        this->title = {title.String()};
    }

    auto description = jsonSchemaElement[JSONSchemaParser::kSchemaDescriptionKeyword];
    if (description.type() == BSONType::string) {
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
