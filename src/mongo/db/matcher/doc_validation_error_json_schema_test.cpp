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

#include "mongo/platform/basic.h"

#include "mongo/db/matcher/doc_validation_error_test.h"

namespace mongo {
namespace {

// properties
TEST(JSONSchemaValidation, BasicProperties) {
    BSONObj query = fromjson("{'$jsonSchema': {'properties': {'a': {'minimum': 1}}}}}");
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

// Exclusive minimum/maximum
TEST(JSONSchemaValidation, ExclusiveMinimum) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'properties': {'a': {'minimum': 1, 'exclusiveMinimum': true}}}}}");
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
        "true}}}}}}");
    BSONObj document = fromjson("{a: 2}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "      'schemaRulesNotSatisfied': ["
        "       {'operatorName': 'not', 'reason': 'child expression matched'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ExclusiveMinimumInvertedTypeMismatch) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'not': {'properties': {'a': {'minimum': 1, 'exclusiveMinimum': "
        "true}}}}}}");
    BSONObj document = fromjson("{a: 'foo'}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "       {'operatorName': 'not', 'reason': 'child expression matched'}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ExclusiveMinimumInvertedMissingField) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'not': {'properties': {'a': {'minimum': 1, 'exclusiveMinimum': "
        "true}}}}}}");
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
        "{'$jsonSchema': {'properties': {'a': {'maximum': 1, 'exclusiveMaximum': true}}}}}");
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

// TODO: Update the test when SERVER-50859 is implemented.
TEST(JSONSchemaValidation, MaximumTypeNumberWithEmptyArray) {
    BSONObj query =
        fromjson("{'$jsonSchema': {'properties': {'a': {'maximum': 1, type: 'number'}}}}");
    BSONObj document = fromjson("{a: []}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "      'schemaRulesNotSatisfied': ["
        "           {'operatorName': 'properties',"
        "            'propertiesNotSatisfied': ["
        "                   {'propertyName': 'a', 'details': "
        "                       [{'operatorName': 'maximum',"
        "                       'specifiedAs': {'maximum' : 1},"
        "                       'reason': 'comparison failed',"
        "                       'consideredValues': []},"
        "                        {'operatorName': 'type',"
        "                       'specifiedAs': {'type': 'number'},"
        "                       'reason': 'type did not match',"
        "                       'consideredValue': [],"
        "                       'consideredType': 'array'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, ExclusiveMaximumInverted) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {'not': {'properties': {'a': {'maximum': 1, 'exclusiveMaximum': "
        "true}}}}}}");
    BSONObj document = fromjson("{a: 0}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "      'schemaRulesNotSatisfied': ["
        "       {'operatorName': 'not', 'reason': 'child expression matched'}]}]}]}");
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
        "           {'b': {'minimum': 1}}}}}}}}");
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
        "           {'b': {'minimum': 10, 'maximum': -10}}}}}}}}");
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
        "'consideredValue': 0}]}]}]}");
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
        "       'd': {'properties': {'e': {'minimum': 50}, 'f': {'minimum': 100}}}}}}}}}");
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
        "           {'b': {'minimum': 1}}}}}}}}");
    BSONObj document = fromjson("{'a': {'b': 1}}");
    BSONObj expectedError = BSON("operatorName"
                                 << "$jsonSchema"
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
        "         'c': {'type': 'object'}}}}}");
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
        "       {'propertyName': 'c', 'details': ["
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
        "         'c': {'bsonType': 'decimal'}}}}}");
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
        "       {'a': {'bsonType': 'string'}}}}");
    // Even though 'a' is an array of strings, this is a type mismatch in the world of $jsonSchema.
    BSONObj document = fromjson("{'a': ['Mihai', 'was', 'here']}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',"
        "'schemaRulesNotSatisfied': ["
        "   {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "       {'propertyName': 'a', 'details': ["
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
    BSONObj query =
        fromjson("{'$jsonSchema': {'properties': {'a': {'type': 'string','minLength': 4}}}}");
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
        "           {'operatorName': 'minLength', "
        "           'specifiedAs': { 'minLength': 4 }, "
        "           'reason': 'type did not match', "
        "           'consideredType': 'int', "
        "           'expectedType': 'string', "
        "           'consideredValue': 1 }, "
        "           {'operatorName': 'type', "
        "           'specifiedAs': { 'type': 'string' }, "
        "           'reason': 'type did not match', "
        "           'consideredValue': 1, "
        "           'consideredType': 'int' }]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MinLengthNested) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {"
        "   'properties': {"
        "       'a': {'properties': "
        "           {'b': {'type': 'string', 'minLength': 4}}}}}}}}");
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
    BSONObj query =
        fromjson("{'$jsonSchema': {'properties': {'a': {'type': 'string','maxLength': 4}}}}");
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
        "           {'operatorName': 'maxLength', "
        "           'specifiedAs': { 'maxLength': 4 }, "
        "           'reason': 'type did not match', "
        "           'consideredType': 'int', "
        "           'expectedType': 'string', "
        "           'consideredValue': 1 }, "
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
        "           {'b': {'type': 'string', 'maxLength': 4}}}}}}}}");
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

TEST(JSONSchemaValidation, PatternNonString) {
    BSONObj query =
        fromjson("{'$jsonSchema': {'properties': {'a': {'type': 'string','pattern': '^S'}}}}");
    BSONObj document = fromjson("{'a': 1}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "   { operatorName: 'properties', 'propertiesNotSatisfied': ["
        "       {propertyName: 'a', 'details': ["
        "           {'operatorName': 'pattern',"
        "           'specifiedAs': { pattern: '^S' }, "
        "           'reason': 'type did not match', "
        "           'consideredType': 'int', "
        "           'expectedTypes': [ 'regex', 'string', 'symbol' ], "
        "           'consideredValue': 1 }, "
        "           {'operatorName': 'type', "
        "           'specifiedAs': { 'type': 'string' }, "
        "           'reason': 'type did not match', "
        "           'consideredValue': 1, "
        "           'consideredType': 'int'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, PatternNested) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {"
        "   'properties': {"
        "       'a': {'properties': "
        "           {'b': {'type': 'string', 'pattern': '^S'}}}}}}}}");
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
        "           {'operatorName': 'multipleOf', "
        "           'specifiedAs': { 'multipleOf': 2.1 }, "
        "           'reason': 'type did not match', "
        "           'consideredType': 'string', "
        "           'expectedTypes': ['decimal', 'double', 'int', 'long'], "
        "           'consideredValue': 'foo' }, "
        "           {'operatorName': 'type', "
        "           'specifiedAs': { 'type': 'number' }, "
        "           'reason': 'type did not match', "
        "           'consideredValue': 'foo', "
        "           'consideredType': 'string' }]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaValidation, MultipleOfNested) {
    BSONObj query = fromjson(
        "{'$jsonSchema': {"
        "   'properties': {"
        "       'a': {'properties':  {'b': {'multipleOf': 2.1}}}}}}}");
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
        "                                       'reason': 'considered value is not a multiple of "
        "the specified value',"
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
    BSONObj document = BSON("a"
                            << "foo");
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
    blob.fleBlobSubtype = FleBlobSubtype::Deterministic;
    memset(blob.keyUUID, 0, sizeof(blob.keyUUID));
    blob.originalBsonType = BSONType::String;

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
    blob.fleBlobSubtype = FleBlobSubtype::Deterministic;
    memset(blob.keyUUID, 0, sizeof(blob.keyUUID));
    blob.originalBsonType = BSONType::NumberInt;

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
    BSONObj query =
        fromjson("{$jsonSchema: {properties: {a: {oneOf: [{minimum: 1},{maximum: 3}]}}}}");
    BSONObj document = fromjson("{a: 2}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema',schemaRulesNotSatisfied: ["
        "       {'operatorName': 'properties', propertiesNotSatisfied: ["
        "           {propertyName: 'a', 'details': ["
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
        "               {'operatorName': 'not', 'reason': 'child expression matched'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(JSONSchemaLogicalKeywordValidation, NestedNot) {
    BSONObj query = fromjson("{$jsonSchema: {not: {not: {properties: {a: {minimum: 3}}}}}}");
    BSONObj document = fromjson("{a: 1}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "               {'operatorName': 'not', 'reason': 'child expression matched'}]}]}]}");
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
    BSONObj query = fromjson("{$jsonSchema: {properties: {a: {not: {}}}}}}");
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
    BSONObj query = fromjson("{'$jsonSchema': {'properties': {'a': {'enum': [1,2,3]}}}}}");
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
    BSONObj query = fromjson("{'$jsonSchema': {'properties': {'a': {'not': {'enum': [1,2,3]}}}}}}");
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
        "               {'oneOf': [{minimum: 1}, {enum: [6,7,8]}]}]}}}}}");
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
        "            'consideredValue': [1]}]}]}]}");
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
        "            'consideredValue': [1]}]}]}]}");
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
        "            'consideredValue': [1, 2, 3]}]}]}]}");
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
        "       {'a': {'items': {'type': 'string'}}}}}");
    BSONObj document = fromjson("{'a': [1, 'A', {}]}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "  {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "    {'propertyName': 'a', 'details': ["
        "      {'operatorName': 'items', 'reason': 'At least one item did not match the "
        "sub-schema', 'itemIndex': 0, 'details': ["
        "          {'operatorName': 'type', 'specifiedAs': {'type': 'string'}, "
        "'reason': 'type did not match', 'consideredValue': 1, 'consideredType': 'int'}]}]}]}]}");
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
        "            'consideredValue': ['A', 'B']}]}]}]}");
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
        "            {'operatorName': 'minItems', 'specifiedAs': { 'minItems': 2 }, 'reason': "
        "'array did not match specified length', 'consideredValue': [1]}]}]}]}]}]}]}");
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
        "not match', 'consideredValue': 2, 'consideredType': 'int'}]}"
        "]}]}]}]}");
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
        "            'consideredValue': [1, 'A']}]}]}]}");
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
        "            'consideredValue': [1]}]}]}]}");
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
        "            'consideredValue': [1, 'A']}]}]}]}");
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
        "'additionalItems': {'type': 'object'}}}}}");
    BSONObj document = fromjson("{'a': [1, 'First', {}, 'Extra element']}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$jsonSchema', 'schemaRulesNotSatisfied': ["
        "  {'operatorName': 'properties', 'propertiesNotSatisfied': ["
        "    {'propertyName': 'a', 'details': ["
        "      {'operatorName': 'additionalItems', 'reason': 'At least one additional item did not "
        "match the sub-schema', 'itemIndex': 3, 'details': ["
        "          {'operatorName': 'type', 'specifiedAs': {'type': 'object'}, 'reason': 'type did "
        "not match', 'consideredValue': 'Extra element', 'consideredType': 'string'}]}]}]}]}");
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
        "            'consideredValue': [1, 'First', {}]}]}]}]}");
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
        "            'consideredValue': [1]}]}]}]}");
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
    BSONObj query = fromjson("{'$jsonSchema': {'properties': {'a': {'minProperties': 10}}}}}");
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
        "'type': 'object'}}}}}");
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
    BSONObj query = fromjson("{'$jsonSchema': {'properties': {'a': {'maxProperties': 1}}}}}");
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
        "'type': 'object'}}}}}");
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

}  // namespace
}  // namespace mongo
