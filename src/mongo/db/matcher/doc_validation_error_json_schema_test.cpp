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

TEST(JSONSchemaValidation, MinimumAtTopLevelHasNoEffect) {
    BSONObj query = fromjson("{'$nor': [{'$jsonSchema': {'minimum': 1}}]}");
    BSONObj document = fromjson("{a: 2}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$nor',"
        "     'clausesNotSatisfied': [{'index': 0, 'details': "
        "       {'operatorName': '$jsonSchema',"
        "         'schemaRulesNotSatisfied': ["
        "               {'operatorName': 'minimum', "
        "              'specifiedAs': {'minimum': 1}, "
        "               'reason': 'expression always evaluates to true'}]}}]}");
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

TEST(JSONSchemaValidation, MaximumAtTopLevelHasNoEffect) {
    BSONObj query = fromjson("{'$nor': [{'$jsonSchema': {'maximum': 1}}]}");
    BSONObj document = fromjson("{a: 2}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$nor',"
        "     'clausesNotSatisfied': [{'index': 0, 'details': "
        "       {'operatorName': '$jsonSchema',"
        "         'schemaRulesNotSatisfied': ["
        "               {'operatorName': 'maximum', "
        "              'specifiedAs': {'maximum': 1}, "
        "               'reason': 'expression always evaluates to true'}]}}]}");
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
        "{'$jsonSchema': { "
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
        "  {'$jsonSchema':{ 'type': 'number',"
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
        "     'clausesNotSatisfied': [{'index': 0, 'details': "
        "       {'operatorName': '$jsonSchema',"
        "         'schemaRulesNotSatisfied': ["
        "               {'operatorName': 'minLength', "
        "              'specifiedAs': {'minLength': 1}, "
        "               'reason': 'expression always evaluates to true'}]}}]}");
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
        "     'clausesNotSatisfied': [{'index': 0, 'details': "
        "       {'operatorName': '$jsonSchema',"
        "         'schemaRulesNotSatisfied': ["
        "               {'operatorName': 'maxLength', "
        "              'specifiedAs': {'maxLength': 1000}, "
        "               'reason': 'expression always evaluates to true'}]}}]}");
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
        "     'clausesNotSatisfied': [{'index': 0, 'details': "
        "       {'operatorName': '$jsonSchema',"
        "         'schemaRulesNotSatisfied': ["
        "               {'operatorName': 'pattern', "
        "              'specifiedAs': {'pattern': '^S'}, "
        "               'reason': 'expression always evaluates to true'}]}}]}");
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
        "                   {'propertyName': 'a',  details: ["
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
        "     'clausesNotSatisfied': [{'index': 0, 'details': "
        "       {'operatorName': '$jsonSchema',"
        "         'schemaRulesNotSatisfied': ["
        "               {'operatorName': 'multipleOf', "
        "              'specifiedAs': {'multipleOf': 1}, "
        "               'reason': 'expression always evaluates to true'}]}}]}");
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
        "                   {'propertyName': 'a',  details: ["
        "                       {'operatorName': 'encrypt',"
        "                       'reason': 'value was not encrypted'}]}]}]}");
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
        "                   {'propertyName': 'a',  details: ["
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
        "                   {'propertyName': 'a',  details: ["
        "                       {'operatorName': 'encrypt',"
        "                       'reason': 'encrypted value has wrong type'}]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

}  // namespace
}  // namespace mongo
