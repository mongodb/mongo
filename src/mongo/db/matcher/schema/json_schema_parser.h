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

#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_type.h"

namespace mongo {

class JSONSchemaParser {
public:
    // Primitive type name constants.
    static constexpr StringData kSchemaTypeArray = "array"_sd;
    static constexpr StringData kSchemaTypeBoolean = "boolean"_sd;
    static constexpr StringData kSchemaTypeNull = "null"_sd;
    static constexpr StringData kSchemaTypeObject = "object"_sd;
    static constexpr StringData kSchemaTypeString = "string"_sd;

    // Explicitly unsupported type name constants.
    static constexpr StringData kSchemaTypeInteger = "integer"_sd;

    // Standard JSON Schema keyword constants.
    static constexpr StringData kSchemaAdditionalItemsKeyword = "additionalItems"_sd;
    static constexpr StringData kSchemaAdditionalPropertiesKeyword = "additionalProperties"_sd;
    static constexpr StringData kSchemaAllOfKeyword = "allOf"_sd;
    static constexpr StringData kSchemaAnyOfKeyword = "anyOf"_sd;
    static constexpr StringData kSchemaDependenciesKeyword = "dependencies"_sd;
    static constexpr StringData kSchemaDescriptionKeyword = "description"_sd;
    static constexpr StringData kSchemaEnumKeyword = "enum"_sd;
    static constexpr StringData kSchemaExclusiveMaximumKeyword = "exclusiveMaximum"_sd;
    static constexpr StringData kSchemaExclusiveMinimumKeyword = "exclusiveMinimum"_sd;
    static constexpr StringData kSchemaItemsKeyword = "items"_sd;
    static constexpr StringData kSchemaMaxItemsKeyword = "maxItems"_sd;
    static constexpr StringData kSchemaMaxLengthKeyword = "maxLength"_sd;
    static constexpr StringData kSchemaMaxPropertiesKeyword = "maxProperties"_sd;
    static constexpr StringData kSchemaMaximumKeyword = "maximum"_sd;
    static constexpr StringData kSchemaMinItemsKeyword = "minItems"_sd;
    static constexpr StringData kSchemaMinLengthKeyword = "minLength"_sd;
    static constexpr StringData kSchemaMinPropertiesKeyword = "minProperties"_sd;
    static constexpr StringData kSchemaMinimumKeyword = "minimum"_sd;
    static constexpr StringData kSchemaMultipleOfKeyword = "multipleOf"_sd;
    static constexpr StringData kSchemaNotKeyword = "not"_sd;
    static constexpr StringData kSchemaOneOfKeyword = "oneOf"_sd;
    static constexpr StringData kSchemaPatternKeyword = "pattern"_sd;
    static constexpr StringData kSchemaPatternPropertiesKeyword = "patternProperties"_sd;
    static constexpr StringData kSchemaPropertiesKeyword = "properties"_sd;
    static constexpr StringData kSchemaRequiredKeyword = "required"_sd;
    static constexpr StringData kSchemaTitleKeyword = "title"_sd;
    static constexpr StringData kSchemaTypeKeyword = "type"_sd;
    static constexpr StringData kSchemaUniqueItemsKeyword = "uniqueItems"_sd;

    // MongoDB-specific (non-standard) JSON Schema keyword constants.
    static constexpr StringData kSchemaBsonTypeKeyword = "bsonType"_sd;
    static constexpr StringData kSchemaEncryptKeyword = "encrypt"_sd;
    static constexpr StringData kSchemaEncryptMetadataKeyword = "encryptMetadata"_sd;

    /**
     * Converts a JSON schema, represented as BSON, into a semantically equivalent match expression
     * tree. Returns a non-OK status if the schema is invalid or cannot be parsed.
     */
    static StatusWithMatchExpression parse(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                           BSONObj schema,
                                           bool ignoreUnknownKeywords = false);

    /**
     * Builds a set of type aliases from the given type element using 'aliasMap'. Returns a non-OK
     * status if 'typeElt' is invalid or does not contain an entry in the 'aliasMap'.
     */
    static StatusWith<MatcherTypeSet> parseTypeSet(BSONElement typeElt,
                                                   const StringMap<BSONType>& aliasMap);
};

}  // namespace mongo
