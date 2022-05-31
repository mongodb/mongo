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


#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {


TEST(ChangeStreamDocumentDiffParserTest, DottedFieldsInsideArrays) {
    BSONObj diff = fromjson(
        "{"
        "   'sarr.F.i.eld..': {a: true, l: 10,"
        "        u0: 1,"
        "        u1: {'a.b.c': {'a.b': 1}},"
        "        s6: {u: {"
        "            'a.b.d': {'a.b.c': 3},"
        "            'a.b': {d: {'a.b': 1}}"
        "        }}"
        "    }"
        "}");

    auto parsedDiff = change_stream_document_diff_parser::parseDiff(diff);

    ASSERT_DOCUMENT_EQ(parsedDiff.updatedFields,
                       Document(fromjson("{'arr.F.i.eld...0': 1, 'arr.F.i.eld...1': {'a.b.c': "
                                         "{'a.b': 1}}, 'arr.F.i.eld...6.a.b.d': {'a.b.c': 3}, "
                                         "'arr.F.i.eld...6.a.b': {d: {'a.b': 1}}}")));

    ASSERT_DOCUMENT_EQ(
        parsedDiff.dottedFields,
        Document(fromjson("{'arr.F.i.eld...6': ['a.b.d', 'a.b'], '': ['arr.F.i.eld..']}")));

    ASSERT(parsedDiff.removedFields.empty());
    ASSERT_EQ(parsedDiff.truncatedArrays.size(), 1);
    ASSERT_VALUE_EQ(parsedDiff.truncatedArrays[0],
                    Value(fromjson("{field: 'arr.F.i.eld..', newSize: 10}")));

    ASSERT_DOCUMENT_EQ(parsedDiff.arrayIndices, Document(fromjson("{'arr.F.i.eld..': [0, 1, 6]}")));
}

TEST(ChangeStreamDocumentDiffParserTest, DottedFieldsInsideObjects) {
    BSONObj diff = fromjson(
        "{"
        "   'sobject.F.i.eld..': {"
        "        u: {'0.0.0': 1, '1.1.1': {'0.0': 1}},"
        "        s6: {'s7.8': {'s9.10': {"
        "            u: {'a.b.d': {'a.b.c': 3}}"
        "        }}}"
        "    }"
        "}");

    auto parsedDiff = change_stream_document_diff_parser::parseDiff(diff);

    ASSERT_DOCUMENT_EQ(
        parsedDiff.updatedFields,
        Document(fromjson("{'object.F.i.eld...0.0.0': 1, 'object.F.i.eld...1.1.1': {'0.0': 1},"
                          "'object.F.i.eld...6.7.8.9.10.a.b.d': {'a.b.c': 3} }")));

    ASSERT_DOCUMENT_EQ(parsedDiff.dottedFields,
                       Document(fromjson("{'object.F.i.eld...6.7.8.9.10': ['a.b.d'], "
                                         "'object.F.i.eld...6.7.8': ['9.10'], "
                                         "'object.F.i.eld...6': ['7.8'], "
                                         "'object.F.i.eld..': ['0.0.0', '1.1.1'],"
                                         "'': ['object.F.i.eld..']}")));

    ASSERT(parsedDiff.removedFields.empty());
    ASSERT(parsedDiff.truncatedArrays.empty());
    ASSERT(parsedDiff.arrayIndices.empty());
}

TEST(ChangeStreamDocumentDiffParserTest, PathToArrayFields) {
    BSONObj diff = fromjson(
        "{"
        "   'sarr.F.i.eld..': {a: true, l: 10,"
        "        u0: 1,"
        "        s6: {a: true, s1: {"
        "            's0.0': {a: true, u0: 1}"
        "        }}"
        "    }"
        "}");

    auto parsedDiff = change_stream_document_diff_parser::parseDiff(diff);

    ASSERT_DOCUMENT_EQ(parsedDiff.updatedFields,
                       Document(fromjson("{'arr.F.i.eld...0': 1, 'arr.F.i.eld...6.1.0.0.0': 1}")));

    ASSERT_DOCUMENT_EQ(parsedDiff.dottedFields,
                       Document(fromjson("{'arr.F.i.eld...6.1': ['0.0'], '': ['arr.F.i.eld..']}")));

    ASSERT(parsedDiff.removedFields.empty());

    ASSERT_EQ(parsedDiff.truncatedArrays.size(), 1);
    ASSERT_VALUE_EQ(parsedDiff.truncatedArrays[0],
                    Value(fromjson("{field: 'arr.F.i.eld..', newSize: 10}")));

    ASSERT_DOCUMENT_EQ(
        parsedDiff.arrayIndices,
        Document(fromjson(
            "{'arr.F.i.eld...6.1.0.0': [0], 'arr.F.i.eld...6': [1], 'arr.F.i.eld..': [0, 6]}")));
}

TEST(ChangeStreamDocumentDiffParserTest, WithDuplicateFieldsInDiff) {
    BSONObj diff = fromjson("{u: {'a.b' : 2}, sa : {u: {b: 1 }}}");
    auto parsedDiff = change_stream_document_diff_parser::parseDiff(diff);

    auto expectedUpdateFields = Document{{"a.b", 2}, {"a.b", 1}};
    ASSERT_DOCUMENT_EQ(parsedDiff.updatedFields, expectedUpdateFields);
    ASSERT_DOCUMENT_EQ(parsedDiff.dottedFields, Document(fromjson("{'': ['a.b']}")));
    ASSERT(parsedDiff.removedFields.empty());
    ASSERT(parsedDiff.truncatedArrays.empty());
    ASSERT(parsedDiff.arrayIndices.empty());
}
}  // namespace
}  // namespace mongo
