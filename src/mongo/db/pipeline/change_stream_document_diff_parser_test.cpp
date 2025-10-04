/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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


#include "mongo/db/pipeline/change_stream_document_diff_parser.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(ChangeStreamDocumentDiffParserTest, DisambiguatesDottedFields) {
    BSONObj diff = fromjson(
        "{"
        "   u: {'a.b': 1},"
        "   'sc.d.': {"
        "        u: {'e': 1, 'f.g': 1},"
        "        'sh': {"
        "            u: {'i.j': 1}"
        "        }"
        "    },"
        "    'sk': {"
        "        u: {'l.m': 1}"
        "    }"
        "}");

    auto parsedDiff = change_stream_document_diff_parser::parseDiff(diff);

    ASSERT_DOCUMENT_EQ(
        parsedDiff.updatedFields,
        Document(fromjson("{'a.b': 1, 'c.d..e': 1, 'c.d..f.g': 1, 'c.d..h.i.j': 1, 'k.l.m': 1}")));

    ASSERT_DOCUMENT_EQ(
        parsedDiff.disambiguatedPaths,
        Document(fromjson("{'a.b': ['a.b'], 'c.d..e': ['c.d.', 'e'], 'c.d..f.g': ['c.d.', 'f.g'], "
                          "'c.d..h.i.j': ['c.d.', 'h', 'i.j'], 'k.l.m': ['k', 'l.m']}")));

    ASSERT(parsedDiff.removedFields.empty());
    ASSERT(parsedDiff.truncatedArrays.empty());
}

TEST(ChangeStreamDocumentDiffParserTest, DisambiguatesNumericFields) {
    BSONObj diff = fromjson(
        "{"
        "   'sa': {"
        "        u: {'0': 1}"
        "    }"
        "}");

    auto parsedDiff = change_stream_document_diff_parser::parseDiff(diff);

    ASSERT_DOCUMENT_EQ(parsedDiff.updatedFields, Document(fromjson("{'a.0': 1}")));

    ASSERT_DOCUMENT_EQ(parsedDiff.disambiguatedPaths, Document(fromjson("{'a.0': ['a', '0']}")));

    ASSERT(parsedDiff.removedFields.empty());
    ASSERT(parsedDiff.truncatedArrays.empty());
}

TEST(ChangeStreamDocumentDiffParserTest, DisambiguatesNumericFieldsFromArrayIndices) {
    BSONObj diff = fromjson(
        "{"
        "   'sa': {"
        "        's0': {a: true, u0: 1}"
        "    }"
        "}");

    auto parsedDiff = change_stream_document_diff_parser::parseDiff(diff);

    ASSERT_DOCUMENT_EQ(parsedDiff.updatedFields, Document(fromjson("{'a.0.0': 1}")));

    ASSERT_DOCUMENT_EQ(parsedDiff.disambiguatedPaths,
                       Document(fromjson("{'a.0.0': ['a', '0', 0]}")));

    ASSERT(parsedDiff.removedFields.empty());
    ASSERT(parsedDiff.truncatedArrays.empty());
}

TEST(ChangeStreamDocumentDiffParserTest, DoesNotDisambiguateNumericFieldAtRootOfDocument) {
    BSONObj diff = fromjson(
        "{"
        "   u: {'0': 1}"
        "}");

    auto parsedDiff = change_stream_document_diff_parser::parseDiff(diff);

    ASSERT_DOCUMENT_EQ(parsedDiff.updatedFields, Document(fromjson("{'0': 1}")));

    // A numeric field at the root of the document is unambiguous; it must be a fieldname and cannot
    // be an array index, since by definition the latter must index a parent field.
    ASSERT(parsedDiff.disambiguatedPaths.empty());

    ASSERT(parsedDiff.removedFields.empty());
    ASSERT(parsedDiff.truncatedArrays.empty());
}

TEST(ChangeStreamDocumentDiffParserTest, DoesNotDisambiguateNumericFieldWithLeadingZeroes) {
    BSONObj diff = fromjson(
        "{"
        "   'sa': {u: {'01': 1}}"
        "}");

    auto parsedDiff = change_stream_document_diff_parser::parseDiff(diff);

    ASSERT_DOCUMENT_EQ(parsedDiff.updatedFields, Document(fromjson("{'a.01': 1}")));

    // A numeric field with leading zeroes is unambiguous; it must be a fieldname and cannot be an
    // array index, since array indexes are simple integers that do not have leading zeroes.
    ASSERT(parsedDiff.disambiguatedPaths.empty());

    ASSERT(parsedDiff.removedFields.empty());
    ASSERT(parsedDiff.truncatedArrays.empty());
}

TEST(ChangeStreamDocumentDiffParserTest, DoesNotDisambiguateIfOnlyArrayIndicesPresent) {
    BSONObj diff = fromjson(
        "{"
        "   'sa': {a: true,"
        "        s0: {u: {'b': 1}}"
        "    }"
        "}");

    auto parsedDiff = change_stream_document_diff_parser::parseDiff(diff);

    ASSERT_DOCUMENT_EQ(parsedDiff.updatedFields, Document(fromjson("{'a.0.b': 1}")));

    ASSERT(parsedDiff.disambiguatedPaths.empty());
    ASSERT(parsedDiff.removedFields.empty());
    ASSERT(parsedDiff.truncatedArrays.empty());
}

TEST(ChangeStreamDocumentDiffParserTest, DisambiguatesRemovedFields) {
    BSONObj diff = fromjson(
        "{"
        "   d: {'a.b': false},"
        "   'sc': {"
        "        d: {'0': false}"
        "    }"
        "}");

    auto parsedDiff = change_stream_document_diff_parser::parseDiff(diff);

    ASSERT(parsedDiff.removedFields.size() == 2);
    ASSERT_VALUE_EQ(parsedDiff.removedFields[0], Value("a.b"_sd));
    ASSERT_VALUE_EQ(parsedDiff.removedFields[1], Value("c.0"_sd));

    ASSERT_DOCUMENT_EQ(parsedDiff.disambiguatedPaths,
                       Document(fromjson("{'a.b': ['a.b'], 'c.0': ['c', '0']}")));

    ASSERT(parsedDiff.updatedFields.empty());
    ASSERT(parsedDiff.truncatedArrays.empty());
}

TEST(ChangeStreamDocumentDiffParserTest, DisambiguatesTruncatedArrays) {
    BSONObj diff = fromjson(
        "{"
        "   'sa.b': {a: true, l: 5},"
        "    'sc': {"
        "        's0': {a: true, l: 5}"
        "    }"
        "}");

    auto parsedDiff = change_stream_document_diff_parser::parseDiff(diff);

    ASSERT(parsedDiff.truncatedArrays.size() == 2);
    ASSERT_VALUE_EQ(parsedDiff.truncatedArrays[0], Value(fromjson("{field: 'a.b', newSize: 5}")));
    ASSERT_VALUE_EQ(parsedDiff.truncatedArrays[1], Value(fromjson("{field: 'c.0', newSize: 5}")));

    ASSERT_DOCUMENT_EQ(parsedDiff.disambiguatedPaths,
                       Document(fromjson("{'a.b': ['a.b'], 'c.0': ['c', '0']}")));

    ASSERT(parsedDiff.updatedFields.empty());
    ASSERT(parsedDiff.removedFields.empty());
}

TEST(ChangeStreamDocumentDiffParserTest, DisambiguatesCombinationOfAmbiguousFields) {
    // Array and numeric field within dotted parent, dotted and numeric fields within array, dotted
    // field and array within numeric parent.
    BSONObj diff = fromjson(
        "{"
        "   'sa.b': {a: true,"
        "        's0': {u: {'1': 1}}"
        "    },"
        "    'sc': {a: true,"
        "        's0': {u: {'d.e': 1}},"
        "        's1': {u: {'2': 1}}"
        "    },"
        "    'sf': {"
        "        's1': {"
        "            u: {'g.h': 1},"
        "            's2': {a: true,"
        "                u3: 1,"
        "                s4: {u: {'5': 1}}"
        "            }"
        "        }"
        "    }"
        "}");

    auto parsedDiff = change_stream_document_diff_parser::parseDiff(diff);

    ASSERT_DOCUMENT_EQ(parsedDiff.updatedFields,
                       Document(fromjson("{'a.b.0.1': 1, 'c.0.d.e': 1, 'c.1.2': 1, 'f.1.g.h': 1, "
                                         "'f.1.2.3': 1, 'f.1.2.4.5': 1}")));

    ASSERT_DOCUMENT_EQ(
        parsedDiff.disambiguatedPaths,
        Document(fromjson("{'a.b.0.1': ['a.b', 0, '1'], 'c.0.d.e': ['c', 0, 'd.e'], 'c.1.2': ['c', "
                          "1, '2'], 'f.1.g.h': ['f', '1', 'g.h'], 'f.1.2.3': ['f', '1', '2', 3], "
                          "'f.1.2.4.5': ['f', '1', '2', 4, '5']}")));

    ASSERT(parsedDiff.removedFields.empty());
    ASSERT(parsedDiff.truncatedArrays.empty());
}

TEST(ChangeStreamDocumentDiffParserTest, DoesNotFullyDisambiguateWithDuplicateFieldsInDiff) {
    BSONObj diff = fromjson("{u: {'a.b' : 2}, sa : {u: {b: 1 }}}");
    auto parsedDiff = change_stream_document_diff_parser::parseDiff(diff);

    auto expectedUpdateFields = Document{{"a.b", 2}, {"a.b", 1}};
    ASSERT_DOCUMENT_EQ(parsedDiff.updatedFields, expectedUpdateFields);
    ASSERT_DOCUMENT_EQ(parsedDiff.disambiguatedPaths, Document(fromjson("{'a.b': ['a.b']}")));
    ASSERT(parsedDiff.removedFields.empty());
    ASSERT(parsedDiff.truncatedArrays.empty());
}
}  // namespace
}  // namespace mongo
