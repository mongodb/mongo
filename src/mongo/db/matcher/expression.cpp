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
