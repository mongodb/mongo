/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/json.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/matcher/doc_validation/doc_validation_error_test.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/unittest/unittest.h"

#include <cstring>

#include <sys/types.h>

namespace mongo {
namespace {

// $jsonSchema
TEST(JSONSchemaValidation, BasicJsonSchemaWithTitleAndDescription) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'properties': {'a': {'minimum': 1}},"
        "title: 'example title', description: 'example description'}}");
    BSONObj document = fromjson("{a: 0}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        " 'title': 'example title',"
        " 'description': 'example description',"
        "      'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'properties',"
        "            'propertiesNotSatisfied': ["
        "                   {'propertyName': 'a', 'details': "
        "                       [{'operatorName': 'minimum',"
        "                       'specifiedAs': {'minimum' : 1},"
        "                       'reason': 'comparison failed',"
        "                       'consideredValue': 0}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// properties
TEST(JSONSchemaValidation, BasicProperties) {
    BSONObj query = fromjson("{'$jsonSchema': {'properties': {'a': {'minimum': 1}}}}");
    BSONObj document = fromjson("{a: 0}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "      'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'properties',"
        "            'propertiesNotSatisfied': ["
        "                   {'propertyName': 'a', 'details': "
        "                       [{'operatorName': 'minimum',"
        "                       'specifiedAs': {'minimum' : 1},"
        "                       'reason': 'comparison failed',"
        "                       'consideredValue': 0}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// minimum
TEST(JSONSchemaValidation, MinimumNonNumericWithType) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'properties': {'a': {'type': 'number','minimum': 1,"
        "title: 'property a', description: 'a >= 1'}}}}");
    BSONObj document = fromjson("{'a': 'foo'}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "   {operatorName: 'properties', 'propertiesNotSatisfied': ["
        "       {propertyName: 'a',"
        "        'title': 'property a', 'description': 'a >= 1',"
        "        'details': ["
        "           {'operatorName': 'type', "
        "           'specifiedAs': { 'type': 'number' }, "
        "           'reason': 'type did not match', "
        "           'consideredValue': 'foo', "
        "           'consideredType': 'string'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MinimumNonNumericWithBSONType) {
    BSONObj query =
        fromjson("{'$jsonSchema': {'properties': {'a': {'bsonType': 'int','minimum': 1}}}}");
    BSONObj document = fromjson("{'a': 1.1}");
    // The value satisfies the minimum keyword, but not the bsonType keyword.
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "   {operatorName: 'properties', 'propertiesNotSatisfied': ["
        "       {propertyName: 'a', 'details': ["
        "           {'operatorName': 'bsonType', "
        "           'specifiedAs': {'bsonType': 'int'}, "
        "           'reason': 'type did not match', "
        "           'consideredValue': 1.1, "
        "           'consideredType': 'double'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MinimumWithRequiredNoTypeMissingProperty) {
    BSONObj query =
        fromjson("{'$jsonSchema': {'required': ['a'], 'properties': {'a': {'minimum': 1}}}}");
    BSONObj document = fromjson("{}");
    // Should not mention 'minimum'.
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'required',"
        "   specifiedAs: {required: ['a']}, "
        "   missingProperties: ['a']}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MinimumWithRequiredAndTypeMissingProperty) {
    BSONObj query = fromjson(
        "{'$jsonSchema': "
        "   {'required': ['a'],"
        "   'properties': "
        "       {a: {'type': 'number','minimum': 1}}}}");
    BSONObj document = fromjson("{}");
    // Should not mention 'minimum', nor 'type'.
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'required',"
        "   specifiedAs: {required: ['a']}, "
        "   missingProperties: ['a']}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MinimumRequiredWithTypeAndScalarFailedMinimum) {
    BSONObj query = fromjson(
        "{'$jsonSchema': "
        "   {'properties': {'a': {minimum: 2, 'bsonType': 'int'}}, 'required': ['a','b']}}");
    BSONObj document = fromjson("{a: 1, b: 1}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "      'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'properties',"
        "            'propertiesNotSatisfied': ["
        "                   {'propertyName': 'a', 'details': "
        "                       [{'operatorName': 'minimum',"
        "                       'specifiedAs': {'minimum' : 2},"
        "                       'reason': 'comparison failed',"
        "                       'consideredValue': 1}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// maximum
TEST(JSONSchemaValidation, MaximumNonNumericWithType) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'properties': {'a': {'type': 'number','maximum': 1,"
        "title: 'property a', description: 'a <= 1'}}}}");
    BSONObj document = fromjson("{'a': 'foo'}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "   {operatorName: 'properties', 'propertiesNotSatisfied': ["
        "       {propertyName: 'a',"
        "        'title': 'property a', 'description': 'a <= 1',"
        "        'details': ["
        "           {'operatorName': 'type', "
        "           'specifiedAs': { 'type': 'number' }, "
        "           'reason': 'type did not match', "
        "           'consideredValue': 'foo', "
        "           'consideredType': 'string'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MaximumNonNumericWithBSONType) {
    BSONObj query =
        fromjson("{'$jsonSchema': {'properties': {'a': {'bsonType': 'int','maximum': 1}}}}");
    BSONObj document = fromjson("{'a': 0.9}");
    // The value satisfies the maximum keyword, but not the bsonType keyword.
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "   {operatorName: 'properties', 'propertiesNotSatisfied': ["
        "       {propertyName: 'a', 'details': ["
        "           {'operatorName': 'bsonType', "
        "           'specifiedAs': {'bsonType': 'int'}, "
        "           'reason': 'type did not match', "
        "           'consideredValue': 0.9, "
        "           'consideredType': 'double'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MaximumWithRequiredNoTypeMissingProperty) {
    BSONObj query =
        fromjson("{'$jsonSchema': {'required': ['a'], 'properties': {'a': {'maximum': 1}}}}");
    BSONObj document = fromjson("{}");
    // Should not mention 'maximum'.
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'required',"
        "   specifiedAs: {required: ['a']}, "
        "   missingProperties: ['a']}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MaximumWithRequiredAndTypeMissingProperty) {
    BSONObj query = fromjson(
        "{'$jsonSchema': "
        "   {'required': ['a'],"
        "   'properties': "
        "       {a: {'type': 'number','maximum': 1}}}}");
    BSONObj document = fromjson("{}");
    // Should not mention 'maximum', nor 'type'.
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'required',"
        "   specifiedAs: {required: ['a']}, "
        "   missingProperties: ['a']}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MaximumRequiredWithTypeAndScalarFailedMaximum) {
    BSONObj query = fromjson(
        "{'$jsonSchema': "
        "   {'properties': {'a': {maximum: 2, 'bsonType': 'int'}}, 'required': ['a','b']}}");
    BSONObj document = fromjson("{a: 3, b: 1}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "      'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'properties',"
        "            'propertiesNotSatisfied': ["
        "                   {'propertyName': 'a', 'details': "
        "                       [{'operatorName': 'maximum',"
        "                       'specifiedAs': {'maximum' : 2},"
        "                       'reason': 'comparison failed',"
        "                       'consideredValue': 3}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// Exclusive minimum/maximum

TEST(JSONSchemaValidation, ExclusiveMinimum) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'properties': {'a': {'minimum': 1, 'exclusiveMinimum': true}}}}");
    BSONObj document = fromjson("{a: 1}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "      'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'properties',"
        "            'propertiesNotSatisfied': ["
        "                   {'propertyName': 'a', 'details': "
        "                       [{'operatorName': 'minimum',"
        "                       'specifiedAs': {'minimum' : 1, 'exclusiveMinimum': true},"
        "                       'reason': 'comparison failed',"
        "                       'consideredValue': 1}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ExclusiveMinimumInverted) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'not': {'properties': {'a': {'minimum': 1, 'exclusiveMinimum': "
        "true}}}}}");
    BSONObj document = fromjson("{a: 2}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "      'schemaRulesNotSatisfied': ["
        "       {'operatorName': 'not', 'reason': 'child expression matched'}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ExclusiveMinimumInvertedTypeMismatch) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'not': {'properties': {'a': {'minimum': 1, 'exclusiveMinimum': "
        "true}}}}}");
    BSONObj document = fromjson("{a: 'foo'}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "       {'operatorName': 'not', 'reason': 'child expression matched'}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ExclusiveMinimumInvertedMissingField) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'not': {'properties': {'a': {'minimum': 1, 'exclusiveMinimum': "
        "true}}}}}");
    BSONObj document = fromjson("{b: 100}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "       {'operatorName': 'not', 'reason': 'child expression matched'}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MinimumAtTopLevelHasNoEffect) {
    BSONObj query = fromjson("{'$nor': [{'$jsonSchema': {'minimum': 1}}]}");
    BSONObj document = fromjson("{a: 2}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$nor',"
        "     'clausesSatisfied': [{'index': 0, 'details': "
        "       {'operatorName': '$jsonSchema', 'reason': 'schema matched'}}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ExclusiveMaximum) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'properties': {'a': {'maximum': 1, 'exclusiveMaximum': true}}}}");
    BSONObj document = fromjson("{a: 1}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "      'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'properties',"
        "            'propertiesNotSatisfied': ["
        "                   {'propertyName': 'a', 'details': "
        "                       [{'operatorName': 'maximum',"
        "                       'specifiedAs': {'maximum' : 1, 'exclusiveMaximum': true},"
        "                       'reason': 'comparison failed',"
        "                       'consideredValue': 1}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MaximumTypeNumberWithEmptyArray) {
    BSONObj query =
        fromjson("{'$jsonSchema': {'properties': {'a': {'maximum': 1, type: 'number'}}}}");
    BSONObj document = fromjson("{a: []}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "      'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'properties',"
        "            'propertiesNotSatisfied': ["
        "                   {'propertyName': 'a', 'details': ["
        "                       {'operatorName': 'type',"
        "                       'specifiedAs': {'type': 'number'},"
        "                       'reason': 'type did not match',"
        "                       'consideredValue': [],"
        "                       'consideredType': 'array'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ExclusiveMaximumInverted) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'not': {'properties': {'a': {'maximum': 1, 'exclusiveMaximum': "
        "true}}}}}");
    BSONObj document = fromjson("{a: 0}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "      'schemaRulesNotSatisfied': ["
        "       {'operatorName': 'not', 'reason': 'child expression matched'}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MaximumAtTopLevelHasNoEffect) {
    BSONObj query = fromjson("{'$nor': [{'$jsonSchema': {'maximum': 1}}]}");
    BSONObj document = fromjson("{a: 2}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$nor',"
        "     'clausesSatisfied': [{'index': 0, 'details': "
        "       {'operatorName': '$jsonSchema', 'reason': 'schema matched'}}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, NestedProperties) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {"
        "   'properties': {"
        "       'a': {'properties': "
        "           {'b': {'minimum': 1}}}}}}");
    BSONObj document = fromjson("{'a': {'b': 0}}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "    'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'properties',"
        "            'propertiesNotSatisfied': ["
        "                   {'propertyName': 'a', "
        "                    'details':"
        "                       [{'operatorName': 'properties',"
        "                         'propertiesNotSatisfied': ["
        "                            {'propertyName': 'b', "
        "                             'details': ["
        "                                   {'operatorName': 'minimum',"
        "                                    'specifiedAs': {'minimum': 1},"
        "                                    'reason': 'comparison failed',"
        "                                    'consideredValue': 0}]}]}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, NestedPropertiesMultipleFailingClauses) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {"
        "   'properties': {"
        "       'a': {'properties': "
        "           {'b': {'minimum': 10, 'maximum': -10}}}}}}");
    BSONObj document = fromjson("{'a': {'b': 5}}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "    'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'properties',"
        "            'propertiesNotSatisfied': ["
        "                   {'propertyName': 'a', "
        "                    'details':"
        "                       [{'operatorName': 'properties',"
        "                         'propertiesNotSatisfied': ["
        "                            {'propertyName': 'b', "
        "                             'details': ["
        "                                    {'operatorName': 'maximum',"
        "                                    'specifiedAs': {'maximum': -10},"
        "                                    'reason': 'comparison failed',"
        "                                    'consideredValue': 5},"
        "                                   {'operatorName': 'minimum',"
        "                                    'specifiedAs': {'minimum': 10},"
        "                                    'reason': 'comparison failed',"
        "                                    'consideredValue': 5}]}]}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, InternalSchemaObjectMatchDottedPathsHandledCorrectly) {
    BSONObj query = fromjson("{'a.b': {'$_internalSchemaObjectMatch': {'c': 1}}}");
    BSONObj document = fromjson("{'a': {'b': {'c': 0}}}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$eq',"
        " 'specifiedAs': {'c' : 1},"
        "'reason': 'comparison failed',"
        "'consideredValue': 0}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, PropertiesObjectTypeMismatch) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {"
        "   'properties': {"
        "       'a': {'type': 'object', 'properties': {"
        "           'b': {'minimum': 10}}}}}}");
    BSONObj document = fromjson("{'a': 'eleven'}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'type',"
        "            'specifiedAs': {'type': 'object'},"
        "            'reason': 'type did not match',"
        "            'consideredValue': 'eleven',"
        "            'consideredType': 'string'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, TypeRestrictionContradictsSpecifiedType) {
    BSONObj query =
        fromjson("{$jsonSchema: {type: 'object', properties: {a: {type: 'string', minimum: 5}}}}");
    BSONObj document = fromjson("{a: 6}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'type',"
        "            'specifiedAs': {'type': 'string'},"
        "            'reason': 'type did not match',"
        "            'consideredValue': 6,"
        "            'consideredType': 'int'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MultipleNestedProperties) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {"
        "   'properties': {"
        "       'a': {'properties': {'b': {'minimum': 1}, 'c': {'minimum': 10}}},"
        "       'd': {'properties': {'e': {'minimum': 50}, 'f': {'minimum': 100}}}}}}");
    BSONObj document = fromjson("{'a': {'b': 0, 'c': 5}, 'd': {'e': 25, 'f': 50}}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "    'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'properties',"
        "            'propertiesNotSatisfied': ["
        "                   {'propertyName': 'a', "
        "                    'details':"
        "                       [{'operatorName': 'properties',"
        "                         'propertiesNotSatisfied': ["
        "                            {'propertyName': 'b', "
        "                             'details': ["
        "                                    {'operatorName': 'minimum',"
        "                                    'specifiedAs': {'minimum': 1},"
        "                                    'reason': 'comparison failed',"
        "                                    'consideredValue': 0}]},"
        "                             {'propertyName': 'c',         "
        "                             'details': ["
        "                                    {'operatorName': 'minimum',"
        "                                    'specifiedAs': {'minimum': 10},"
        "                                    'reason': 'comparison failed',"
        "                                    'consideredValue': 5}]}]}]},"
        "                   {'propertyName': 'd', "
        "                    'details':"
        "                       [{'operatorName': 'properties',"
        "                         'propertiesNotSatisfied': ["
        "                            {'propertyName': 'e', "
        "                             'details': ["
        "                                    {'operatorName': 'minimum',"
        "                                    'specifiedAs': {'minimum': 50},"
        "                                    'reason': 'comparison failed',"
        "                                    'consideredValue': 25}]},"
        "                             {'propertyName': 'f',         "
        "                             'details': ["
        "                                    {'operatorName': 'minimum',"
        "                                    'specifiedAs': {'minimum': 100},"
        "                                    'reason': 'comparison failed',"
        "                                    'consideredValue': 50}]}]}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, JSONSchemaAndQueryOperators) {
    BSONObj query = fromjson(
        "{$and: ["
        "{'$jsonSchema': {'properties': {'a': {'minimum': 1}}}},"
        "{'$jsonSchema': {'properties': {'b': {'minimum': 1}}}},"
        "{'$jsonSchema': {'properties': {'c': {'minimum': 1}}}}]}");
    BSONObj document = fromjson("{a: 0, b: 2, c: -1}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$and',"
        "     'clausesNotSatisfied': ["
        " {'index': 0, 'details': "
        "       {'operatorName': '$jsonSchema',"
        "       'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'properties',"
        "            'propertiesNotSatisfied': ["
        "                   {'propertyName': 'a', 'details': "
        "                       [{'operatorName': 'minimum',"
        "                       'specifiedAs': {'minimum' : 1},"
        "                       'reason': 'comparison failed',"
        "                       'consideredValue': 0}]}]}]}},"
        " {'index': 2, 'details': "
        "      {'operatorName': '$jsonSchema',"
        "      'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'properties',"
        "            'propertiesNotSatisfied': ["
        "                   {'propertyName': 'c', 'details': "
        "                       [{'operatorName': 'minimum',"
        "                       'specifiedAs': {'minimum' : 1},"
        "                       'reason': 'comparison failed',"
        "                       'consideredValue': -1}]}]}]}}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, NoTopLevelObjectTypeRejectsAllDocuments) {
    BSONObj query = fromjson(
        "  {'$jsonSchema': {'type': 'number',"
        "   'properties': {"
        "       'a': {'properties': "
        "           {'b': {'minimum': 1}}}}}}");
    BSONObj document = fromjson("{'a': {'b': 1}}");
    BSONObj expectedError = BSON("operatorName" << "$jsonSchema"
                                                << "specifiedAs" << query << "reason"
                                                << "expression always evaluates to false");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// type
TEST(JSONSchemaValidation, BasicType) {
    BSONObj query = fromjson("  {'$jsonSchema': {'properties': {'a': {'type': 'string'}}}}");
    BSONObj document = fromjson("{'a': {'b': 1}}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'type',"
        "            'specifiedAs': {'type': 'string'},"
        "            'reason': 'type did not match',"
        "            'consideredValue': {'b': 1},"
        "            'consideredType': 'object'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MultipleTypeFailures) {
    BSONObj query = fromjson(
        "  {'$jsonSchema':"
        "   {'properties':"
        "        {'a': {'type': 'string'}, "
        "         'b': {'type': 'number'}, "
        "         'c': {'type': 'object', title: 'property c', description: 'c is a string'}}}}");
    BSONObj document = fromjson("{'a': {'b': 1}, 'b': 4, 'c': 'foo'}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'type',"
        "            'specifiedAs': {'type': 'string'},"
        "            'reason': 'type did not match',"
        "            'consideredValue': {'b': 1},"
        "            'consideredType': 'object'}]},"
        "       {'propertyName': 'c',"
        "        'title': 'property c', 'description': 'c is a string',"
        "        'details': ["
        "           {'operatorName': 'type',"
        "            'specifiedAs': {'type': 'object'},"
        "            'reason': 'type did not match',"
        "            'consideredValue': 'foo',"
        "            'consideredType': 'string'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, TypeNoImplicitArrayTraversal) {
    BSONObj query = fromjson(
        "  {'$jsonSchema':"
        "   {'properties': "
        "       {'a': {'type': 'string'}}}}");
    // Even though 'a' is an array of strings, this is a type mismatch in the world of $jsonSchema.
    BSONObj document = fromjson("{'a': ['Mihai', 'was', 'here']}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'type',"
        "            'specifiedAs': {'type': 'string'},"
        "            'reason': 'type did not match',"
        "            'consideredValue': ['Mihai', 'was', 'here'],"
        "            'consideredType': 'array'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// bsonType
TEST(JSONSchemaValidation, BasicBSONType) {
    BSONObj query = fromjson(
        "  {'$jsonSchema':"
        "   {'properties': "
        "       {'a': {'bsonType': 'double'}}}}");
    BSONObj document = fromjson("{'a': {'b': 1}}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'bsonType',"
        "            'specifiedAs': {'bsonType': 'double'},"
        "            'reason': 'type did not match',"
        "            'consideredValue': {'b': 1},"
        "            'consideredType': 'object'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MultipleBSONTypeFailures) {
    BSONObj query = fromjson(
        "  {'$jsonSchema':"
        "   {'properties': "
        "        {'a': {'bsonType': 'double'}, "
        "         'b': {'bsonType': 'int'}, "
        "         'c': {'bsonType': 'decimal'}}}}");
    BSONObj document = fromjson("{'a': {'b': 1}, 'b': 4, 'c': 'foo'}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'bsonType',"
        "            'specifiedAs': {'bsonType': 'double'},"
        "            'reason': 'type did not match',"
        "            'consideredValue': {'b': 1},"
        "            'consideredType': 'object'}]},"
        "       {'propertyName': 'c', 'details': ["
        "           {'operatorName': 'bsonType',"
        "            'specifiedAs': {'bsonType': 'decimal'},"
        "            'reason': 'type did not match',"
        "            'consideredValue': 'foo',"
        "            'consideredType': 'string'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, BSONTypeNoImplicitArrayTraversal) {
    BSONObj query = fromjson(
        "  {'$jsonSchema':"
        "   {'properties': "
        "       {'a': {'bsonType': 'string',"
        "        title: 'property a', description: 'a is a string'}}}}");
    // Even though 'a' is an array of strings, this is a type mismatch in the world of $jsonSchema.
    BSONObj document = fromjson("{'a': ['Mihai', 'was', 'here']}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a',"
        "        'title': 'property a', 'description': 'a is a string',"
        "        'details': ["
        "           {'operatorName': 'bsonType',"
        "            'specifiedAs': {'bsonType': 'string'},"
        "            'reason': 'type did not match',"
        "            'consideredValue': ['Mihai', 'was', 'here'],"
        "            'consideredType': 'array'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// Scalar keywords

// minLength
TEST(JSONSchemaValidation, BasicMinLength) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'properties': {'a': {'type': 'string', 'minLength': 4,"
        "title: 'property a', description: 'a min length is 4'}}}}");
    BSONObj document = fromjson("{'a': 'foo'}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a',"
        "        'title': 'property a', 'description': 'a min length is 4',"
        "        'details': ["
        "           {'operatorName': 'minLength',"
        "            'specifiedAs': {'minLength': 4},"
        "            'reason': 'specified string length was not satisfied',"
        "            'consideredValue': 'foo'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MinLengthNoExplicitType) {
    BSONObj query = fromjson("{'$jsonSchema': {'properties': {'a': {'minLength': 4}}}}");
    BSONObj document = fromjson("{'a': 'foo'}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'minLength',"
        "            'specifiedAs': {'minLength': 4},"
        "            'reason': 'specified string length was not satisfied',"
        "            'consideredValue': 'foo'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MinLengthNonString) {
    BSONObj query =
        fromjson("{'$jsonSchema': {'properties': {'a': {'type': 'string','minLength': 4}}}}");
    BSONObj document = fromjson("{'a': 1}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'type', "
        "           'specifiedAs': { 'type': 'string' }, "
        "           'reason': 'type did not match', "
        "           'consideredValue': 1, "
        "           'consideredType': 'int' }]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MinLengthRequiredNoTypeMissingProperty) {
    BSONObj query =
        fromjson("{'$jsonSchema': {'required': ['a'], 'properties': {'a': {'minLength': 1}}}}");
    BSONObj document = fromjson("{}");
    // Should not mention 'minLength'.
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'required',"
        "   specifiedAs: {required: ['a']}, "
        "   missingProperties: ['a']}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MinLengthWithRequiredAndTypeMissingProperty) {
    BSONObj query = fromjson(
        "{'$jsonSchema': "
        "   {'required': ['a'],"
        "   'properties': "
        "       {a: {'type': 'string','minLength': 1}}}}");
    BSONObj document = fromjson("{}");
    // Should not mention 'minimum', nor 'type'.
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'required',"
        "   specifiedAs: {required: ['a']}, "
        "   missingProperties: ['a']}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MinLengthRequiredWithTypeAndScalarFailedMinLength) {
    BSONObj query = fromjson(
        "{'$jsonSchema': "
        "   {'properties': {'a': "
        "   {minLength: 2, 'bsonType': 'string'}}, 'required': ['a','b']}}");
    BSONObj document = fromjson("{a: 'a', b: 1}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "      'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'properties',"
        "            'propertiesNotSatisfied': ["
        "                   {'propertyName': 'a', 'details': "
        "                       [{'operatorName': 'minLength',"
        "                       'specifiedAs': {'minLength' : 2},"
        "                       'reason': 'specified string length was not satisfied',"
        "                       'consideredValue': 'a'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MinLengthNested) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {"
        "   'properties': {"
        "       'a': {'properties': "
        "           {'b': {'type': 'string', 'minLength': 4}}}}}}");
    BSONObj document = fromjson("{'a': {'b':'foo'}}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "    'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'properties',"
        "            'propertiesNotSatisfied': ["
        "                   {'propertyName': 'a', "
        "                    'details':"
        "                       [{'operatorName': 'properties',"
        "                         'propertiesNotSatisfied': ["
        "                            {'propertyName': 'b', "
        "                             'details': ["
        "                                      {'operatorName': 'minLength',"
        "                                       'specifiedAs': {'minLength': 4},"
        "                                       'reason': 'specified string length was not "
        "satisfied',"
        "                                       'consideredValue': 'foo'}]}]}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MinLengthAtTopLevelHasNoEffect) {
    BSONObj query = fromjson("{'$nor': [{'$jsonSchema': {'minLength': 1}}]}");
    BSONObj document = fromjson("{a: 'foo'}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$nor',"
        "     'clausesSatisfied': [{'index': 0, 'details': "
        "       {'operatorName': '$jsonSchema', 'reason': 'schema matched'}}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MinLengthInvertedLengthDoesMatch) {
    BSONObj query = fromjson("{'$jsonSchema': {'not': {'properties': {'a': {'minLength': 4}}}}}");
    BSONObj document = fromjson("{'a': 'this string is long'}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'not', 'reason': 'child expression matched'}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MinLengthInvertedTypeMismatch) {
    BSONObj query = fromjson("{'$jsonSchema': {'not': {'properties': {'a': {'minLength': 4}}}}}");
    BSONObj document = fromjson("{'a': 1}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "       {'operatorName': 'not', 'reason': 'child expression matched'}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MinLengthInvertedMissingProperty) {
    BSONObj query = fromjson("{'$jsonSchema': {'not': {'properties': {'a': {'minLength': 4}}}}}");
    BSONObj document = fromjson("{'b': 1}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "       {'operatorName': 'not', 'reason': 'child expression matched'}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// maxLength
TEST(JSONSchemaValidation, BasicMaxLength) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'properties': {'a': {'type': 'string','maxLength': 4,"
        "title: 'property a', description: 'a max length is 4'}}}}");
    BSONObj document = fromjson("{'a': 'foo, bar, baz'}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a',"
        "        'title': 'property a', 'description': 'a max length is 4',"
        "        'details': ["
        "           {'operatorName': 'maxLength',"
        "            'specifiedAs': {'maxLength': 4},"
        "            'reason': 'specified string length was not satisfied',"
        "            'consideredValue': 'foo, bar, baz'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MaxLengthRequiredNoTypeMissingProperty) {
    BSONObj query =
        fromjson("{'$jsonSchema': {'required': ['a'], 'properties': {'a': {'minLength': 1}}}}");
    BSONObj document = fromjson("{}");
    // Should not mention 'minLength'.
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'required',"
        "   specifiedAs: {required: ['a']}, "
        "   missingProperties: ['a']}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MaxLengthWithRequiredAndTypeMissingProperty) {
    BSONObj query = fromjson(
        "{'$jsonSchema': "
        "   {'required': ['a'],"
        "   'properties': "
        "       {a: {'type': 'string','minLength': 1}}}}");
    BSONObj document = fromjson("{}");
    // Should not mention 'maxLength', nor 'type'.
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'required',"
        "   specifiedAs: {required: ['a']}, "
        "   missingProperties: ['a']}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MaxLengthRequiredWithTypeAndScalarFailedMaxLength) {
    BSONObj query = fromjson(
        "{'$jsonSchema': "
        "   {'properties': {'a': "
        "   {maxLength: 2, 'bsonType': 'string'}}, 'required': ['a','b']}}");
    BSONObj document = fromjson("{a: 'aaaa', b: 1}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "      'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'properties',"
        "            'propertiesNotSatisfied': ["
        "                   {'propertyName': 'a', 'details': "
        "                       [{'operatorName': 'maxLength',"
        "                       'specifiedAs': {'maxLength' : 2},"
        "                       'reason': 'specified string length was not satisfied',"
        "                       'consideredValue': 'aaaa'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MaxLengthNoExplicitType) {
    BSONObj query = fromjson("{'$jsonSchema': {'properties': {'a': {'maxLength': 4}}}}");
    BSONObj document = fromjson("{'a': 'foo, bar, baz'}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'maxLength',"
        "            'specifiedAs': {'maxLength': 4},"
        "            'reason': 'specified string length was not satisfied',"
        "            'consideredValue': 'foo, bar, baz'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MaxLengthNonString) {
    BSONObj query =
        fromjson("{'$jsonSchema': {'properties': {'a': {'type': 'string','maxLength': 4}}}}");
    BSONObj document = fromjson("{'a': 1}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'type', "
        "           'specifiedAs': { 'type': 'string' }, "
        "           'reason': 'type did not match', "
        "           'consideredValue': 1, "
        "           'consideredType': 'int' }]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MaxLengthNested) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {"
        "   'properties': {"
        "       'a': {'properties': "
        "           {'b': {'type': 'string', 'maxLength': 4}}}}}}");
    BSONObj document = fromjson("{'a': {'b': 'foo, bar, baz'}}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "    'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'properties',"
        "            'propertiesNotSatisfied': ["
        "                   {'propertyName': 'a', "
        "                    'details':"
        "                       [{'operatorName': 'properties',"
        "                         'propertiesNotSatisfied': ["
        "                            {'propertyName': 'b', "
        "                             'details': ["
        "                                      {'operatorName': 'maxLength',"
        "                                       'specifiedAs': {'maxLength': 4},"
        "                                       'reason': 'specified string length was not "
        "satisfied',"
        "                                       'consideredValue': 'foo, bar, baz'}]}]}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MaxLengthAtTopLevelHasNoEffect) {
    BSONObj query = fromjson("{'$nor': [{'$jsonSchema': {'maxLength': 1000}}]}");
    BSONObj document = fromjson("{a: 'foo'}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$nor',"
        "     'clausesSatisfied': [{'index': 0, 'details': "
        "       {'operatorName': '$jsonSchema', 'reason': 'schema matched'}}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MaxLengthInvertedLengthDoesMatch) {
    BSONObj query = fromjson("{'$jsonSchema': {'not': {'properties': {'a': {'maxLength': 30}}}}}");
    BSONObj document = fromjson("{'a': 'this string is long'}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'not', 'reason': 'child expression matched'}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MaxLengthInvertedTypeMismatch) {
    BSONObj query = fromjson("{'$jsonSchema': {'not': {'properties': {'a': {'maxLength': 4}}}}}");
    BSONObj document = fromjson("{'a': 1}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "       {'operatorName': 'not', 'reason': 'child expression matched'}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MaxLengthInvertedMissingProperty) {
    BSONObj query = fromjson("{'$jsonSchema': {'not': {'properties': {'a': {'maxLength': 4}}}}}");
    BSONObj document = fromjson("{'b': 1}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "       {'operatorName': 'not', 'reason': 'child expression matched'}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// pattern
TEST(JSONSchemaValidation, BasicPattern) {
    BSONObj query =
        fromjson("{'$jsonSchema': {'properties': {'a': {'type': 'string','pattern': '^S'}}}}");
    BSONObj document = fromjson("{'a': 'slow'}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'pattern',"
        "            'specifiedAs': {'pattern': '^S'},"
        "            'reason': 'regular expression did not match',"
        "            'consideredValue': 'slow'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, PatternNoExplicitType) {
    BSONObj query = fromjson("{'$jsonSchema': {'properties': {'a': {'pattern': '^S'}}}}");
    BSONObj document = fromjson("{'a': 'slow'}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'pattern',"
        "            'specifiedAs': {'pattern': '^S'},"
        "            'reason': 'regular expression did not match',"
        "            'consideredValue': 'slow'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, PatternNonStringWithType) {
    BSONObj query =
        fromjson("{'$jsonSchema': {'properties': {'a': {'type': 'string','pattern': '^S'}}}}");
    BSONObj document = fromjson("{'a': 1}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "   {operatorName: 'properties', 'propertiesNotSatisfied': ["
        "       {propertyName: 'a', 'details': ["
        "           {'operatorName': 'type', "
        "           'specifiedAs': { 'type': 'string' }, "
        "           'reason': 'type did not match', "
        "           'consideredValue': 1, "
        "           'consideredType': 'int'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, PatternWithRequiredNoTypeMissingProperty) {
    BSONObj query =
        fromjson("{'$jsonSchema': {'required': ['a'], 'properties': {'a': {'pattern': '^S'}}}}");
    BSONObj document = fromjson("{}");
    // Should not mention 'pattern'.
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'required',"
        "   specifiedAs: {required: ['a']}, "
        "   missingProperties: ['a']}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, PatternWithRequiredAndTypeMissingProperty) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'required': ['a'],"
        "   'properties': {'a': {'type': 'string', 'pattern': '^S'}}}}");
    BSONObj document = fromjson("{}");
    // Should not mention 'pattern', nor 'type'.
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'required',"
        "   specifiedAs: {required: ['a']}, "
        "   missingProperties: ['a']}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, PatternWithRequiredAndTypePatternFails) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'required': ['a'],"
        "   'properties': {'a': {'type': 'string', 'pattern': '^S'}}}}");
    BSONObj document = fromjson("{a: 'floo'}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'pattern',"
        "            'specifiedAs': {'pattern': '^S'},"
        "            'reason': 'regular expression did not match',"
        "            'consideredValue': 'floo'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, PatternNested) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {"
        "   'properties': {"
        "       'a': {'properties': "
        "           {'b': {'type': 'string', 'pattern': '^S',"
        "                  title: 'pattern property',"
        "                  description: 'values of a should start with S'}}}}}}");
    BSONObj document = fromjson("{'a': {'b': 'foo'}}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "    'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'properties',"
        "            'propertiesNotSatisfied': ["
        "                   {'propertyName': 'a', "
        "                    'details':"
        "                       [{'operatorName': 'properties',"
        "                         'propertiesNotSatisfied': ["
        "                            {'propertyName': 'b', "
        "                             'title': 'pattern property',"
        "                             'description': 'values of a should start with S',"
        "                             'details': ["
        "                                      {'operatorName': 'pattern',"
        "                                       'specifiedAs': {'pattern': '^S'},"
        "                                       'reason': 'regular expression did not match',"
        "                                       'consideredValue': 'foo'}]}]}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, PatternAtTopLevelHasNoEffect) {
    BSONObj query = fromjson("{'$nor': [{'$jsonSchema': {'pattern': '^S'}}]}");
    BSONObj document = fromjson("{a: 'Slow'}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$nor',"
        "     'clausesSatisfied': [{'index': 0, 'details': "
        "       {'operatorName': '$jsonSchema', 'reason': 'schema matched'}}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, PatternInvertedRegexDoesMatch) {
    BSONObj query = fromjson("{'$jsonSchema': {'not': {'properties': {'a': {'pattern': '^S'}}}}}");
    BSONObj document = fromjson("{'a': 'String'}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'not', 'reason': 'child expression matched'}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, PatternInvertedTypeMismatch) {
    BSONObj query = fromjson("{'$jsonSchema': {'not': {'properties': {'a': {'pattern': '^S'}}}}}");
    BSONObj document = fromjson("{'a': 1}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "       {'operatorName': 'not', 'reason': 'child expression matched'}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, PatternInvertedMissingProperty) {
    BSONObj query = fromjson("{'$jsonSchema': {'not': {'properties': {'a': {'pattern': '^S'}}}}}");
    BSONObj document = fromjson("{'b': 1}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "       {'operatorName': 'not', 'reason': 'child expression matched'}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// multipleOf
TEST(JSONSchemaValidation, BasicMultipleOf) {
    BSONObj query =
        fromjson("{'$jsonSchema':{properties: {a: {type: 'number', multipleOf: 2.1}}}}");
    BSONObj document = fromjson("{'a': 1.1}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "     'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'properties',"
        "            'propertiesNotSatisfied': ["
        "                   {'propertyName': 'a',  details: ["
        "                       {'operatorName': 'multipleOf',"
        "                       'specifiedAs': {'multipleOf': 2.1},"
        "                       'reason': 'considered value is not a multiple of the specified "
        "value',"
        "                       'consideredValue': 1.1}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MultipleOfNoExplicitType) {
    BSONObj query = fromjson("{'$jsonSchema':{properties: {a: {multipleOf: 2.1}}}}");
    BSONObj document = fromjson("{'a': 1.1}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "     'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'properties',"
        "            'propertiesNotSatisfied': ["
        "                   {'propertyName': 'a',  'details': ["
        "                       {'operatorName': 'multipleOf',"
        "                       'specifiedAs': {'multipleOf': 2.1},"
        "                       'reason': 'considered value is not a multiple of the specified "
        "value',"
        "                       'consideredValue': 1.1}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MultipleOfNonNumeric) {
    BSONObj query =
        fromjson("{'$jsonSchema':{properties: {a: {type: 'number', 'multipleOf': 2.1}}}}");
    BSONObj document = fromjson("{'a': 'foo'}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'type', "
        "           'specifiedAs': { 'type': 'number' }, "
        "           'reason': 'type did not match', "
        "           'consideredValue': 'foo', "
        "           'consideredType': 'string' }]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MultipleOfWithRequiredNoTypeMissingProperty) {
    BSONObj query =
        fromjson("{'$jsonSchema': {'required': ['a'], 'properties': {'a': {'multipleOf': 2}}}}");
    BSONObj document = fromjson("{}");
    // Should not mention 'pattern'.
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'required',"
        "   specifiedAs: {required: ['a']}, "
        "   missingProperties: ['a']}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MultipleOfWithRequiredAndTypeMissingProperty) {
    BSONObj query =
        fromjson("{'$jsonSchema': {'required': ['a'], 'properties': {'a': {'multipleOf': 2}}}}");
    BSONObj document = fromjson("{}");
    // Should not mention 'pattern', nor 'type'.
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'required',"
        "   specifiedAs: {required: ['a']}, "
        "   missingProperties: ['a']}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MultipleOfWithRequiredAndTypeMultipleOfFails) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'required': ['a'],"
        "   'properties': {'a': {'type': 'number', 'multipleOf': 2}}}}");
    BSONObj document = fromjson("{a: 3}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'multipleOf',"
        "            'specifiedAs': {'multipleOf': 2},"
        "            'reason': 'considered value is not a multiple of the specified value',"
        "            'consideredValue': 3}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MultipleOfNested) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {"
        "   'properties': {"
        "       'a': {'properties':  {'b': {'multipleOf': 2.1}}}}}}");
    BSONObj document = fromjson("{'a': {'b': 1}}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "       'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'properties',"
        "            'propertiesNotSatisfied': ["
        "                   {'propertyName': 'a', "
        "                    'details':"
        "                       [{'operatorName': 'properties',"
        "                         'propertiesNotSatisfied': ["
        "                            {'propertyName': 'b', "
        "                             'details': ["
        "                                      {'operatorName': 'multipleOf',"
        "                                       'specifiedAs': {'multipleOf': 2.1},"
        "                             'reason': 'considered value is not a multiple of the "
        "specified value',"
        "                                       'consideredValue': 1}]}]}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MultipleOfAtTopLevelHasNoEffect) {
    BSONObj query = fromjson("{'$nor': [{'$jsonSchema': {'multipleOf': 1}}]}");
    BSONObj document = fromjson("{a: 'foo'}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$nor',"
        "     'clausesSatisfied': [{'index': 0, 'details': "
        "       {'operatorName': '$jsonSchema', 'reason': 'schema matched'}}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MultipleOfInvertedMultipleDoesMatch) {
    BSONObj query = fromjson("{'$jsonSchema': {'not': {'properties': {'a': {'multipleOf': 1}}}}}");
    BSONObj document = fromjson("{'a': 2}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'not', 'reason': 'child expression matched'}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MultipleOfInvertedTypeMismatch) {
    BSONObj query = fromjson("{'$jsonSchema': {'not': {'properties': {'a': {'multipleOf': 1}}}}}");
    BSONObj document = fromjson("{'a': 'not a number!'}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'not', 'reason': 'child expression matched'}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MultipleOfInvertedMissingProperty) {
    BSONObj query = fromjson("{'$jsonSchema': {'not': {'properties': {'a': {'multipleOf': 1}}}}}");
    BSONObj document = fromjson("{'b': 1}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "       {'operatorName': 'not', 'reason': 'child expression matched'}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// encrypt
TEST(JSONSchemaValidation, BasicEncrypt) {
    BSONObj query =
        fromjson("{'$jsonSchema': {bsonType: 'object', properties: {a: {encrypt: {}}}}}");
    BSONObj document = BSON("a" << BSONBinData("abc", 3, BinDataType::BinDataGeneral));
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "     'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'properties',"
        "            'propertiesNotSatisfied': ["
        "                   {'propertyName': 'a',  'details': ["
        "                       {'operatorName': 'encrypt',"
        "                       'reason': 'value was not encrypted'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, EncryptInvertedValueIsEncrypted) {
    BSONObj query = fromjson(
        "{$nor: [{'$jsonSchema': {bsonType: 'object', properties: {a: {encrypt: "
        "{}}}}}]}");
    BSONObj document = BSON("a" << BSONBinData("abc", 3, BinDataType::Encrypt));
    BSONObj expectedError = fromjson(
        "{'operatorName': '$nor','clausesSatisfied': ["
        "   {'index': 0, 'details':{'operatorName': '$jsonSchema', 'reason': 'schema matched'}}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, EncryptMissingProperty) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {not: {bsonType: 'object', properties: {a: {encrypt: "
        "{}}}}}}");
    BSONObj document = BSON("b" << BSONBinData("abc", 3, BinDataType::Encrypt));
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "       {'operatorName': 'not', 'reason': 'child expression matched'}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, EncryptWithSubtypeFailsBecauseNotEncrypted) {
    BSONObj query = fromjson(
        "{'$jsonSchema':"
        "{bsonType: 'object', properties: {a: {encrypt: {bsonType: 'number'}}}}}");
    BSONObj document = BSON("a" << "foo");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "     'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'properties',"
        "            'propertiesNotSatisfied': ["
        "                   {'propertyName': 'a',  'details': ["
        "                       {'operatorName': 'encrypt',"
        "                       'reason': 'value was not encrypted'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, EncryptWithSubtypeFailsDueToMismatchedSubtype) {
    BSONObj query = fromjson(
        "{'$jsonSchema':"
        "{bsonType: 'object', properties: {a: {encrypt: {bsonType: 'number'}}}}}");
    FleBlobHeader blob;
    blob.fleBlobSubtype = static_cast<int8_t>(EncryptedBinDataType::kDeterministic);
    memset(blob.keyUUID, 0, sizeof(blob.keyUUID));
    blob.originalBsonType = stdx::to_underlying(BSONType::string);

    BSONObj document = BSON("a" << BSONBinData(reinterpret_cast<const void*>(&blob),
                                               sizeof(FleBlobHeader),
                                               BinDataType::Encrypt));

    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "     'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'properties',"
        "            'propertiesNotSatisfied': ["
        "                   {'propertyName': 'a',  'details': ["
        "                       {'operatorName': 'encrypt',"
        "                       'reason': 'encrypted value has wrong type'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, EncryptWithSubtypeInvertedValueIsEncrypted) {
    BSONObj query = fromjson(
        "{$nor: [{'$jsonSchema': {bsonType: 'object', properties: "
        "  {a: {encrypt: {bsonType: 'number'}}}}}]}");
    FleBlobHeader blob;
    blob.fleBlobSubtype = static_cast<int8_t>(EncryptedBinDataType::kDeterministic);
    memset(blob.keyUUID, 0, sizeof(blob.keyUUID));
    blob.originalBsonType = stdx::to_underlying(BSONType::numberInt);

    BSONObj document = BSON("a" << BSONBinData(reinterpret_cast<const void*>(&blob),
                                               sizeof(FleBlobHeader),
                                               BinDataType::Encrypt));
    BSONObj expectedError = fromjson(
        "{'operatorName': '$nor','clausesSatisfied': ["
        "   {'index': 0, 'details':{'operatorName': '$jsonSchema', 'reason': 'schema matched'}}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, EncryptWithSubtypeInvertedMissingProperty) {
    BSONObj query =
        fromjson("{$nor: [{'$jsonSchema': {bsonType: 'object', properties: {a: {encrypt:{}}}}}]}");
    BSONObj document = BSON("b" << BSONBinData("abc", 3, BinDataType::Encrypt));
    BSONObj expectedError = fromjson(
        "{'operatorName': '$nor','clausesSatisfied': ["
        "   {'index': 0, 'details':{'operatorName': '$jsonSchema', 'reason': 'schema matched'}}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// Logical keywords

// allOf
TEST(JSONSchemaLogicalKeywordValidation, TopLevelAllOf) {
    BSONObj query = fromjson(
        "{$jsonSchema: "
        "{allOf: [{properties: {a: {minimum: 1}}},{properties:{b:{minimum:2}}}]}}");
    BSONObj document = fromjson("{a: 1, b: 0}");
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', "
        "   schemaRulesNotSatisfied: ["
        "       {operatorName: 'allOf', "
        "       schemasNotSatisfied: [ "
        "           {index: 1, details: ["
        "                   {operatorName: 'properties', propertiesNotSatisfied: ["
        "                       {'propertyName': 'b', details: ["
        "                           {'operatorName': 'minimum', specifiedAs: {minimum: 2},"
        "                             reason: 'comparison failed', consideredValue: 0}]}]}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaLogicalKeywordValidation, NestedAllOf) {
    BSONObj query =
        fromjson("{$jsonSchema: {properties: {a: {allOf: [{minimum: 1},{maximum: 3}]}}}}");
    BSONObj document = fromjson("{a: 4}");
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', "
        "   'schemaRulesNotSatisfied': ["
        "       {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "           {'propertyName': 'a', 'details': ["
        "               {'operatorName': 'allOf',"
        "                'schemasNotSatisfied': ["
        "                   {'index': 1, 'details': ["
        "                           {'operatorName': 'maximum', "
        "                           'specifiedAs': {'maximum': 3},"
        "                           'reason': 'comparison failed',"
        "                           'consideredValue': 4}]}]}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaLogicalKeywordValidation, NotOverAllOf) {
    BSONObj query =
        fromjson("{$jsonSchema: {properties: {a: {not: {allOf: [{minimum: 1},{maximum: 3}]}}}}}");
    BSONObj document = fromjson("{a: 2}");
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'properties', propertiesNotSatisfied: ["
        "       {propertyName: 'a', 'details': ["
        "           {operatorName: 'not', 'reason': 'child expression matched'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// anyOf
TEST(JSONSchemaLogicalKeywordValidation, TopLevelAnyOf) {
    BSONObj query = fromjson(
        "{$jsonSchema: {anyOf: [{properties: {a: {minimum: 1}}}, "
        "{properties:{b:{minimum: 1}}}]}}");
    BSONObj document = fromjson("{a: 0, b: 0}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {'operatorName': 'anyOf', schemasNotSatisfied: ["
        "       {index: 0, details:  [ "
        "               {'operatorName': 'properties', propertiesNotSatisfied: ["
        "                   {propertyName: 'a', 'details': [ "
        "                       {'operatorName': 'minimum', "
        "                       'specifiedAs': {'minimum': 1 }, "
        "                       'reason': 'comparison failed', "
        "                       'consideredValue': 0}]}]}]}, "
        "        {index: 1, details: [ "
        "           {'operatorName': 'properties', propertiesNotSatisfied: ["
        "               {propertyName: 'b', 'details': [ "
        "                   {'operatorName': 'minimum', "
        "                   'specifiedAs': {minimum: 1 }, "
        "                   'reason': 'comparison failed', consideredValue: 0}]}]}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaLogicalKeywordValidation, NestedAnyOfWithDescription) {
    BSONObj query = fromjson(
        "{$jsonSchema: {properties: {a: { description: 'property a',"
        "anyOf: [{bsonType: 'number', description: 'number?'}, {bsonType: 'string'}]}}}}");
    BSONObj document = fromjson("{a: {}}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {'operatorName': 'properties', propertiesNotSatisfied: ["
        "       {propertyName: 'a', description: 'property a', details: [ "
        "               {'operatorName': 'anyOf', schemasNotSatisfied: ["
        "                   {index: 0, description: 'number?', details: [{"
        "                       operatorName: 'bsonType',"
        "                       specifiedAs: { bsonType: 'number' },"
        "                       reason: 'type did not match',"
        "                       consideredValue: {},"
        "                       consideredType: 'object' }]},"
        "                   {index: 1, details: [{"
        "                       operatorName: 'bsonType',"
        "                       specifiedAs: { bsonType: 'string' },"
        "                       reason: 'type did not match',"
        "                       consideredValue: {},"
        "                       consideredType: 'object' }]}]}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaLogicalKeywordValidation, NestedAnyOf) {
    BSONObj query =
        fromjson("{$jsonSchema: {properties: {a: {anyOf: [{type: 'string'},{maximum: 3}]}}}}");
    BSONObj document = fromjson("{a: 4}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', schemaRulesNotSatisfied: ["
        "       {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "           {'propertyName': 'a', 'details': ["
        "               {'operatorName': 'anyOf', 'schemasNotSatisfied': ["
        "                   {'index': 0, 'details': ["
        "                       {'operatorName': 'type', "
        "                       'specifiedAs': {'type': 'string' }, "
        "                       'reason': 'type did not match', "
        "                       'consideredValue': 4, "
        "                       consideredType: 'int' }]},"
        "                   {'index': 1, 'details': ["
        "                           {'operatorName': 'maximum',"
        "                           'specifiedAs': {maximum: 3}, "
        "                           'reason': 'comparison failed', "
        "                           'consideredValue': 4 }]}]}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// Combine anyOf and allOf
TEST(JSONSchemaLogicalKeywordValidation, TopLevelAnyOfAllOf) {
    BSONObj query = fromjson(
        "{$jsonSchema: {anyOf: ["
        "   {allOf: [{properties: "
        "       {d: {type: 'string', 'minLength': 3},"
        "       e: {maxLength: 10}}}]},"
        "   {allOf: [{properties: "
        "       {a: {type: 'number', 'maximum': 3}, "
        "       b: {type: 'number', 'minimum': 3},"
        "       c: {type: 'number', 'enum': [1,2,3]}}}]}]}}");
    BSONObj document =
        fromjson("{a: 0, b: 5, c: 0, d: 'foobar', e: 'a string that is over ten characters'}");
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema',"
        "   'schemaRulesNotSatisfied': [{operatorName: 'anyOf', 'schemasNotSatisfied': ["
        "   {index: 0, details: ["
        "       {operatorName: 'allOf', schemasNotSatisfied:[{'index': 0, 'details': "
        "           [{operatorName: 'properties', propertiesNotSatisfied: ["
        "               {propertyName: 'e', details: ["
        "                   {operatorName: 'maxLength',"
        "                   specifiedAs: {maxLength: 10}, "
        "                   reason: 'specified string length was not satisfied',"
        "                   consideredValue: 'a string that is over ten "
        "characters'}]}]}]}]}]}, "
        " {index: 1,details: ["
        "       {operatorName: 'allOf', schemasNotSatisfied:[{'index': 0, 'details': ["
        "           {operatorName: 'properties', propertiesNotSatisfied: ["
        "               {propertyName: 'c', details: ["
        "                   {operatorName: 'enum',"
        "                   specifiedAs: {enum: [1,2,3]}, "
        "                   reason: 'value was not found in enum',"
        "                   consideredValue: 0}]}]}]}]}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaLogicalKeywordValidation, TopLevelAllOfAnyOf) {
    BSONObj query = fromjson(
        "{$jsonSchema: {allOf: ["
        "   {anyOf: [{properties: "
        "       {d: {type: 'string', 'minLength': 3},"
        "       e: {maxLength: 10}}}]},"
        "   {anyOf: [{properties: "
        "       {a: {type: 'number', 'maximum': 3}, "
        "       b: {type: 'number', 'minimum': 3},"
        "       c: {type: 'number', 'enum': [1,2,3]}}}]}]}}");
    BSONObj document = fromjson("{a: 10, b: 0, c: 0, d: 'foobar', e: 1}");
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema',"
        "   'schemaRulesNotSatisfied': [{operatorName: 'allOf', 'schemasNotSatisfied': ["
        " {index: 1,details: ["
        "       {operatorName: 'anyOf', schemasNotSatisfied:[{'index': 0, 'details': ["
        "           {operatorName: 'properties', propertiesNotSatisfied: ["
        "               {propertyName: 'a', details: ["
        "                   {operatorName: 'maximum',"
        "                   specifiedAs: {maximum: 3}, "
        "                   reason: 'comparison failed',"
        "                   consideredValue: 10}]},"
        "               {propertyName: 'b', details: ["
        "                   {operatorName: 'minimum',"
        "                   specifiedAs: {minimum: 3}, "
        "                   reason: 'comparison failed',"
        "                   consideredValue: 0}]},"
        "               {propertyName: 'c', details: ["
        "                   {operatorName: 'enum',"
        "                   specifiedAs: {enum: [1,2,3]}, "
        "                   reason: 'value was not found in enum',"
        "                   consideredValue: 0}]}"
        "]}]}]}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaLogicalKeywordValidation, NotOverAnyOf) {
    BSONObj query =
        fromjson("{$jsonSchema: {properties: {a: {not: {anyOf: [{minimum: 1},{maximum: 6}]}}}}}");
    BSONObj document = fromjson("{a: 4}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "       {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "           {'propertyName': 'a', 'details': ["
        "               {'operatorName': 'not', 'reason': 'child expression matched'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// oneOf
TEST(JSONSchemaLogicalKeywordValidation, OneOfMoreThanOneMatchingClause) {
    BSONObj query = fromjson(
        "{$jsonSchema: {properties: {a: {oneOf: [{minimum: 1},{maximum: 3}],"
        "description: 'oneOf description'}}}}");
    BSONObj document = fromjson("{a: 2}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',schemaRulesNotSatisfied: ["
        "       {'operatorName': 'properties', propertiesNotSatisfied: ["
        "           {propertyName: 'a', description: 'oneOf description',"
        "            'details': ["
        "               {'operatorName': 'oneOf', "
        "                'reason': 'more than one subschema matched', "
        "                'matchingSchemaIndexes': [0, 1]}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaLogicalKeywordValidation, NorOverOneOf) {
    BSONObj query = fromjson(
        "{$nor: [{$jsonSchema: {properties: {a: {oneOf: [{minimum: 1},{maximum: 3}]}}}}]}");
    BSONObj document = fromjson("{a: 4}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$nor','clausesSatisfied': ["
        "   {'index': 0, 'details':{'operatorName': '$jsonSchema', 'reason': 'schema matched'}}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaLogicalKeywordValidation, OneOfAllFailingClauses) {
    BSONObj query = fromjson(
        "{'$jsonSchema':"
        "{'properties':{'a':{'oneOf': [{'minimum': 4},{'maximum': 1},{'bsonType':'int'}]}}}}");
    BSONObj document = fromjson("{a: 2.1}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {'operatorName': 'properties', propertiesNotSatisfied: ["
        "       {propertyName: 'a', 'details': ["
        "           {'operatorName': 'oneOf', schemasNotSatisfied: ["
        "               {index: 0, details: ["
        "                       {'operatorName': 'minimum', "
        "                       'specifiedAs': {minimum: 4}, "
        "                       'reason': 'comparison failed', "
        "                       'consideredValue': 2.1 }]}, "
        "                {index: 1, details: ["
        "                       {'operatorName': 'maximum', "
        "                       'specifiedAs': {maximum: 1}, "
        "                       'reason': 'comparison failed', "
        "                       'consideredValue': 2.1 }]}, "
        "                 {index: 2, details: ["
        "                   {'operatorName': 'bsonType', "
        "                   'specifiedAs': {'bsonType': 'int' }, "
        "                   'reason': 'type did not match', "
        "                   'consideredValue': 2.1, "
        "                   'consideredType': 'double'}]}]}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaLogicalKeywordValidation, NotOverOneOf) {
    BSONObj query =
        fromjson("{$jsonSchema: {properties: {a: {not: {oneOf: [{minimum: 1},{maximum: 3}]}}}}}");
    BSONObj document = fromjson("{a: 4}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "       {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "           {'propertyName': 'a', 'details': ["
        "               {'operatorName': 'not', 'reason': 'child expression matched'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaLogicalKeywordValidation, NotOverOneOfOneClause) {
    BSONObj query = fromjson("{$jsonSchema: {properties: {a: {not: {oneOf: [{minimum: 1}]}}}}}");
    BSONObj document = fromjson("{a: 4}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "       {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "           {propertyName: 'a', 'details': ["
        "               {'operatorName': 'not', 'reason': 'child expression matched'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// not
TEST(JSONSchemaLogicalKeywordValidation, BasicNot) {
    BSONObj query = fromjson("{$jsonSchema: {not: {properties: {a: {minimum: 3}}}}}");
    BSONObj document = fromjson("{a: 6}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "               {'operatorName': 'not', 'reason': 'child expression matched'}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaLogicalKeywordValidation, BasicNotWithDescription) {
    BSONObj query = fromjson(
        "{$jsonSchema: { properties: { a: { not: { type: 'number', title: 'type title'},"
        "                                   title: 'not title' } } } }");
    BSONObj document = fromjson("{a: 1}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "       {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "           {propertyName: 'a',"
        "            title: 'not title',"
        "            details: ["
        "               {'operatorName': 'not', 'reason': 'child expression matched'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaLogicalKeywordValidation, NestedNot) {
    BSONObj query = fromjson("{$jsonSchema: {not: {not: {properties: {a: {minimum: 3}}}}}}");
    BSONObj document = fromjson("{a: 1}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "               {'operatorName': 'not', 'reason': 'child expression matched'}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaLogicalKeywordValidation, NotOverEmptySchema) {
    BSONObj query = fromjson("{$jsonSchema: {not: {}}}");
    BSONObj document = fromjson("{}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', "
        "   'schemaRulesNotSatisfied': [{'operatorName': 'not', 'reason': 'child expression "
        "matched'}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaLogicalKeywordValidation, NotFailsDueToType) {
    BSONObj query = fromjson("{$jsonSchema: {not: {properties: {a: {minimum: 3}}}}}");
    BSONObj document = fromjson("{a: 'foo'}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "       {'operatorName': 'not', 'reason': 'child expression matched'}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaLogicalKeywordValidation, NotFailsDueToExistence) {
    BSONObj query = fromjson("{$jsonSchema: {not: {properties: {a: {minimum: 3}}}}}");
    BSONObj document = fromjson("{b: 6}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "       {'operatorName': 'not', 'reason': 'child expression matched'}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaLogicalKeywordValidation, NotUnderProperties) {
    BSONObj query = fromjson("{$jsonSchema: {properties: {a: {not: {}}}}}");
    BSONObj document = fromjson("{a: 'foo'}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'not', "
        "            'reason': 'child expression matched'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// enum
TEST(JSONSchemaLogicalKeywordValidation, BasicEnum) {
    BSONObj query = fromjson("{'$jsonSchema': {'properties': {'a': {'enum': [1,2,3]}}}}");
    BSONObj document = fromjson("{a: 0}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {'operatorName': 'properties', propertiesNotSatisfied: ["
        "       {propertyName: 'a', 'details': ["
        "           {'operatorName': 'enum', "
        "           specifiedAs: {enum: [ 1, 2, 3 ]}, "
        "           reason: 'value was not found in enum', "
        "           consideredValue: 0 }]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaLogicalKeywordValidation, EnumWithRequiredMissingProperty) {
    BSONObj query =
        fromjson("{'$jsonSchema': {'required': ['a'], 'properties': {'a': {'enum':[1,2,3]}}}}");
    BSONObj document = fromjson("{}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "     'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'required',"
        "            'specifiedAs': {'required': ['a']},"
        "            'missingProperties': ['a']}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaLogicalKeywordValidation, TopLevelEnum) {
    BSONObj query =
        fromjson("{'$jsonSchema': {'enum': [{'a': 1, 'b': 1}, {'a': 0, 'b': {'c': [1,2,3]}}]}}");
    BSONObj document = fromjson("{'a': 0, b: 1, _id: 1}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', schemaRulesNotSatisfied: ["
        "       {'operatorName': 'enum', "
        "        specifiedAs: {enum: [{a: 1, b: 1 }, {a: 0, b: {c: [ 1, 2, 3 ]}}]}, "
        "        reason: 'value was not found in enum', "
        "       'consideredValue': {a: 0, b: 1, _id: 1}}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaLogicalKeywordValidation, EnumTypeArrayWithEmptyArray) {
    BSONObj query =
        fromjson("{'$jsonSchema': {'properties': {'a': {'type': 'array', 'enum': [[1]]}}}}");
    BSONObj document = fromjson("{'a': []}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "    {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "        {'propertyName': 'a', 'details': ["
        "            {'operatorName': 'enum', 'specifiedAs': { 'enum': [[1]]}, 'reason': "
        "'value was not found in enum', consideredValue: []}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaLogicalKeywordValidation, NotOverEnum) {
    BSONObj query = fromjson("{'$jsonSchema': {'properties': {'a': {'not': {'enum': [1,2,3]}}}}}");
    BSONObj document = fromjson("{a: 1}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "       {'operatorName': 'not', 'reason': 'child expression matched'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// Combine logical keywords
TEST(JSONSchemaLogicalKeywordValidation, CombineLogicalKeywords) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'properties': {'a': "
        "   {'allOf': [{'bsonType': 'int'},"
        "               {'oneOf': [{minimum: 1}, {enum: [6,7,8]}]}]}}}}");
    BSONObj document = fromjson("{a: 0}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'allOf', 'schemasNotSatisfied': ["
        "               {'index': 1, 'details':  [{'operatorName': 'oneOf', "
        "                   'schemasNotSatisfied': ["
        "                       {'index': 0, 'details':["
        "                                {'operatorName': 'minimum',"
        "                                   'specifiedAs': {'minimum': 1 },"
        "                                   'reason': 'comparison failed', "
        "                                   'consideredValue': 0}]},"
        "                        {'index': 1, 'details': ["
        "                               {'operatorName': 'enum', "
        "                               'specifiedAs': {'enum': [ 6, 7, 8 ]}, "
        "                               'reason': 'value was not found in enum', "
        "                                   'consideredValue': 0 }]}]}]}]}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// Array keywords.
TEST(JSONSchemaValidation, ArrayType) {
    BSONObj query = fromjson(
        "  {'$jsonSchema':"
        "   {'properties': "
        "       {'a': {'type': 'array'}}}}");
    BSONObj document = fromjson("{'a': {'b': 1}}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'type',"
        "            'specifiedAs': {'type': 'array'},"
        "            'reason': 'type did not match',"
        "            'consideredValue': {'b': 1},"
        "            'consideredType': 'object'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayMinItemsTypeArray) {
    BSONObj query = fromjson(
        "  {'$jsonSchema':"
        "   {'properties': "
        "       {'a': {'type': 'array', 'minItems': 2}}}}");
    BSONObj document = fromjson("{'a': [1]}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'minItems',"
        "            'specifiedAs': {'minItems': 2},"
        "            'reason': 'array did not match specified length',"
        "            'consideredValue': [1],"
        "            'numberOfItems': 1}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayMinItemsTypeArrayRequiredMissingProperty) {
    BSONObj query = fromjson(
        "  {'$jsonSchema':"
        "   {'required': ['a'],"
        "   'properties': "
        "       {'a': {'type': 'array', 'minItems': 2}}}}");
    BSONObj document = fromjson("{}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "     'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'required',"
        "            'specifiedAs': {'required': ['a']},"
        "            'missingProperties': ['a']}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayMinItems) {
    BSONObj query = fromjson(
        "  {'$jsonSchema':"
        "   {'properties': "
        "       {'a': {'minItems': 2}}}}");
    BSONObj document = fromjson("{'a': [1]}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'minItems',"
        "            'specifiedAs': {'minItems': 2},"
        "            'reason': 'array did not match specified length',"
        "            'consideredValue': [1],"
        "            'numberOfItems': 1}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayMinItemsRequiredMissingProperty) {
    BSONObj query = fromjson(
        "  {'$jsonSchema':"
        "   {'required': ['a'],"
        "   'properties': "
        "       {'a': {'minItems': 2}}}}");
    BSONObj document = fromjson("{}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "     'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'required',"
        "            'specifiedAs': {'required': ['a']},"
        "            'missingProperties': ['a']}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayMinItemsAlwaysTrue) {
    BSONObj query = fromjson("{$nor: [{'$jsonSchema': {'minItems': 2}}]}");
    BSONObj document = fromjson("{}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$nor', 'clausesSatisfied': ["
        "  {'index': 0, 'details': "
        "    {'operatorName': '$jsonSchema', 'reason': 'schema matched'}}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayMinItemsTypeArrayOnNonArrayAttribute) {
    BSONObj query = fromjson(
        "  {'$jsonSchema':"
        "   {'properties': "
        "       {'a': {'type': 'array', 'minItems': 2}}}}");
    BSONObj document = fromjson("{'a': 1}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'type',"
        "            'specifiedAs': {'type': 'array'},"
        "            'reason': 'type did not match',"
        "            'consideredValue': 1,"
        "            'consideredType': 'int'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayMaxItems) {
    BSONObj query = fromjson(
        "  {'$jsonSchema':"
        "   {'properties': "
        "       {'a': {'maxItems': 2}}}}");
    BSONObj document = fromjson("{'a': [1, 2, 3]}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'maxItems',"
        "            'specifiedAs': {'maxItems': 2},"
        "            'reason': 'array did not match specified length',"
        "            'consideredValue': [1, 2, 3],"
        "            'numberOfItems': 3}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayMaxItemsRequiredMissingProperty) {
    BSONObj query = fromjson(
        "  {'$jsonSchema':"
        "   {'required': ['a'],"
        "   'properties': {'a': {'maxItems': 2}}}}");
    BSONObj document = fromjson("{}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "     'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'required',"
        "            'specifiedAs': {'required': ['a']},"
        "            'missingProperties': ['a']}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayMaxItemsAlwaysTrue) {
    BSONObj query = fromjson("{$nor: [{'$jsonSchema': {'maxItems': 2}}]}");
    BSONObj document = fromjson("{}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$nor', 'clausesSatisfied': ["
        "  {'index': 0, 'details': "
        "    {'operatorName': '$jsonSchema', 'reason': 'schema matched'}}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayUniqueItems) {
    BSONObj query = fromjson(
        "  {'$jsonSchema':"
        "   {'properties': "
        "       {'a': {'uniqueItems': true}}}}");
    BSONObj document = fromjson("{'a': [1, 2, 3, 3, 4, 4]}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'uniqueItems',"
        "            'specifiedAs': {'uniqueItems': true},"
        "            'reason': 'found a duplicate item',"
        "            'consideredValue': [1, 2, 3, 3, 4, 4],"
        "            'duplicatedValue': 3}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayUniqueItemsRequiredMissingProperty) {
    BSONObj query = fromjson(
        "  {'$jsonSchema':"
        "   {'required': ['a'], 'properties': "
        "       {'a': {'uniqueItems': true}}}}");
    BSONObj document = fromjson("{}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "     'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'required',"
        "            'specifiedAs': {'required': ['a']},"
        "            'missingProperties': ['a']}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayUniqueItemTypeArray) {
    BSONObj query = fromjson(
        "  {'$jsonSchema':"
        "   {'properties': "
        "       {'a': {'type': 'array', 'uniqueItems': true}}}}");
    BSONObj document = fromjson("{'a': [1, 2, 3, 3, 4, 4]}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'uniqueItems',"
        "            'specifiedAs': {'uniqueItems': true},"
        "            'reason': 'found a duplicate item',"
        "            'consideredValue': [1, 2, 3, 3, 4, 4],"
        "            'duplicatedValue': 3}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayUniqueItemTypeArrayRequiredMissingProperty) {
    BSONObj query = fromjson(
        "  {'$jsonSchema':"
        "   {'required': ['a'],"
        "   'properties': {'a': {'type': 'array', 'uniqueItems': true}}}}");
    BSONObj document = fromjson("{}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "     'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'required',"
        "            'specifiedAs': {'required': ['a']},"
        "            'missingProperties': ['a']}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayUniqueItemsTypeArrayOnNonArrayAttribute) {
    BSONObj query = fromjson(
        "  {'$jsonSchema':"
        "   {'properties': "
        "       {'a': {'type': 'array', 'uniqueItems': true}}}}");
    BSONObj document = fromjson("{'a': 1}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'type',"
        "            'specifiedAs': {'type': 'array'},"
        "            'reason': 'type did not match',"
        "            'consideredValue': 1,"
        "            'consideredType': 'int'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayUniqueItemsAlwaysTrue) {
    BSONObj query = fromjson("{$nor: [{'$jsonSchema': {'uniqueItems': true}}]}");
    BSONObj document = fromjson("{}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$nor', 'clausesSatisfied': ["
        "  {'index': 0, 'details': "
        "    {'operatorName': '$jsonSchema', 'reason': 'schema matched'}}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayItemsSingleSchema) {
    BSONObj query = fromjson(
        "  {'$jsonSchema':"
        "   {'properties': "
        "       {'a': {'items': {'type': 'string', 'description': 'elements must be of string "
        "type'}}}}}");
    BSONObj document = fromjson("{'a': [1, 'A', {}]}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "  {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "    {'propertyName': 'a', 'details': ["
        "      {'operatorName': 'items',"
        "       'description': 'elements must be of string type',"
        "       'reason': 'At least one item did not match the sub-schema',"
        "       'itemIndex': 0, 'details': ["
        "          {'operatorName': 'type', 'specifiedAs': {'type': 'string'}, "
        "'reason': 'type did not match', 'consideredValue': 1, 'consideredType':'int'}]}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayItemsSingleSchemaRequiredMissingProperty) {
    BSONObj query = fromjson(
        "  {'$jsonSchema':"
        "   {'required': ['a'], 'properties': "
        "       {'a': {'items': {'type': 'string'}}}}}");
    BSONObj document = fromjson("{}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "     'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'required',"
        "            'specifiedAs': {'required': ['a']},"
        "            'missingProperties': ['a']}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayItemsSingleSchemaTypeArrayOnNonArrayAttribute) {
    BSONObj query = fromjson(
        "  {'$jsonSchema':"
        "   {'properties': "
        "       {'a': {'items': {'type': 'string'}, 'type': 'array'}}}}");
    BSONObj document = fromjson("{'a': 1}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'type',"
        "            'specifiedAs': {'type': 'array'},"
        "            'reason': 'type did not match',"
        "            'consideredValue': 1,"
        "            'consideredType': 'int'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayItemsSingleSchemaTypeArrayRequiredMissingProperty) {
    BSONObj query = fromjson(
        "  {'$jsonSchema':"
        "   {'required': ['a'], 'properties': "
        "       {'a': {'items': {'type': 'string'}, 'type': 'array'}}}}");
    BSONObj document = fromjson("{}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "     'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'required',"
        "            'specifiedAs': {'required': ['a']},"
        "            'missingProperties': ['a']}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// Verifies that "items" with a single schema does not produce any unwanted artifacts when it does
// not fail. We use "minItems" that fails validation to check that.
TEST(JSONSchemaValidation, ArrayItemsSingleSchemaCombinedWithMinItems) {
    BSONObj query = fromjson(
        "  {'$jsonSchema':"
        "   {'properties': "
        "       {'a': {'items': {'type': 'string'}, 'minItems': 5}}}}");
    BSONObj document = fromjson("{'a': ['A', 'B']}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'minItems',"
        "            'specifiedAs': {'minItems': 5},"
        "            'reason': 'array did not match specified length',"
        "            'consideredValue': ['A', 'B'],"
        "            'numberOfItems': 2}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayItemsSingleSchemaAlwaysTrue) {
    BSONObj query = fromjson("{$nor: [{'$jsonSchema': {'items': {'type': 'string'}}}]}");
    BSONObj document = fromjson("{}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$nor', 'clausesSatisfied': ["
        "  {'index': 0, 'details': "
        "    {'operatorName': '$jsonSchema', 'reason': 'schema matched'}}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayItemsSingleSchemaNested) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'properties': "
        "  {'a': {'items': {'properties': {'b': {'minItems': 2}}}}}}}");
    BSONObj document = fromjson("{'a': [{'b': [1]}]}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "  {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "    {'propertyName': 'a', 'details': ["
        "      {'operatorName': 'items', 'reason': 'At least one item did not match the "
        "sub-schema', 'itemIndex': 0, 'details': ["
        "        {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "          {'propertyName': 'b', 'details': [ "
        "            {'operatorName': 'minItems',"
        "             'specifiedAs': { 'minItems': 2 },"
        "             'reason': 'array did not match specified length', "
        "             'consideredValue': [1],"
        "             'numberOfItems': 1}]}]}]}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayItemsSingleSchemaNestedRequiredMissingProperty) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'properties': "
        "  {'a': {'items': {'required': ['b'], 'properties': {'b': {'minItems': 2}}}}}}}");
    BSONObj document = fromjson("{'a': [{}]}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "  {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "    {'propertyName': 'a', 'details': ["
        "      {'operatorName': 'items',"
        "       'reason': 'At least one item did not match the sub-schema',"
        "       'itemIndex': 0, "
        "       'details':["
        "       {'operatorName': 'required',"
        "        'specifiedAs': {'required': ['b']},'missingProperties': ['b']}]}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayItemsSingleSchema2DArray) {
    BSONObj query =
        fromjson("{'$jsonSchema': {'properties': {'a': {'items': {'items': {'minimum': 0}}}}}}");
    BSONObj document = fromjson("{'a': [[1],[],[2, 4], [-1]]}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "  {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "    {'propertyName': 'a', 'details': ["
        "      {'operatorName': 'items', 'reason': 'At least one item did not match the "
        "sub-schema', 'itemIndex': 3, 'details': ["
        "        {'operatorName': 'items', 'reason': 'At least one item did not match the "
        "sub-schema', 'itemIndex': 0, 'details': ["
        "          {'operatorName': 'minimum', 'specifiedAs': {'minimum': 0}, 'reason': "
        "'comparison failed', 'consideredValue': -1}]}]}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayItemsSingleSchemaNestedWithMinimum) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'properties': "
        "  {'a': {'items': {'properties': {'b': {'minimum': 2}}}}}}}");
    BSONObj document = fromjson("{'a': [{'b': 2}, {'b': 1}]}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "{'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "  {'propertyName': 'a', 'details': ["
        "    {'operatorName': 'items', 'reason': 'At least one item did not match the sub-schema', "
        "'itemIndex': 1, 'details': ["
        "      {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "        {'propertyName': 'b', 'details': ["
        "          {'operatorName': 'minimum', 'specifiedAs': {'minimum': 2}, 'reason': "
        "'comparison failed', 'consideredValue': 1}]}]}]}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayItemsSchemaArray) {
    BSONObj query = fromjson(
        "  {'$jsonSchema':"
        "   {'properties': "
        "       {'a': {'items': [{'type': 'number'}, {'type': 'string'}]}}}}");
    BSONObj document = fromjson("{'a': [1, 2]}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "  {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "    {'propertyName': 'a', 'details': ["
        "      {'operatorName': 'items', 'details': ["
        "        {'itemIndex': 1, 'details': ["
        "          {'operatorName': 'type', 'specifiedAs': {'type': 'string'}, 'reason': 'type did "
        "not match',"
        "           'consideredValue': 2, 'consideredType': 'int'}]}"
        "]}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayItemsSchemaArrayRequiredMissingProperty) {
    BSONObj query = fromjson(
        "  {'$jsonSchema':"
        "   {'required': ['a'], 'properties': "
        "       {'a': {'items': [{'type': 'number'}, {'type': 'string'}]}}}}");
    BSONObj document = fromjson("{}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "     'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'required',"
        "            'specifiedAs': {'required': ['a']},"
        "            'missingProperties': ['a']}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// Verifies that "items" with an array of schemas does not produce any unwanted artifacts when it
// does not fail. We use "minItems" that fails validation to check that.
TEST(JSONSchemaValidation, ArrayItemsSchemaArrayCombinedWithMinItems) {
    BSONObj query = fromjson(
        "  {'$jsonSchema':"
        "   {'properties': "
        "       {'a': {'items': [{'type': 'number'}, {'type': 'string'}], 'minItems': 5}}}}");
    BSONObj document = fromjson("{'a': [1, 'A']}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'minItems',"
        "            'specifiedAs': {'minItems': 5},"
        "            'reason': 'array did not match specified length',"
        "            'consideredValue': [1, 'A'],"
        "            'numberOfItems': 2}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// Verifies that "items" with an array of schemas does not produce any unwanted artifacts when it
// does not fail on array elements that do not exist. We use "minItems" that fails validation to
// check that.
TEST(JSONSchemaValidation, ArrayItemsSchemaArrayCombinedWithMinItemsOnShortArray) {
    BSONObj query = fromjson(
        "  {'$jsonSchema':"
        "   {'properties': "
        "       {'a': {'items': [{'type': 'number'}, {'type': 'string'}], 'minItems': 5}}}}");
    BSONObj document = fromjson("{'a': [1]}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'minItems',"
        "            'specifiedAs': {'minItems': 5},"
        "            'reason': 'array did not match specified length',"
        "            'consideredValue': [1],"
        "            'numberOfItems': 1}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayItemsSchemaArrayTypeArrayOnNonArrayAttribute) {
    BSONObj query = fromjson(
        "  {'$jsonSchema':"
        "   {'properties': "
        "       {'a': {'items': [{'type': 'number'}, {'type': 'string'}], 'type': 'array'}}}}");
    BSONObj document = fromjson("{'a': 1}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'type',"
        "            'specifiedAs': {'type': 'array'},"
        "            'reason': 'type did not match',"
        "            'consideredValue': 1,"
        "            'consideredType': 'int'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayItemsSchemaArrayTypeArrayRequiredMissingProperty) {
    BSONObj query = fromjson(
        "  {'$jsonSchema':"
        "   {'required': ['a'],'properties': "
        "       {'a': {'items': [{'type': 'number'}, {'type': 'string'}], 'type': 'array'}}}}");
    BSONObj document = fromjson("{}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "     'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'required',"
        "            'specifiedAs': {'required': ['a']},"
        "            'missingProperties': ['a']}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// Verifies that "items" with an empty array of schemas does not produce any unwanted artifacts. We
// use "minItems" that fails validation to check that.
TEST(JSONSchemaValidation, ArrayItemsEmptySchemaArrayCombinedWithMinItems) {
    BSONObj query = fromjson(
        "  {'$jsonSchema':"
        "   {'properties': "
        "       {'a': {'items': [], 'minItems': 5}}}}");
    BSONObj document = fromjson("{'a': [1, 'A']}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'minItems',"
        "            'specifiedAs': {'minItems': 5},"
        "            'reason': 'array did not match specified length',"
        "            'consideredValue': [1, 'A'],"
        "            'numberOfItems': 2}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayItemsSchemaArrayAlwaysTrue) {
    BSONObj query = fromjson("{$nor: [{'$jsonSchema': {'items': [{'type': 'string'}]}}]}");
    BSONObj document = fromjson("{}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$nor', 'clausesSatisfied': ["
        "  {'index': 0, 'details': "
        "    {'operatorName': '$jsonSchema', 'reason': 'schema matched'}}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayItemsSchemaArrayOneEmptyObject) {
    BSONObj query = fromjson("{$nor: [{'$jsonSchema': {'items': [{}]}}]}");
    BSONObj document = fromjson("{}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$nor', 'clausesSatisfied': ["
        "  {'index': 0, 'details': "
        "    {'operatorName': '$jsonSchema', 'reason': 'schema matched'}}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayAdditionalItemsSchema) {
    BSONObj query = fromjson(
        "  {'$jsonSchema':"
        "   {'properties': "
        "       {'a': {'type': 'array', 'items': [{'type': 'number'}, {'type': 'string'}], "
        "'additionalItems': {'type': 'object', 'description': 'only extra documents'}}}}}");
    BSONObj document = fromjson("{'a': [1, 'First', {}, 'Extra element']}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "  {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "    {'propertyName': 'a', 'details': ["
        "      {'operatorName': 'additionalItems',"
        "       'description': 'only extra documents',"
        "       'reason': 'At least one additional item did not "
        "match the sub-schema', 'itemIndex': 3, 'details': ["
        "          {'operatorName': 'type', 'specifiedAs': {'type': 'object'}, 'reason': 'type did "
        "not match',"
        "           'consideredValue': 'Extra element', 'consideredType': 'string'}]}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayAdditionalItemsRequiredMissingProperty) {
    BSONObj query = fromjson(
        "  {'$jsonSchema':"
        "   {'required': ['a'], 'properties': "
        "       {'a': {'type': 'array', 'items': [{'type': 'number'}, {'type': 'string'}], "
        "'additionalItems': {'type': 'object'}}}}}");
    BSONObj document = fromjson("{}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "     'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'required',"
        "            'specifiedAs': {'required': ['a']},"
        "            'missingProperties': ['a']}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayAdditionalItemsSchemaItemsAndItemsSchemaFail) {
    BSONObj query = fromjson(
        "  {'$jsonSchema':"
        "   {'properties': "
        "       {'a': {'type': 'array', 'items': [{'type': 'number'}, {'type': 'string'}], "
        "'additionalItems': {'type': 'object'}}}}}");
    BSONObj document = fromjson("{'a': ['1', 2, {}, {'b': 1}, 'Fail']}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "  {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "    {'propertyName': 'a', 'details': ["
        "      {'operatorName': 'items', 'details': ["
        "        {'itemIndex': 0, 'details': ["
        "          {'operatorName': 'type', 'specifiedAs': {'type': 'number'}, 'reason': 'type did "
        "not match', 'consideredValue': '1', 'consideredType': 'string'}]},"
        "        {'itemIndex': 1, 'details': ["
        "          {'operatorName': 'type', 'specifiedAs': {'type': 'string'}, 'reason': 'type did "
        "not match', 'consideredValue': 2, 'consideredType': 'int'}]}"
        "]},"
        "      {'operatorName': 'additionalItems', 'reason': 'At least one additional item did not "
        "match the sub-schema', 'itemIndex': 4, 'details': ["
        "          {'operatorName': 'type', 'specifiedAs': {'type': 'object'}, 'reason': 'type did "
        "not match', 'consideredValue': 'Fail', 'consideredType': 'string'}]}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayAdditionalItemsSchemaNested) {
    BSONObj query = fromjson(
        "  {'$jsonSchema':"
        "   {'properties': "
        "       {'a': {'type': 'array', 'items': [{'type': 'string'}], 'additionalItems': "
        "         {'properties': {'b': {'items': {'type': 'object'}}}}}}}}");
    BSONObj document = fromjson("{'a': ['A', {'b': [{}, 'A']}]}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "  {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "    {'propertyName': 'a', 'details': ["
        "      {'operatorName': 'additionalItems', 'reason': 'At least one additional item did not "
        "match the sub-schema', 'itemIndex': 1, 'details': ["
        "        {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "          {'propertyName': 'b', 'details': ["
        "            {'operatorName': 'items', 'reason': 'At least one item did not match the "
        "sub-schema', 'itemIndex': 1, 'details': ["
        "              {'operatorName': 'type', 'specifiedAs': {'type': 'object'}, 'reason': "
        "'type did not match', 'consideredValue': 'A', 'consideredType': 'string'}]}]}]}]}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayAdditionalItemsSchemaTypeArrayOnNonArrayAttribute) {
    BSONObj query = fromjson(
        "  {'$jsonSchema':"
        "   {'properties': "
        "       {'a': {'type': 'array', 'items': [{'type': 'number'}, {'type': 'string'}], "
        "'additionalItems': {'type': 'object'}}}}}");
    BSONObj document = fromjson("{'a': 1}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'type',"
        "            'specifiedAs': {'type': 'array'},"
        "            'reason': 'type did not match',"
        "            'consideredValue': 1,"
        "            'consideredType': 'int'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// Verifies that "additionalItems" with a single schema does not produce any unwanted artifacts when
// it does not fail. We use "minItems" that fails validation to check that.
TEST(JSONSchemaValidation, ArrayAdditionalItemsSchemaCombinedWithMinItems) {
    BSONObj query = fromjson(
        "  {'$jsonSchema':"
        "   {'properties': "
        "       {'a': {'type': 'array', 'items': [{'type': 'number'}, {'type': 'string'}], "
        "'additionalItems': {'type': 'object'}, 'minItems': 5}}}}");
    BSONObj document = fromjson("{'a': [1, 'First', {}]}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'minItems',"
        "            'specifiedAs': {'minItems': 5},"
        "            'reason': 'array did not match specified length',"
        "            'consideredValue': [1, 'First', {}],"
        "            'numberOfItems': 3}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayAdditionalItemsSchemaAlwaysTrue) {
    BSONObj query = fromjson(
        "{$nor: [{'$jsonSchema': "
        "{'items': [{'type': 'object'}], 'additionalItems': {'type': 'object'}}}]}");
    BSONObj document = fromjson("{}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$nor', 'clausesSatisfied': ["
        "  {'index': 0, 'details': "
        "    {'operatorName': '$jsonSchema', 'reason': 'schema matched'}}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayAdditionalItemsFalse) {
    BSONObj query = fromjson(
        "{'$jsonSchema': "
        "  {'properties': {'a': "
        "    {'items': [{'type': 'number'}, {'type': 'string'}], "
        "'additionalItems': false}}}}");
    BSONObj document = fromjson("{'a': [1, 'First', 'Extra element']}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "  {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "    {'propertyName': 'a', 'details': ["
        "      {'operatorName': 'additionalItems', 'specifiedAs': {'additionalItems': false}, "
        "'reason': 'found additional items', 'additionalItems': ['Extra element']}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayAdditionalItemsFalseRequiredMissingProperty) {
    BSONObj query = fromjson(
        "{'$jsonSchema': "
        "  {'required': ['a'], 'properties': {'a': "
        "    {'items': [{'type': 'number'}, {'type': 'string'}], "
        "'additionalItems': false}}}}");
    BSONObj document = fromjson("{}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "     'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'required',"
        "            'specifiedAs': {'required': ['a']},"
        "            'missingProperties': ['a']}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayAdditionalItemsFalseTypeArrayOnNonArrayAttribute) {
    BSONObj query = fromjson(
        "{'$jsonSchema': "
        "  {'properties': {'a': "
        "    {'items': [{'type': 'number'}], 'additionalItems': false, 'type': 'array'}}}}");
    BSONObj document = fromjson("{'a': 1}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'type',"
        "            'specifiedAs': {'type': 'array'},"
        "            'reason': 'type did not match',"
        "            'consideredValue': 1,"
        "            'consideredType': 'int'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayAdditionalItemsFalseCombinedWithMinItems) {
    BSONObj query = fromjson(
        "{'$jsonSchema': "
        "  {'properties': {'a': "
        "    {'items': [{'type': 'number'}], 'additionalItems': false, 'minItems': 5}}}}");
    BSONObj document = fromjson("{'a': [1]}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'minItems',"
        "            'specifiedAs': {'minItems': 5},"
        "            'reason': 'array did not match specified length',"
        "            'consideredValue': [1],"
        "            'numberOfItems': 1}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ArrayAdditionalItemsFalseAlwaysTrue) {
    BSONObj query = fromjson(
        "{$nor: [{'$jsonSchema': {'items': [{'type': 'number'}], 'additionalItems': false}}]}");
    BSONObj document = fromjson("{}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$nor', 'clausesSatisfied': ["
        "  {'index': 0, 'details': "
        "    {'operatorName': '$jsonSchema', 'reason': 'schema matched'}}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// Object keywords

// minProperties
TEST(JSONSchemaValidation, BasicMinProperties) {
    BSONObj query = fromjson("{'$jsonSchema': {minProperties: 10}}");
    BSONObj document = fromjson("{_id: 1, a: 1, b: 2, c: 3}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "     'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'minProperties',"
        "            'specifiedAs': {minProperties: 10},"
        "            'reason': 'specified number of properties was not satisfied',"
        "            'numberOfProperties': 4}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, NestedMinProperties) {
    BSONObj query = fromjson("{'$jsonSchema': {'properties': {'a': {'minProperties': 10}}}}");
    BSONObj document = fromjson("{a: {'b': 1, 'c': 2}}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "     'schemaRulesNotSatisfied': ["
        "      {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "           {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'minProperties',"
        "            'specifiedAs': {minProperties: 10},"
        "            'reason': 'specified number of properties was not satisfied',"
        "            'numberOfProperties': 2}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, NestedMinPropertiesTypeMismatch) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'properties': {'a': {'minProperties': 10, "
        "'type': 'object'}}}}");
    BSONObj document = fromjson("{a: ['clearly', 'not', 'an', 'object']}");
    // No mention of the 'minProperties' keyword.
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "     'schemaRulesNotSatisfied': ["
        "      {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "           {'propertyName': 'a', 'details': ["
        "               {'operatorName': 'type',"
        "               'specifiedAs': {'type': 'object'},"
        "               'reason': 'type did not match',"
        "               'consideredValue': ['clearly', 'not', 'an', 'object'],"
        "               'consideredType': 'array'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, NestedMinPropertiesWithRequiredMissingProperty) {
    BSONObj query = fromjson(
        "{'$jsonSchema': "
        "   {'required': ['a'],'properties': {'a': {type: 'object', minProperties: 10}}}}");
    BSONObj document = fromjson("{}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "     'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'required',"
        "            'specifiedAs': {'required': ['a']},"
        "            'missingProperties': ['a']}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, NestedMinPropertiesWithRequiredNonObject) {
    BSONObj query = fromjson(
        "{'$jsonSchema': "
        "   {'required': ['a'],'properties': {'a': {type: 'object', minProperties: 10}}}}");
    BSONObj document = fromjson("{a: []}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "     'schemaRulesNotSatisfied': ["
        "      {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "           {'propertyName': 'a', 'details': ["
        "               {'operatorName': 'type',"
        "               'specifiedAs': {'type': 'object'},"
        "               'reason': 'type did not match',"
        "               'consideredValue': [],"
        "               'consideredType': 'array'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// maxProperties
TEST(JSONSchemaValidation, BasicMaxProperties) {
    BSONObj query = fromjson("{'$jsonSchema': {maxProperties: 2}}");
    BSONObj document = fromjson("{_id: 1, a: 1, b: 2, c: 3}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "     'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'maxProperties',"
        "            'specifiedAs': {maxProperties: 2},"
        "            'reason': 'specified number of properties was not satisfied',"
        "            'numberOfProperties': 4}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, NestedMaxProperties) {
    BSONObj query = fromjson("{'$jsonSchema': {'properties': {'a': {'maxProperties': 1}}}}");
    BSONObj document = fromjson("{a: {'b': 1, 'c': 2}}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "     'schemaRulesNotSatisfied': ["
        "      {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "           {'propertyName': 'a', 'details': ["
        "           {'operatorName': 'maxProperties',"
        "            'specifiedAs': {maxProperties: 1},"
        "            'reason': 'specified number of properties was not satisfied',"
        "            'numberOfProperties': 2}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, NestedMaxPropertiesTypeMismatch) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'properties': {'a': {'maxProperties': 10, "
        "'type': 'object'}}}}");
    BSONObj document = fromjson("{a: ['clearly', 'not', 'an', 'object']}");
    // No mention of the 'maxProperties' keyword.
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "     'schemaRulesNotSatisfied': ["
        "      {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "           {'propertyName': 'a', 'details': ["
        "               {'operatorName': 'type',"
        "               'specifiedAs': {'type': 'object'},"
        "               'reason': 'type did not match',"
        "               'consideredValue': ['clearly', 'not', 'an', 'object'],"
        "               'consideredType': 'array'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// property dependencies
TEST(JSONSchemaValidation, BasicPropertyDependency) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'dependencies': {'a': ['b', 'c'],"
        "                                  title: 'a needs b and c'}}}");
    BSONObj document = fromjson("{'a': 1, 'b': 2}");
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'dependencies', title: 'a needs b and c', failingDependencies: ["
        "       {conditionalProperty: 'a', "
        "        missingProperties: [ 'c' ]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, NestedPropertyDependency) {
    BSONObj query =
        fromjson("{'$jsonSchema': {'properties': {'obj': {'dependencies': {'a': ['b','c']}}}}}");
    BSONObj document = fromjson("{'obj': {'a': 1, 'b': 2}}");
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'properties', propertiesNotSatisfied: ["
        "       {propertyName: 'obj', details: ["
        "           {operatorName: 'dependencies', failingDependencies: ["
        "               {conditionalProperty: 'a', "
        "               missingProperties: ['c']}]}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, PropertyDependencyOneFailingDependency) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'dependencies': {'a': ['b', 'c'], 'b': ['c','d'], 'c': ['a', 'b']}}}");
    BSONObj document = fromjson("{'a': 1, 'b': 2, 'c': 5}");
    // Should only report an error for 'b's dependency of missing 'd'.
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'dependencies', failingDependencies: ["
        "       {conditionalProperty: 'b', "
        "       missingProperties: ['d']}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, PropertyDependencyManyFailingDependencies) {
    BSONObj query = fromjson(
        "{'$jsonSchema': "
        "{'dependencies': {'a': ['b', 'c', 'd'], 'b': ['c','d'], 'e': ['c', 'd']}}}");
    BSONObj document = fromjson("{'a': 1, 'b': 2, 'e': 3}");
    // Should only report an error for 'b's dependency of missing 'd'.
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'dependencies', failingDependencies: ["
        "       {conditionalProperty: 'a', missingProperties: ['c','d']}, "
        "       {conditionalProperty: 'b', missingProperties: ['c','d']},"
        "       {conditionalProperty: 'e', missingProperties: ['c','d']}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, PropertyFailingBiconditionalDependency) {
    BSONObj query = fromjson("{'$jsonSchema': {'dependencies': {'a': ['b'], 'b': ['a']}}}");
    BSONObj document = fromjson("{'a': 1}");
    BSONObj expectedError = fromjson(
        "{ operatorName: '$jsonSchema', schemaRulesNotSatisfied: [ { operatorName: 'dependencies', "
        "failingDependencies: [ { conditionalProperty: 'a', missingProperties: [ 'b' ] } ] } ] }");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, PropertyDependencyWithRequiredMissingProperty) {
    BSONObj query =
        fromjson("{'$jsonSchema': {'required': ['a'], 'dependencies': {'a': ['b', 'c']}}}");
    BSONObj document = fromjson("{}");
    // Should not mention 'dependencies'.
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'required',"
        "   specifiedAs: {required: ['a']}, "
        "   missingProperties: ['a']}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, PropertyDependencyWithRequiredMissingDependency) {
    BSONObj query =
        fromjson("{'$jsonSchema': {'required': ['a'], 'dependencies': {'a': ['b', 'c']}}}");
    BSONObj document = fromjson("{'a': 1}");
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'dependencies', failingDependencies: ["
        "       {conditionalProperty: 'a', "
        "       missingProperties: ['b', 'c']}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// schema dependencies
TEST(JSONSchemaValidation, BasicSchemaDependency) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'dependencies': {'a': {'properties': {'b': {'type': 'number'}}},"
        "                                  title: 'a needs b'}}}");
    BSONObj document = fromjson("{'a': 1, 'b': 'foo'}");
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'dependencies', title: 'a needs b', failingDependencies: ["
        "       {conditionalProperty: 'a', details: ["
        "       {operatorName: 'properties',propertiesNotSatisfied: ["
        "           {propertyName: 'b', details: ["
        "               {operatorName: 'type', "
        "               specifiedAs: {type: 'number'}, "
        "               reason: 'type did not match', "
        "               consideredValue:'foo',"
        "               consideredType: 'string'}]}]}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, NestedSchemaDependency) {
    BSONObj query = fromjson(
        "{'$jsonSchema': "
        "   {'properties': {'topLevelField': "
        "       {'dependencies': {'a': {'properties': {'b': {'type': 'number'}}}}}}}}");
    BSONObj document = fromjson("{'topLevelField': {'a': 1, 'b': 'foo'}}");
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        " {operatorName: 'properties', propertiesNotSatisfied: ["
        "   {propertyName: 'topLevelField', 'details': ["
        "   {operatorName: 'dependencies', failingDependencies: ["
        "       {conditionalProperty: 'a', details: ["
        "       {operatorName: 'properties',propertiesNotSatisfied: ["
        "           {propertyName: 'b', details: ["
        "               {operatorName: 'type', "
        "               specifiedAs: {type: 'number'}, "
        "               reason: 'type did not match', "
        "               consideredValue:'foo',"
        "               consideredType: 'string'}]}]}]}]}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, SchemaDependencyWithRequiredMissingProperty) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'required': ['a'], 'dependencies': {'a': {'properties': {'b': {'type':"
        "'number'}}}}}}");
    BSONObj document = fromjson("{'b': 'foo'}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "     'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'required',"
        "            'specifiedAs': {'required': ['a']},"
        "            'missingProperties': ['a']}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, SchemaDependencyWithRequiredFailedDependency) {
    BSONObj query = fromjson(
        "{'$jsonSchema': "
        "{'required': ['a'], 'dependencies': {'a': {'properties': {'b': {'type': 'number'}}}}}}");
    BSONObj document = fromjson("{a: 1, 'b': 'foo'}");
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'dependencies', failingDependencies: ["
        "       {conditionalProperty: 'a', details: ["
        "       {operatorName: 'properties',propertiesNotSatisfied: ["
        "           {propertyName: 'b', details: ["
        "               {operatorName: 'type', "
        "               specifiedAs: {type: 'number'}, "
        "               reason: 'type did not match', "
        "               consideredValue:'foo',"
        "               consideredType: 'string'}]}]}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, BiconditionalSchemaDependency) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'dependencies': "
        "   {'a': {'properties': {'b': {'type': 'array'}}}, "
        "   'b': {'properties': {'a': {'type': 'number'}}}}}}");
    BSONObj document = fromjson("{'a': 'foo', 'b': 'bar'}");
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'dependencies', failingDependencies: ["
        "       {conditionalProperty: 'a', details: ["
        "       {operatorName: 'properties', propertiesNotSatisfied: ["
        "           {propertyName: 'b', details: ["
        "               {operatorName: 'type', "
        "               specifiedAs: {type: 'array'}, "
        "               reason: 'type did not match', "
        "               consideredValue: 'bar',"
        "               consideredType: 'string'}]}]}]},"
        "       {conditionalProperty: 'b', details: ["
        "       {operatorName: 'properties',propertiesNotSatisfied: ["
        "           {propertyName: 'a', details: ["
        "               {operatorName: 'type', "
        "               specifiedAs: {type: 'number'}, "
        "               reason: 'type did not match', "
        "               consideredValue:'foo',"
        "               consideredType: 'string'}]}]}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, SchemaDependencySomeDependenciesSatisfied) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'dependencies': "
        "   {'a': {'properties': {'b': {'type': 'number'}}},"
        "   'b': {'properties': {'c': {'type': 'string'}}},"
        "   'c': {'properties': {'a': {'type': 'array'}}}}}}");
    BSONObj document = fromjson("{'a': [0,1,2,3], 'b': 1, 'c': 12}");
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'dependencies', failingDependencies: ["
        "       {conditionalProperty: 'b', details: ["
        "       {operatorName: 'properties',propertiesNotSatisfied: ["
        "           {propertyName: 'c', details: ["
        "               {operatorName: 'type', "
        "               specifiedAs: {type: 'string'}, "
        "               reason: 'type did not match', "
        "               consideredValue: 12,"
        "               consideredType: 'int'}]}]}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, SchemaDependencyAllDependenciesFailed) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'dependencies': "
        "   {'a': {'properties': {'b': {'type': 'number'}}},"
        "   'b': {'properties': {'c': {'type': 'string'}}},"
        "   'c': {'properties': {'a': {'type': 'array'}}}}}}");
    BSONObj document = fromjson("{'a': 'foo', 'b': [], 'c': 12}");
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'dependencies', failingDependencies: ["
        "       {conditionalProperty: 'a', details: ["
        "       {operatorName: 'properties', propertiesNotSatisfied: ["
        "           {propertyName: 'b', details: ["
        "               {operatorName: 'type', "
        "               specifiedAs: {type: 'number'}, "
        "               reason: 'type did not match', "
        "               consideredValue: [],"
        "               consideredType: 'array'}]}]}]},"
        "       {conditionalProperty: 'b', details: ["
        "       {operatorName: 'properties', propertiesNotSatisfied: ["
        "           {propertyName: 'c', details: ["
        "               {operatorName: 'type', "
        "               specifiedAs: {type: 'string'}, "
        "               reason: 'type did not match', "
        "               consideredValue: 12,"
        "               consideredType: 'int'}]}]}]},"
        "       {conditionalProperty: 'c', details: ["
        "       {operatorName: 'properties',propertiesNotSatisfied: ["
        "           {propertyName: 'a', details: ["
        "               {operatorName: 'type', "
        "               specifiedAs: {type: 'array'}, "
        "               reason: 'type did not match', "
        "               consideredValue:'foo',"
        "               consideredType: 'string'}]}]}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, SchemaDependencyAndPropertyDependency) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'dependencies': "
        "   {'a': {'properties': {'b': {'type': 'number'}}},"
        "   'b': ['a', 'c']}}}");
    BSONObj document = fromjson("{'a': [0,1,2,3], 'b': 'foo'}");
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'dependencies', failingDependencies: ["
        "       {conditionalProperty: 'a', details: ["
        "       {operatorName: 'properties', propertiesNotSatisfied: ["
        "           {propertyName: 'b', details: ["
        "               {operatorName: 'type', "
        "               specifiedAs: {type: 'number'}, "
        "               reason: 'type did not match', "
        "               consideredValue: 'foo',"
        "               consideredType: 'string'}]}]}]},"
        "       {conditionalProperty: 'b', "
        "       missingProperties: ['c']}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// additionalProperties (boolean argument)
TEST(JSONSchemaValidation, BasicAdditionalPropertiesFalse) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'properties': {'b': {'type': 'number'}}, 'additionalProperties': "
        "false}}");
    BSONObj document = fromjson("{'_id': 0, 'a': 1, 'b': 2}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'additionalProperties', "
        "   'specifiedAs': {additionalProperties: false},"
        "   'additionalProperties': ['_id', 'a']}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, BasicAdditionalPropertiesFalseRequiredMissingProperty) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'required': ['a'], 'properties': {'b': {'type': 'number'}}, "
        "'additionalProperties': false}}");
    BSONObj document = fromjson("{'_id': 0, 'b': 2}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'additionalProperties', "
        "   'specifiedAs': {additionalProperties: false},"
        "   'additionalProperties': ['_id']},"
        "   {'operatorName': 'required',"
        "   'specifiedAs': {'required': ['a']},"
        "   'missingProperties': ['a']}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, AdditionalPropertiesTrueProducesNoError) {
    BSONObj query =
        fromjson("{'$jsonSchema': {'minProperties': 100, 'additionalProperties': true}}");
    BSONObj document = fromjson("{'_id': 0, 'a': 1, 'b': 2}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "     'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'minProperties',"
        "            'specifiedAs': {minProperties: 100},"
        "            'reason': 'specified number of properties was not satisfied',"
        "            'numberOfProperties': 3}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, BasicAdditionalPropertiesFalseNested) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'properties': {'b': {'type': 'object', 'additionalProperties':false}}}}");
    BSONObj document = fromjson("{'b': {'a': 1}}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'b', 'details': ["
        "           {'operatorName': 'additionalProperties', "
        "           'specifiedAs': {additionalProperties: false},"
        "           'additionalProperties': ['a']}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, BasicAdditionalPropertiesFalseNestedMultiLevel) {
    BSONObj query = fromjson(
        "{'$jsonSchema': "
        "   {'additionalProperties': false, 'properties':"
        "       {'_id': {},'a': {'type': 'object', 'additionalProperties':false}}}}");
    BSONObj document = fromjson("{_id: 1, 'a': {'b': 1}, 'c': 1}");
    // Report _id and c at the top level and b in the nested error.
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'properties', propertiesNotSatisfied: ["
        "       {propertyName: 'a', details: ["
        "               {operatorName: 'additionalProperties', "
        "               specifiedAs: {additionalProperties: false}, "
        "               additionalProperties: ['b']}]}]}, "
        "   {operatorName: 'additionalProperties',"
        "   specifiedAs: {additionalProperties: false}, "
        "   additionalProperties: ['c']}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// additionalProperties (object argument)
TEST(JSONSchemaValidation, BasicAdditionalPropertiesSchema) {
    BSONObj query = fromjson(
        "{'$jsonSchema': "
        "   {'properties': {'a': {'type': 'number'}}, "
        "   'additionalProperties': {'type': 'string'}}}");
    BSONObj document = fromjson(
        "{'a': 1, 'b': 'not this one', 'c': 'not this one either, but the next one', 'd': 1 }");
    // Should only produce an error for 'd'.
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "       {'operatorName': 'additionalProperties',"
        "        'reason':'at least one additional property did not match the subschema',"
        "        'failingProperty': 'd', 'details': [ "
        "           {'operatorName': 'type', "
        "           'specifiedAs': {type: 'string'},"
        "           'reason': 'type did not match',"
        "           'consideredValue': 1,"
        "           'consideredType': 'int'}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, BasicAdditionalPropertiesSchemaRequiredMissingProperty) {
    BSONObj query = fromjson(
        "{'$jsonSchema': "
        "   {'required': ['a', 'b'], 'properties': {'a': {'type': 'number'}}, "
        "   'additionalProperties': {'type': 'string'}}}");
    BSONObj document = fromjson("{'a': 1, 'c': 'not this one, but the next one', 'd': 1 }");
    // Should produce an error for 'd' and 'b'.
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "       {'operatorName': 'additionalProperties',"
        "        'reason':'at least one additional property did not match the subschema',"
        "        'failingProperty': 'd', 'details': [ "
        "           {'operatorName': 'type', "
        "           'specifiedAs': {type: 'string'},"
        "           'reason': 'type did not match',"
        "           'consideredValue': 1,"
        "           'consideredType': 'int'}]},"
        "       {'operatorName': 'required',"
        "       'specifiedAs': {'required': ['a','b']},"
        "        'missingProperties': ['b']}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, BasicAdditionalPropertiesSchemaNested) {
    BSONObj query = fromjson(
        "{'$jsonSchema': "
        "   {'properties': {'b': {'type': 'object', 'additionalProperties': {'type':'string'}}}}}");
    BSONObj document = fromjson("{'b': {'a': 1}}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'b', 'details': ["
        "           {'operatorName': 'additionalProperties',"
        "           'reason': 'at least one additional property did not match the subschema',"
        "           'failingProperty': 'a', 'details': [ "
        "               {'operatorName': 'type',"
        "               'specifiedAs': {type: 'string'},"
        "               'reason': 'type did not match',"
        "               'consideredValue': 1,"
        "               'consideredType': 'int'}]}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, AdditionalPropertiesSchemaNoErrorWhenAdditionalPropertiesAreValid) {
    BSONObj query = fromjson(
        "{'$jsonSchema': "
        "   {'properties': {'_id': {}, 'a': {'type': 'number'}}, "
        "   'minProperties': 10,"
        "   'additionalProperties': {'type': 'string'}}}");
    BSONObj document = fromjson(
        "{'_id': 'foo', 'a': 1, "
        "'b': 'all', 'c': 'additional', 'd': 'properties', 'e': 'are', 'f': 'strings!'}");
    // Should only produce an error for 'd'.
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "       {'operatorName': 'minProperties',"
        "       'specifiedAs': {minProperties: 10},"
        "       'reason': 'specified number of properties was not satisfied',"
        "       'numberOfProperties': 7}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// patternProperties
TEST(JSONSchemaValidation, BasicPatternProperties) {
    BSONObj query = fromjson("{'$jsonSchema': {'patternProperties': {'^S': {'type': 'number'}}}}");
    BSONObj document = fromjson("{'Super': 1, 'Slow': '67'}");
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'patternProperties', details: ["
        "       {propertyName: 'Slow',"
        "       regexMatched: '^S', "
        "       details: [{ "
        "           operatorName: 'type',"
        "           specifiedAs: {type: 'number'}, "
        "           reason: 'type did not match',"
        "           consideredValue: '67',"
        "           consideredType: 'string'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, BasicPatternPropertiesRequired) {
    BSONObj query = fromjson(
        "{'$jsonSchema': "
        "   {'required': ['Super'], 'patternProperties': {'^S': {'type':'number'}}}}");
    BSONObj document = fromjson("{'super': 1, 'slow': '67'}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "     'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'required',"
        "            'specifiedAs': {'required': ['Super']},"
        "            'missingProperties': ['Super']}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, NestedPatternProperties) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'properties': {'a': {'patternProperties': {'^S': {'type': "
        "'number'}}}}}}");
    BSONObj document = fromjson("{'a': {'String': 1, 'StringNoNumber': '67'}}");
    BSONObj expectedError = fromjson(
        "{ operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'properties', propertiesNotSatisfied: ["
        "       {propertyName: 'a', details: ["
        "           {operatorName: 'patternProperties', details: ["
        "               {propertyName: 'StringNoNumber',"
        "               regexMatched: '^S',"
        "               details: ["
        "                   {operatorName: 'type',"
        "                   specifiedAs: {type: 'number'}, "
        "                   reason: 'type did not match', "
        "                   consideredValue: '67', "
        "                   consideredType: 'string'}]}]}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, PatternPropertiesSomePatternsSatifised) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'patternProperties': {'^a': {'type': 'string'},'^S': {'type': "
        "'number'}}}}");
    BSONObj document = fromjson(
        "{'a': 'foo', 'Super': 1, 'aa': 'also a string', 'aaa': 'never a number!', "
        "'Slow': '67'}");
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'patternProperties', details: ["
        "       {propertyName: 'Slow', "
        "       regexMatched: '^S', "
        "       details: ["
        "           {operatorName: 'type', "
        "           specifiedAs: {type: 'number'}, "
        "           reason: 'type did not match', "
        "           consideredValue: '67', "
        "           consideredType: 'string'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, PatternPropertiesRecursive) {
    BSONObj query = fromjson(
        "{'$jsonSchema': "
        "   {'patternProperties': {'^a': {'patternProperties': {'b+': {type: 'string'}}}}}}");
    BSONObj document = fromjson("{'a': {'bbbbb': 1}}");
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'patternProperties', details: ["
        "       {propertyName: 'a', regexMatched: '^a', details: ["
        "           { operatorName: 'patternProperties', details: ["
        "               {propertyName: 'bbbbb', regexMatched: 'b+', details: ["
        "                   {operatorName: 'type', "
        "                   specifiedAs: { type: 'string' }, "
        "                   reason: 'type did not match',"
        "                   consideredValue: 1, "
        "                   consideredType: 'int'}]}]}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, PatternPropertiesOneElementFailsMultiplePatternSchemas) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'patternProperties': {'^a': {'minimum': 3}, 'b$': {'maximum': 1}}}}");
    BSONObj document = fromjson("{'a': 45, 'b': 0, 'ab': 2}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'patternProperties', details: ["
        "       {propertyName: 'ab', regexMatched: '^a', details: ["
        "           {operatorName: 'minimum', "
        "           specifiedAs: {minimum: 3}, "
        "           reason: 'comparison failed', "
        "           consideredValue: 2}]}, "
        "       {propertyName: 'ab', regexMatched: 'b$', details: ["
        "           {operatorName: 'maximum', "
        "           specifiedAs: { maximum: 1 },"
        "           reason: 'comparison failed',"
        "           consideredValue: 2}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// patternProperties and additionalProperties (false) combined
TEST(JSONSchemaValidation, PatternPropertiesAndAdditionalPropertiesFalse) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'patternProperties': {'^S': {'type': 'number'}}, "
        "'additionalProperties': false}}");
    BSONObj document = fromjson("{'Super': 1, 'Slow': 'oh no a string', b: 1}");
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'additionalProperties',"
        "   specifiedAs: {additionalProperties: false},"
        "   additionalProperties: ['b', '_id']}, "
        "   {operatorName: 'patternProperties', details: ["
        "       {propertyName: 'Slow', "
        "       regexMatched: '^S',"
        "       details: ["
        "           {operatorName: 'type', "
        "           specifiedAs: {type: 'number'},"
        "           reason: 'type did not match',"
        "           consideredValue: 'oh no a string',"
        "           consideredType: 'string'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation,
     PatternPropertiesAndAdditionalPropertiesFalseOnlyPatternPropertiesFails) {
    BSONObj query = fromjson(
        "{'$jsonSchema': "
        "   {'properties': {_id: {}},"
        "   'patternProperties': {'^S': {'type': 'number'}}, "
        "   'additionalProperties': false}}");
    BSONObj document = fromjson("{'_id': 1, 'Super': 1, 'Slow': 'oh no a string'}");
    // There should be no 'additionalProperties' error.
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'patternProperties', details: ["
        "       {propertyName: 'Slow', "
        "       regexMatched: '^S', details: ["
        "           {operatorName: 'type', "
        "           specifiedAs: {type: 'number'}, "
        "           reason: 'type did not match',"
        "           consideredValue: 'oh no a string', "
        "           consideredType: 'string'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, PatternPropertiesAndAdditionalPropertiesTrueOnlyPatternPropertiesFails) {
    BSONObj query = fromjson(
        "{'$jsonSchema': "
        "   {'properties': {_id: {}},"
        "   'patternProperties': {'^S': {'type': 'number'}}, "
        "   'additionalProperties': true}}");
    BSONObj document = fromjson("{'_id': 1, 'Super': 1, 'Slow': 'oh no a string', a: 'foo'}");
    // There should be no 'additionalProperties' error.
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'patternProperties', details: ["
        "       {propertyName: 'Slow', "
        "       regexMatched: '^S', details: ["
        "           {operatorName: 'type', "
        "           specifiedAs: {type: 'number'}, "
        "           reason: 'type did not match',"
        "           consideredValue: 'oh no a string', "
        "           consideredType: 'string'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation,
     PatternPropertiesAndAdditionalPropertiesFalseOnlyAdditionalPropertiesFails) {
    BSONObj query = fromjson(
        "{'$jsonSchema': "
        "   {'patternProperties': {'^S': {'type': 'number'}}, 'additionalProperties': false}}");
    BSONObj document = fromjson("{'Super': 1, 'Slow': 2, 'b': 'clearly an extra property'}");
    // There should be no 'patternProperties' error.
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'additionalProperties', "
        "   specifiedAs: {additionalProperties: false}, "
        "   additionalProperties: ['b', '_id']}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, PatternPropertiesAndAdditionalPropertiesFalseNeitherFail) {
    BSONObj query = fromjson(
        "{'$jsonSchema':"
        "   {'minProperties': 10,"
        "   'patternProperties': {'^_id': {}, '^S': {'type': 'number'}}, "
        "   'additionalProperties': false}}");
    BSONObj document = fromjson("{'_id': 1, 'Super': 1, 'Slow': 2}");
    // The error should reference neither 'patternProperties' nor 'additionalProperties'.
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "     'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'minProperties',"
        "            'specifiedAs': {minProperties: 10},"
        "            'reason': 'specified number of properties was not satisfied',"
        "            'numberOfProperties': 3}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// patternProperties and additionalProperties (object) combined
TEST(JSONSchemaValidation, PatternPropertiesAndAdditionalPropertiesSchema) {
    BSONObj query = fromjson(
        "{'$jsonSchema': "
        "   {'patternProperties': {'^S': {'type': 'number', title: 'properties starting with S',"
        "                          description: 'property should be of integer type'}},"
        "   'additionalProperties': {'type': 'string', title: 'additional properties',"
        "                            description: 'additional properties are strings'}}}");
    BSONObj document = fromjson("{'Super': 1, 'Slow': 'oh no a string', b: 1}");
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'additionalProperties', "
        "    title: 'additional properties',"
        "    description: 'additional properties are strings',"
        "    reason: 'at least one additional property did not match the subschema',"
        "    failingProperty: 'b', details: ["
        "       {operatorName: 'type', "
        "       specifiedAs: {type: 'string'}, "
        "       reason: 'type did not match', "
        "       consideredValue: 1, "
        "       consideredType: 'int'}]}, "
        "   {operatorName: 'patternProperties', details: ["
        "       {propertyName: 'Slow',"
        "        title: 'properties starting with S',"
        "        description: 'property should be of integer type',"
        "        regexMatched: '^S',"
        "        details: ["
        "           {operatorName: 'type',"
        "            specifiedAs: {type:'number'},"
        "            reason: 'type did not match',"
        "            consideredValue: 'oh no a string',"
        "            consideredType: 'string'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation,
     PatternPropertiesAndAdditionalPropertiesSchemaOnlyPatternPropertiesFails) {
    BSONObj query = fromjson(
        "{'$jsonSchema': "
        "   {'properties': {_id: {}}, "
        "   'patternProperties': {'^S': {'type': 'number'}},"
        "   'additionalProperties': {'type': 'string'}}}");
    BSONObj document =
        fromjson("{_id: 1, 'Super': 1, 'Slow': 'oh no a string', b: 'actually a string!'}");
    // There should only be a 'patternProperties' error
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'patternProperties', details: ["
        "       {propertyName: 'Slow', "
        "       regexMatched: '^S', "
        "       details: ["
        "           {operatorName: 'type',"
        "           specifiedAs: {type: 'number'}, "
        "           reason: 'type did not match', "
        "           consideredValue: 'oh no a string',"
        "           consideredType: 'string'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation,
     PatternPropertiesAndAdditionalPropertiesSchemaOnlyAdditionalPropertiesFails) {
    BSONObj query = fromjson(
        "{'$jsonSchema':"
        "   {'patternProperties': {'^S': {'type': 'number'}}, "
        "   'additionalProperties': {'type': 'string'}}}");
    BSONObj document = fromjson("{'Super': 1, 'Slow': 2, 'b': 1}");
    // There should be no 'patternProperties' error.
    BSONObj expectedError = fromjson(
        "{operatorName: '$jsonSchema', schemaRulesNotSatisfied: ["
        "   {operatorName: 'additionalProperties', "
        "   reason: 'at least one additional property did not match the subschema',"
        "   failingProperty: 'b', details: ["
        "       {operatorName: 'type', "
        "       specifiedAs: {type: 'string'}, "
        "       reason: 'type did not match', "
        "       consideredValue: 1, consideredType: 'int'}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, PatternPropertiesAndAdditionalPropertiesSchemaNeitherFail) {
    BSONObj query = fromjson(
        "{'$jsonSchema':"
        "   {'minProperties': 10,"
        "   'patternProperties': {'^S': {'type': 'number'}}, "
        "   'additionalProperties': {'type': 'number'}}}");
    BSONObj document = fromjson("{'_id': 1, 'Super': 1, 'Slow': 2, 'b': 1}");
    // The error should reference neither 'patternProperties' nor 'additionalProperties'.
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "     'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'minProperties',"
        "            'specifiedAs': {minProperties: 10},"
        "            'reason': 'specified number of properties was not satisfied',"
        "            'numberOfProperties': 4}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// required
TEST(JSONSchemaValidation, BasicRequired) {
    BSONObj query = fromjson("{'$jsonSchema': {required: ['a','b','c']}}");
    BSONObj document = fromjson("{'c': 1, 'd': 2}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "     'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'required',"
        "            'specifiedAs': {'required': ['a','b','c']},"
        "            'missingProperties': ['a', 'b']}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, RequiredMixedWithProperties) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'properties': {'a': {minimum: 2}, 'd': {maximum: 5}}, 'required': "
        "['a','b','c']}}");
    BSONObj document = fromjson("{'c': 1, 'b': 2}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "     'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'required',"
        "            'specifiedAs': {'required': ['a','b','c']},"
        "            'missingProperties': ['a']}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, RequiredNested) {
    BSONObj query =
        fromjson("{'$jsonSchema': {'properties': {'topLevelField': {'required': ['a','b','c']}}}}");
    BSONObj document = fromjson("{'topLevelField': {'c': 1, 'd': 2}}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "     'schemaRulesNotSatisfied': ["
        "      {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "           {'propertyName': 'topLevelField', 'details': ["
        "           {'operatorName': 'required',"
        "            'specifiedAs': {'required': ['a','b','c']},"
        "            'missingProperties': ['a', 'b']}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

}  // namespace
}  // namespace mongo
