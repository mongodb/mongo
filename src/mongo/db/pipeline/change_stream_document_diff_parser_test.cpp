// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/pipeline/change_stream_document_diff_parser.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

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
    ASSERT_VALUE_EQ(parsedDiff.removedFields[0], Value("a.b"sv));
    ASSERT_VALUE_EQ(parsedDiff.removedFields[1], Value("c.0"sv));

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
