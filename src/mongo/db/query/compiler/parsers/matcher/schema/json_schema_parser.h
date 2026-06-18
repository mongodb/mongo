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

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/matcher/matcher_type_set.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

class JSONSchemaParser {
public:
    // Primitive type name constants.
    static constexpr std::string_view kSchemaTypeArray{"array"};
    static constexpr std::string_view kSchemaTypeBoolean{"boolean"};
    static constexpr std::string_view kSchemaTypeNull{"null"};
    static constexpr std::string_view kSchemaTypeObject{"object"};
    static constexpr std::string_view kSchemaTypeString{"string"};

    // Explicitly unsupported type name constants.
    static constexpr std::string_view kSchemaTypeInteger{"integer"};

    // Standard JSON Schema keyword constants.
    static constexpr std::string_view kSchemaAdditionalItemsKeyword{"additionalItems"};
    static constexpr std::string_view kSchemaAdditionalPropertiesKeyword{"additionalProperties"};
    static constexpr std::string_view kSchemaAllOfKeyword{"allOf"};
    static constexpr std::string_view kSchemaAnyOfKeyword{"anyOf"};
    static constexpr std::string_view kSchemaDependenciesKeyword{"dependencies"};
    static constexpr std::string_view kSchemaDescriptionKeyword{"description"};
    static constexpr std::string_view kSchemaEnumKeyword{"enum"};
    static constexpr std::string_view kSchemaExclusiveMaximumKeyword{"exclusiveMaximum"};
    static constexpr std::string_view kSchemaExclusiveMinimumKeyword{"exclusiveMinimum"};
    static constexpr std::string_view kSchemaItemsKeyword{"items"};
    static constexpr std::string_view kSchemaMaxItemsKeyword{"maxItems"};
    static constexpr std::string_view kSchemaMaxLengthKeyword{"maxLength"};
    static constexpr std::string_view kSchemaMaxPropertiesKeyword{"maxProperties"};
    static constexpr std::string_view kSchemaMaximumKeyword{"maximum"};
    static constexpr std::string_view kSchemaMinItemsKeyword{"minItems"};
    static constexpr std::string_view kSchemaMinLengthKeyword{"minLength"};
    static constexpr std::string_view kSchemaMinPropertiesKeyword{"minProperties"};
    static constexpr std::string_view kSchemaMinimumKeyword{"minimum"};
    static constexpr std::string_view kSchemaMultipleOfKeyword{"multipleOf"};
    static constexpr std::string_view kSchemaNotKeyword{"not"};
    static constexpr std::string_view kSchemaOneOfKeyword{"oneOf"};
    static constexpr std::string_view kSchemaPatternKeyword{"pattern"};
    static constexpr std::string_view kSchemaPatternPropertiesKeyword{"patternProperties"};
    static constexpr std::string_view kSchemaPropertiesKeyword{"properties"};
    static constexpr std::string_view kSchemaRequiredKeyword{"required"};
    static constexpr std::string_view kSchemaTitleKeyword{"title"};
    static constexpr std::string_view kSchemaTypeKeyword{"type"};
    static constexpr std::string_view kSchemaUniqueItemsKeyword{"uniqueItems"};

    // MongoDB-specific (non-standard) JSON Schema keyword constants.
    static constexpr std::string_view kSchemaBsonTypeKeyword{"bsonType"};
    static constexpr std::string_view kSchemaEncryptKeyword{"encrypt"};
    static constexpr std::string_view kSchemaEncryptMetadataKeyword{"encryptMetadata"};

    // A name of placeholder used in ExpressionWithPlaceholder expressions.
    static constexpr std::string_view kNamePlaceholder{"i"};

    /**
     * Converts a JSON schema, represented as BSON, into a semantically equivalent match expression
     * tree. Returns a non-OK status if the schema is invalid or cannot be parsed.
     */
    static StatusWithMatchExpression parse(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        BSONObj schema,
        MatchExpressionParser::AllowedFeatureSet allowedFeatures =
            MatchExpressionParser::kAllowAllSpecialFeatures,
        bool ignoreUnknownKeywords = false);

    /**
     * Builds a set of type aliases from the given type element using 'aliasMapFind'. Returns a
     * non-OK status if 'typeElt' is invalid or does not contain an entry in the 'aliasMap' by
     * calling 'aliasMapFind'.
     */
    static StatusWith<MatcherTypeSet> parseTypeSet(BSONElement typeElt,
                                                   const findBSONTypeAliasFun& aliasMapFind);
};

}  // namespace mongo
