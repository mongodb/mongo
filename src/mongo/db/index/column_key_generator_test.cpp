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

#include "mongo/db/index/column_cell.h"
#include "mongo/platform/basic.h"

#include "mongo/bson/bson_depth.h"
#include "mongo/bson/json.h"
#include "mongo/db/index/column_key_generator.h"
#include "mongo/unittest/unittest.h"

namespace mongo::column_keygen {
namespace {
class CellConverter {
private:
    // It shouldn't matter, but g++8.3 seems to want this at the top of this type.
    struct ValueEncoder {
        using Out = bool;

        template <typename T>
        bool operator()(T&& value) {
            *builder << value;
            return true;
        }

        BSONArrayBuilder* builder;
    };

public:
    UnencodedCellView toUnencodedCellView(const SplitCellView& view) {
        // Fill builder with bson-encoded elements from view.
        auto cursor = view.subcellValuesGenerator(ValueEncoder{&builder});
        while (cursor.nextValue()) {
            // Work done in ValueEncoder::operator() rather than here.
        }

        builder.done().elems(elems);

        return UnencodedCellView{
            elems,
            view.arrInfo,
            view.hasDuplicateFields,
            view.hasSubPaths,
            view.isSparse,
            view.hasDoubleNestedArrays,
        };
    }

private:
    std::vector<BSONElement> elems;
    BSONArrayBuilder builder;
};

enum Flags_ForTest {
    kNoFlags = 0,
    kHasDuplicateFields = 1 << 0,
    kHasSubPath = 1 << 1,
    kIsSparse = 1 << 2,
    kHasDoubleNestedArrays = 1 << 3,
};

struct UnencodedCellValue_ForTest {
    StringData valsJson;  // Comma-separated values
    std::string arrayInfo;
    int flags = kNoFlags;

    UnencodedCellView toView(BSONObj& owner, std::vector<BSONElement>& elems) const {
        owner = fromjson(std::string("{x:[") + valsJson + "]}");
        elems = owner.firstElement().Array();
        return {
            elems,
            arrayInfo,
            bool(flags & kHasDuplicateFields),
            bool(flags & kHasSubPath),
            bool(flags & kIsSparse),
            bool(flags & kHasDoubleNestedArrays),
        };
    }
};

void insertTest(int line,
                const BSONObj& doc,
                const StringMap<UnencodedCellValue_ForTest>& expected) {
    BSONObj owner;
    std::vector<BSONElement> elems;

    StringSet seenPaths;
    visitCellsForInsert(doc, [&](PathView path, const UnencodedCellView& cell) {
        seenPaths.insert(path.toString());

        auto it = expected.find(path);
        if (it == expected.end()) {
            FAIL("Unexpected path in insert") << "test:" << line << " path:" << path;
        }
        auto expectedCell = it->second.toView(owner, elems);
        ASSERT_EQ(cell, expectedCell) << "test:" << line << " path:" << path;

        // Round-trip the cell through encoder/parser and make sure we still agree.
        BufBuilder encoder;
        writeEncodedCell(cell, &encoder);
        auto decoded = SplitCellView::parse({encoder.buf(), size_t(encoder.len())});
        CellConverter converter;
        ASSERT_EQ(converter.toUnencodedCellView(decoded), expectedCell)
            << "test:" << line << " path:" << path;
    });

    for (auto&& [path, _] : expected) {
        if (seenPaths.contains(path))
            continue;

        FAIL("Expected to see path in insert, but didn't") << "test:" << line << " path:" << path;
    }
}

void insertMultiTest(int line,
                     const BSONObj& doc,
                     const StringMap<UnencodedCellValue_ForTest>& expected) {
    // Test with both 1 and 2 records because they use different code paths.
    for (size_t size = 1; size <= 2; size++) {
        BSONObj owner;
        std::vector<BSONElement> elems;

        auto recs = std::vector<BsonRecord>(size);
        for (size_t i = 0; i < size; i++) {
            recs[i].docPtr = &doc;
            recs[i].id = RecordId(i);
            recs[i].ts = Timestamp(i);
        }

        size_t counter = 0;
        auto seenPaths = std::vector<StringSet>(size);
        visitCellsForInsert(
            recs, [&](PathView path, const BsonRecord& rec, const UnencodedCellView& cell) {
                size_t i = counter++ % size;
                ASSERT_EQ(rec.id.getLong(), int64_t(i));
                ASSERT_EQ(rec.ts.asInt64(), int64_t(i));
                ASSERT_EQ(&rec, &recs[i]);

                seenPaths[i].insert(path.toString());

                auto it = expected.find(path);
                if (it == expected.end()) {
                    FAIL("Unexpected path in insert") << "test:" << line << " path:" << path;
                }
                auto expectedCell = it->second.toView(owner, elems);
                ASSERT_EQ(cell, expectedCell) << "test:" << line << " path:" << path;

                // Round-trip the cell through encoder/parser and make sure we still agree.
                BufBuilder encoder;
                writeEncodedCell(cell, &encoder);
                auto decoded = SplitCellView::parse({encoder.buf(), size_t(encoder.len())});
                CellConverter converter;
                ASSERT_EQ(converter.toUnencodedCellView(decoded), expectedCell)
                    << "test:" << line << " path:" << path;
            });

        for (auto&& set : seenPaths) {
            for (auto&& [path, _] : expected) {
                if (set.contains(path))
                    continue;

                FAIL("Expected to see path in insert, but didn't")
                    << "test:" << line << " path:" << path;
            }
        }
    };
}

void deleteTest(int line,
                const BSONObj& doc,
                const StringMap<UnencodedCellValue_ForTest>& expected) {
    StringSet seenPaths;
    visitPathsForDelete(doc, [&](PathView path) {
        seenPaths.insert(path.toString());

        auto it = expected.find(path);
        if (it == expected.end()) {
            FAIL("Unexpected path in delete") << "test:" << line << " path:" << path;
        }
    });

    for (auto&& [path, _] : expected) {
        if (seenPaths.contains(path))
            continue;

        FAIL("Expected to see path in delete, but didn't") << "test:" << line << " path:" << path;
    }
}

void updateToEmptyTest(int line,
                       const BSONObj& doc,
                       const StringMap<UnencodedCellValue_ForTest>& expected) {
    StringSet seenPaths;
    visitDiffForUpdate(
        doc, BSONObj(), [&](DiffAction action, PathView path, const UnencodedCellView* cell) {
            ASSERT_EQ(action, kDelete) << "test:" << line << " path:" << path;

            ASSERT(!seenPaths.contains(path)) << "test:" << line << " path:" << path;
            seenPaths.insert(path.toString());

            auto it = expected.find(path);
            if (it == expected.end()) {
                FAIL("Unexpected path in updateToEmpty") << "action:" << action << " cell:" << cell
                                                         << " test:" << line << " path:" << path;
            }

            ASSERT(!cell) << "test:" << line << " path:" << path;
        });

    for (auto&& [path, _] : expected) {
        if (seenPaths.contains(path))
            continue;

        FAIL("Expected to see path in updateToEmpty, but didn't")
            << "test:" << line << " path:" << path;
    }
}

void updateFromEmptyTest(int line,
                         const BSONObj& doc,
                         const StringMap<UnencodedCellValue_ForTest>& expected) {
    BSONObj owner;
    std::vector<BSONElement> elems;

    StringSet seenPaths;
    visitDiffForUpdate(
        BSONObj(), doc, [&](DiffAction action, PathView path, const UnencodedCellView* cell) {
            ASSERT_EQ(action, kInsert) << "test:" << line << " path:" << path;

            ASSERT(!seenPaths.contains(path)) << "test:" << line << " path:" << path;
            seenPaths.insert(path.toString());

            auto it = expected.find(path);
            if (it == expected.end()) {
                FAIL("Unexpected path in updateFromEmpty")
                    << "action:" << action << " cell:" << cell << " test:" << line
                    << " path:" << path;
            }

            ASSERT(cell) << "test:" << line << " path:" << path;
            auto expectedCell = it->second.toView(owner, elems);
            ASSERT_EQ(*cell, expectedCell) << "test:" << line << " path:" << path;
        });

    for (auto&& [path, _] : expected) {
        if (seenPaths.contains(path))
            continue;

        FAIL("Expected to see path in updateFromEmpty, but didn't")
            << "test:" << line << " path:" << path;
    }
}

void updateWithNoChange(int line, const BSONObj& doc) {
    BSONObj owner;
    std::vector<BSONElement> elems;

    StringSet seenPaths;
    visitDiffForUpdate(
        doc, doc, [&](DiffAction action, PathView path, const UnencodedCellView* cell) {
            FAIL("Unexpected path in updateNoChange")
                << "action:" << action << " cell:" << cell << " test:" << line << " path:" << path;
        });
}


void basicTests(int line, std::string json, StringMap<UnencodedCellValue_ForTest> expected) {
    const BSONObj doc = fromjson(json);

    // Add in the RowID column. Since it is always the same, tests shouldn't include it.
    // We always expect to see it in inserts and deletes, and never in updates.
    expected.insert({ColumnStore::kRowIdPath.toString(), {"", "", kHasSubPath}});

    insertTest(line, doc, expected);
    insertMultiTest(line, doc, expected);
    deleteTest(line, doc, expected);

    expected.erase(ColumnStore::kRowIdPath);
    updateToEmptyTest(line, doc, expected);
    updateFromEmptyTest(line, doc, expected);
    updateWithNoChange(line, doc);
}

TEST(ColKeyGen, BasicTests) {
    basicTests(__LINE__, R"({})", {});
    basicTests(__LINE__,
               R"({a: 1})",
               {
                   {"a", {"1", ""}},
               });
    basicTests(__LINE__,
               R"({a: 1, b:2})",
               {
                   {"a", {"1", ""}},
                   {"b", {"2", ""}},
               });
    basicTests(__LINE__,
               R"({a: [1, 2]})",
               {
                   {"a", {"1, 2", "["}},
               });
    basicTests(__LINE__,
               R"({a: [1, 1]})",  // Identical
               {
                   {"a", {"1, 1", "["}},
               });
    basicTests(__LINE__,
               R"({a: [1, [2]]})",
               {
                   {"a", {"1, 2", "[|[", kHasDoubleNestedArrays}},
               });
    basicTests(__LINE__,
               R"({a: [1, []]})",  // Empty array isn't "double nested" (unless it is...)
               {
                   {"a", {"1, []", "["}},
               });
    basicTests(__LINE__,
               R"({a: [1, [[]]]})",  // ... now it is
               {
                   {"a", {"1, []", "[|[", kHasDoubleNestedArrays}},
               });
    basicTests(__LINE__,
               R"({a: [{b:1}, {b:2}]})",
               {
                   {"a", {"", "[o1", kHasSubPath}},
                   {"a.b", {"1, 2", "["}},
               });
    basicTests(__LINE__,
               R"({a: [{b:1}, {c:2}]})",
               {
                   {"a", {"", "[o1", kHasSubPath}},
                   {"a.b", {"1", "[", kIsSparse}},
                   {"a.c", {"2", "[1", kIsSparse}},
               });
    basicTests(__LINE__,
               R"({a: [{b:1}, {c:[2, 3]}, null]})",
               {
                   {"a", {"null", "[o1", kHasSubPath}},
                   {"a.b", {"1", "[", kIsSparse}},
                   {"a.c", {"2, 3", "[1{[", kIsSparse}},
               });
    basicTests(__LINE__,
               R"({a: [{b:1}, {b:[2, 3]}]})",
               {
                   {"a", {"", "[o1", kHasSubPath}},
                   {"a.b", {"1,2,3", "[|{["}},
               });
    basicTests(__LINE__,
               R"({a: [{b:1}, {b:[2, 3]}, null]})",
               {
                   {"a", {"null", "[o1", kHasSubPath}},
                   {"a.b", {"1,2,3", "[|{[", kIsSparse}},
               });
    basicTests(__LINE__,
               R"({a: [{b:1}, [{b:[2, 3]}], null]})",
               {
                   {"a", {"null", "[o[o]", kHasSubPath}},
                   {"a.b", {"1,2,3", "[|[{[", kIsSparse | kHasDoubleNestedArrays}},
               });
    basicTests(__LINE__,
               R"({a: [[{b: {c: 1}}]]})",
               {
                   {"a", {"", "[[o", kHasSubPath}},
                   {"a.b", {"", "[[o", kHasSubPath | kHasDoubleNestedArrays}},
                   {"a.b.c", {"1", "[[", kHasDoubleNestedArrays}},
               });
    basicTests(__LINE__,
               R"({a: 1, 'dotted.field': 1})",
               {
                   {"a", {"1", ""}},
               });
    basicTests(__LINE__,
               R"({a: 1, b: {'dotted.field': 1}})",
               {
                   {"a", {"1", ""}},
                   {"b", {"", "", kHasSubPath}},
               });
    basicTests(__LINE__,
               R"({a: 1, b: [{'dotted.field': 1}]})",
               {
                   {"a", {"1", ""}},
                   {"b", {"", "[o", kHasSubPath}},
               });
    basicTests(__LINE__,
               R"({a: 1, b: [{'dotted.field': 1, c: 1}]})",
               {
                   {"a", {"1", ""}},
                   {"b", {"", "[o", kHasSubPath}},
                   {"b.c", {"1", "["}},
               });
    basicTests(__LINE__,
               R"({'': 1})",
               {
                   {"", {"1", ""}},
               });
    basicTests(__LINE__,
               R"({'': {'': 1}})",
               {
                   {"", {"", "", kHasSubPath}},
                   {".", {"1", ""}},
               });
    basicTests(__LINE__,
               R"({'': {'': {'': 1}}})",
               {
                   {"", {"", "", kHasSubPath}},
                   {".", {"", "", kHasSubPath}},
                   {"..", {"1", ""}},
               });
    basicTests(__LINE__,
               R"({'': [{'': [{'': [1]}]}]})",
               {
                   {"", {"", "[o", kHasSubPath}},
                   {".", {"", "[{[o", kHasSubPath}},
                   {"..", {"1", "[{[{["}},
               });
    basicTests(__LINE__,
               R"({'a': [1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20]})",
               {
                   {"a", {"1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20", "["}},
               });
    basicTests(__LINE__,
               R"({'a': [[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20], [99]]})",
               {
                   {"a",
                    {
                        "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,99",
                        "[[|19][",
                        kHasDoubleNestedArrays,
                    }},
               });
    basicTests(
        __LINE__,
        R"({'a': [1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20, {b: 1}]})",
        {
            {"a", {"1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20", "[|19o", kHasSubPath}},
            {"a.b", {"1", "[20", kIsSparse}},
        });
    basicTests(
        __LINE__,
        R"({'a': [{b:1}, 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20, {b: 1}]})",
        {
            {"a", {"1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20", "[o|19o", kHasSubPath}},
            {"a.b", {"1,1", "[|+20", kIsSparse}},
        });
    basicTests(__LINE__,
               R"({
                    'a': [
                        {b:1},{b:2},{b:3},{b:4},{b:5},{b:6},{b:7},{b:8},{b:9},{b:10},
                        {b:11},{b:12},{b:13},{b:14},{b:15},{b:16},{b:17},{b:18},{b:19},{b:20}
                    ]
                })",
               {
                   {"a", {"", "[o19", kHasSubPath}},
                   {"a.b", {"1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20", "["}},
               });
}

TEST(ColKeyGen, DeepObjectTests) {
    // Can't use lambdas because they can't be recursive (especially mutually recursive), but I'd
    // still like to keep these local to this test
    struct funcs {
        static void addObjToStr(std::string& str, int depthLeft) {
            if (!depthLeft) {
                str += '1';
                return;
            }

            str += "{x:";
            addObjToStr(str, depthLeft - 1);
            str += "}";
        }

        static void addArrToStr(std::string& str, int depthLeft) {
            if (!depthLeft) {
                str += '1';
                return;
            }

            str += "[";
            addArrToStr(str, depthLeft - 1);
            str += "]";
        }

        static std::string dottedPath(int length) {
            invariant(length >= 1);
            std::string out = "x";
            for (int i = 1; i < length; i++) {
                out += ".x";
            }
            return out;
        }

        static void addAlternatingObjToStr(std::string& str, int depthLeft) {
            if (!depthLeft) {
                str += '1';
                return;
            }

            str += "{x:";
            addAlternatingArrToStr(str, depthLeft - 1);
            str += "}";
        }

        static void addAlternatingArrToStr(std::string& str, int depthLeft) {
            if (!depthLeft) {
                str += '1';
                return;
            }

            str += "[";
            addAlternatingObjToStr(str, depthLeft - 1);
            str += "]";
        }

        static std::string alternatingArrayInfo(int pathDepth) {
            invariant(pathDepth >= 1);
            std::string out = "[";
            for (int i = 1; i < pathDepth; i++) {
                out += "{[";
            }
            return out;
        }
    };

    // The full name is quite the line-full.
    constexpr int kDepth = BSONDepth::kBSONDepthParameterCeiling;

    {  // Just object nesting
        std::string obj;
        funcs::addObjToStr(obj, kDepth);
        StringMap<UnencodedCellValue_ForTest> expected;
        for (int i = 1; i < kDepth; i++) {
            expected[funcs::dottedPath(i)] = {"", "", kHasSubPath};
        }
        expected[funcs::dottedPath(kDepth)] = {"1", ""};
        basicTests(__LINE__, obj, expected);
    }

    {  // Just array nesting
        std::string arr;
        funcs::addArrToStr(arr, kDepth);
        basicTests(__LINE__,
                   "{x: " + arr + "}",
                   {
                       {"x", {"1", std::string(kDepth, '['), kHasDoubleNestedArrays}},
                   });
    }

    // The next two tests cover a mix of object and array nesting, but differ only in which is the
    // innermost. Since the constant was even when this was written, the first tests with an
    // innermost array, and the second tests an innermost object. There may need to be slight
    // adjustments to the tests in the unlikely event that the constant ever becomes odd.
    static_assert(BSONDepth::kBSONDepthParameterCeiling % 2 == 0);  // See above if this fails.

    {  // Innermost array.
        std::string obj;
        funcs::addAlternatingObjToStr(obj, kDepth);
        StringMap<UnencodedCellValue_ForTest> expected;
        constexpr auto kPathLen = kDepth / 2;
        for (int i = 1; i < kPathLen; i++) {
            expected[funcs::dottedPath(i)] = {
                "", funcs::alternatingArrayInfo(i) + 'o', kHasSubPath};
        }
        expected[funcs::dottedPath(kPathLen)] = {"1", funcs::alternatingArrayInfo(kPathLen)};
        basicTests(__LINE__, obj, expected);
    }
    {  // Innermost object.
        std::string obj;
        funcs::addAlternatingObjToStr(obj, kDepth + 1);
        StringMap<UnencodedCellValue_ForTest> expected;
        constexpr auto kPathLen = kDepth / 2 + 1;
        for (int i = 1; i < kPathLen; i++) {
            expected[funcs::dottedPath(i)] = {
                "", funcs::alternatingArrayInfo(i) + 'o', kHasSubPath};
        }
        expected[funcs::dottedPath(kPathLen)] = {"1", funcs::alternatingArrayInfo(kPathLen - 1)};
        basicTests(__LINE__, obj, expected);
    }
}

TEST(ColKeyGen, DuplicateFieldTests) {
    basicTests(__LINE__,
               R"({a: 1, a: 2})",
               {
                   {"a", {"", "", kHasDuplicateFields}},
               });
    basicTests(__LINE__,
               R"({a: [1], a: 2})",
               {
                   {"a", {"", "", kHasDuplicateFields}},
               });
    basicTests(__LINE__,
               R"({a: 1, a: [2]})",
               {
                   {"a", {"", "", kHasDuplicateFields}},
               });
    basicTests(__LINE__,
               R"({a: [1], a: [2]})",
               {
                   {"a", {"", "", kHasDuplicateFields}},
               });
    basicTests(__LINE__,
               R"({a: {b:1}, a: {b:2}})",
               {
                   {"a", {"", "", kHasDuplicateFields}},
                   {"a.b", {"", "", kHasDuplicateFields}},
               });
    basicTests(__LINE__,
               R"({a: {b:[1]}, a: {b:[2]}})",
               {
                   {"a", {"", "", kHasDuplicateFields}},
                   {"a.b", {"", "", kHasDuplicateFields}},
               });
    basicTests(__LINE__,
               R"({a: [{b:[1]}], a: [{b:[2]}]})",
               {
                   {"a", {"", "", kHasDuplicateFields}},
                   {"a.b", {"", "", kHasDuplicateFields}},
               });
    basicTests(__LINE__,
               R"({a: {b:[1]}, a: [{b:[2]}]})",
               {
                   {"a", {"", "", kHasDuplicateFields}},
                   {"a.b", {"", "", kHasDuplicateFields}},
               });
    basicTests(__LINE__,
               R"({a: [{b:[1]}], a: {b:[2]}})",
               {
                   {"a", {"", "", kHasDuplicateFields}},
                   {"a.b", {"", "", kHasDuplicateFields}},
               });
    basicTests(__LINE__,
               R"({a: {b:[1]}, a: [null, {b:[2]}]})",
               {
                   {"a", {"", "", kHasDuplicateFields}},
                   {"a.b", {"", "", kHasDuplicateFields}},
               });
    basicTests(__LINE__,
               R"({a: [null, {b:[1]}], a: {b:[2]}})",
               {
                   {"a", {"", "", kHasDuplicateFields}},
                   {"a.b", {"", "", kHasDuplicateFields}},
               });
    basicTests(__LINE__,
               R"({a: [{b:[1, 3]}], a: [{b:[2]}]})",
               {
                   {"a", {"", "", kHasDuplicateFields}},
                   {"a.b", {"", "", kHasDuplicateFields}},
               });
    basicTests(__LINE__,
               R"({a: [{b:[1, 3]}, null], a: [{b:[2]}]})",
               {
                   {"a", {"", "", kHasDuplicateFields}},
                   {"a.b", {"", "", kHasDuplicateFields}},
               });
    basicTests(__LINE__,
               R"({a: [null, {b:[1, 3]}], a: [{b:[2]}]})",  // No index in second a.b
               {
                   {"a", {"", "", kHasDuplicateFields}},
                   {"a.b", {"", "", kHasDuplicateFields}},
               });
    basicTests(__LINE__,
               R"({a: [null, null, {b:[1, 3]}], a: [null, {b:[2]}]})",  // Index in second a.b
               {
                   {"a", {"", "", kHasDuplicateFields}},
                   {"a.b", {"", "", kHasDuplicateFields}},
               });
    basicTests(__LINE__,
               R"({a: [null, {b:[{c:1}, {c:3}]}], a: [{b:[{c:2}]}]})",
               {
                   {"a", {"", "", kHasDuplicateFields}},
                   {"a.b", {"", "", kHasDuplicateFields}},
                   {"a.b.c", {"", "", kHasDuplicateFields}},
               });
    basicTests(__LINE__,
               R"({a: [null, null, {b:[{c:1}, {c:3}]}], a: [null, {b:[{c:2}]}]})",
               {
                   {"a", {"", "", kHasDuplicateFields}},
                   {"a.b", {"", "", kHasDuplicateFields}},
                   {"a.b.c", {"", "", kHasDuplicateFields}},
               });
    basicTests(__LINE__,
               R"({"": 1, "": 2})",
               {
                   {"", {"", "", kHasDuplicateFields}},
               });

    // This behaves the same as {a:[{b:[1,3]}, {b:2}]} as far as a.b can tell.
    basicTests(__LINE__,
               R"({a: [{b:[1, 3]}], a: [null, {b:[2]}]})",
               {
                   {"a", {"", "", kHasDuplicateFields}},
                   {"a.b", {"1,3,2", "[{[|1]{[", kIsSparse}},
               });

    // These tests only have one a.b path.
    basicTests(__LINE__,
               R"({a: [{b:1}], a: 2})",
               {
                   {"a", {"", "", kHasDuplicateFields}},
                   {"a.b", {"1", "[", kIsSparse}},
               });
    basicTests(__LINE__,
               R"({a: 1, a: [{b:2}]})",
               {
                   {"a", {"", "", kHasDuplicateFields}},
                   {"a.b", {"2", "[", kIsSparse}},
               });
    basicTests(__LINE__,
               R"({a: [{b:1}], a: [2]})",
               {
                   {"a", {"", "", kHasDuplicateFields}},
                   {"a.b", {"1", "[", kIsSparse}},
               });
    basicTests(__LINE__,
               R"({a: [1], a: [{b:2}]})",
               {
                   {"a", {"", "", kHasDuplicateFields}},
                   {"a.b", {"2", "[", kIsSparse}},
               });
}

void updateTest(int line,
                std::string jsonOld,
                std::string jsonNew,
                StringMap<std::pair<DiffAction, UnencodedCellValue_ForTest>> expected) {
    BSONObj owner;
    std::vector<BSONElement> elems;

    StringSet seenPaths;
    visitDiffForUpdate(fromjson(jsonOld),
                       fromjson(jsonNew),
                       [&](DiffAction action, PathView path, const UnencodedCellView* cell) {
                           ASSERT(!seenPaths.contains(path)) << "test:" << line << " path:" << path;
                           seenPaths.insert(path.toString());

                           auto it = expected.find(path);
                           if (it == expected.end()) {
                               FAIL("Unexpected path in updateTest")
                                   << "action:" << action << " cell:" << cell << " test:" << line
                                   << " path:" << path;
                           }

                           auto expectedAction = it->second.first;
                           ASSERT_EQ(action, expectedAction) << "test:" << line << " path:" << path;
                           if (action == kDelete) {
                               ASSERT(!cell) << "test:" << line << " path:" << path;
                           } else {
                               ASSERT(cell) << "test:" << line << " path:" << path;
                               auto expectedCell = it->second.second.toView(owner, elems);
                               ASSERT_EQ(*cell, expectedCell)
                                   << "test:" << line << " path:" << path;
                           }
                       });

    for (auto&& [path, _] : expected) {
        if (seenPaths.contains(path))
            continue;

        FAIL("Expected to see path in updateTest, but didn't")
            << "test:" << line << " path:" << path;
    }
}

TEST(ColKeyGen, UpdateTests) {

    updateTest(__LINE__,
               R"({a: [1, {b: 1}]})",
               R"({a: [1, {b: 2}]})",
               {
                   {"a.b", {kUpdate, {"2", "[1", kIsSparse}}},
               });
    updateTest(__LINE__,
               R"({a: [1, {b: 1}]})",
               R"({a: [2, {b: 1}]})",
               {
                   {"a", {kUpdate, {"2", "[|o", kHasSubPath}}},

               });
    updateTest(__LINE__,
               R"({a: [{b: 1}]})",  // a.b becomes isSparse
               R"({a: [{b: 1}, {c:1}]})",
               {
                   {"a", {kUpdate, {"", "[o1", kHasSubPath}}},
                   {"a.b", {kUpdate, {"1", "[", kIsSparse}}},
                   {"a.c", {kInsert, {"1", "[1", kIsSparse}}},
               });
    updateTest(__LINE__,
               R"({a: [{b: 1}, {c:1}]})",  // a.b becomes not isSparse
               R"({a: [{b: 1}]})",
               {
                   {"a", {kUpdate, {"", "[o", kHasSubPath}}},
                   {"a.b", {kUpdate, {"1", "["}}},
                   {"a.c", {kDelete, {}}},
               });
    updateTest(__LINE__,
               R"({'': 1})",
               R"({'': 2})",
               {
                   {"", {kUpdate, {"2", ""}}},
               });
    updateTest(__LINE__,
               R"({'': [1, {'': 1}]})",
               R"({'': [1, {'': 2}]})",
               {
                   {".", {kUpdate, {"2", "[1", kIsSparse}}},
               });
}

// TODO more tests, of course! In particular, the testing of complex update changes is a bit light.
}  // namespace
}  // namespace mongo::column_keygen
