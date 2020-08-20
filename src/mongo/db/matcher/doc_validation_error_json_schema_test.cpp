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

}  // namespace
}  // namespace mongo
