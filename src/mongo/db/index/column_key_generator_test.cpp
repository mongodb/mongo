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

#include "mongo/bson/bson_depth.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/index/column_cell.h"
#include "mongo/db/index/column_key_generator.h"
#include "mongo/platform/basic.h"
#include "mongo/unittest/unittest.h"
#include <unordered_map>

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
        ValueEncoder encoder{&builder};
        auto cursor = view.subcellValuesGenerator(&encoder);
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

using ProjPairVector = std::vector<std::pair<std::unique_ptr<ColumnKeyGenerator>, StringSet>>;

void insertTest(int line,
                const BSONObj& doc,
                const StringMap<UnencodedCellValue_ForTest>& expected,
                const ColumnKeyGenerator& keyGen) {
    BSONObj owner;
    std::vector<BSONElement> elems;

    StringSet seenPaths;
    keyGen.visitCellsForInsert(doc, [&](PathView path, const UnencodedCellView& cell) {
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
                     const StringMap<UnencodedCellValue_ForTest>& expected,
                     const ColumnKeyGenerator& keyGen) {
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
        keyGen.visitCellsForInsert(
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
                const StringMap<UnencodedCellValue_ForTest>& expected,
                const ColumnKeyGenerator& keyGen) {
    StringSet seenPaths;
    keyGen.visitPathsForDelete(doc, [&](PathView path) {
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
                       const StringMap<UnencodedCellValue_ForTest>& expected,
                       const ColumnKeyGenerator& keyGen) {
    StringSet seenPaths;
    keyGen.visitDiffForUpdate(
        doc,
        BSONObj(),
        [&](ColumnKeyGenerator::DiffAction action, PathView path, const UnencodedCellView* cell) {
            ASSERT_EQ(action, ColumnKeyGenerator::kDelete) << "test:" << line << " path:" << path;

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
                         const StringMap<UnencodedCellValue_ForTest>& expected,
                         const ColumnKeyGenerator& keyGen) {
    BSONObj owner;
    std::vector<BSONElement> elems;

    StringSet seenPaths;
    keyGen.visitDiffForUpdate(
        BSONObj(),
        doc,
        [&](ColumnKeyGenerator::DiffAction action, PathView path, const UnencodedCellView* cell) {
            ASSERT_EQ(action, ColumnKeyGenerator::kInsert) << "test:" << line << " path:" << path;

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

void updateWithNoChange(int line, const BSONObj& doc, const ColumnKeyGenerator& keyGen) {
    BSONObj owner;
    std::vector<BSONElement> elems;

    StringSet seenPaths;
    keyGen.visitDiffForUpdate(
        doc,
        doc,
        [&](ColumnKeyGenerator::DiffAction action, PathView path, const UnencodedCellView* cell) {
            FAIL("Unexpected path in updateNoChange")
                << "action:" << action << " cell:" << cell << " test:" << line << " path:" << path;
        });
}

void basicTests(int line,
                std::string json,
                const StringMap<UnencodedCellValue_ForTest>& pathMap,
                ProjPairVector expected) {
    const BSONObj doc = fromjson(json);
    for (auto&& [keyGen, expectedPaths] : expected) {
        StringMap<UnencodedCellValue_ForTest> expected;
        // Create expected by retrieving flags and vals from expected paths
        for (auto path : expectedPaths) {
            expected.insert({path, pathMap.find(path)->second});
        }
        // Add in the RowID column. Since it is always the same, tests shouldn't include it.
        // We always expect to see it in inserts and deletes, and never in updates.
        expected.insert({ColumnStore::kRowIdPath.toString(), {"", "", kHasSubPath}});

        insertTest(line, doc, expected, *keyGen);
        insertMultiTest(line, doc, expected, *keyGen);
        deleteTest(line, doc, expected, *keyGen);

        expected.erase(ColumnStore::kRowIdPath);
        updateToEmptyTest(line, doc, expected, *keyGen);
        updateFromEmptyTest(line, doc, expected, *keyGen);
        updateWithNoChange(line, doc, *keyGen);
    }
}

std::unique_ptr<ColumnKeyGenerator> makeKeyGen(BSONObj columnstoreProjection = BSONObj(),
                                               BSONObj keyPattern = BSON("$**"
                                                                         << "columnstore")) {
    return std::make_unique<ColumnKeyGenerator>(keyPattern, columnstoreProjection);
}

ProjPairVector expectedProjPairs(std::vector<std::pair<BSONObj, StringSet>> bsonPairs) {
    ProjPairVector projPairs;
    for (auto [projection, fields] : bsonPairs) {
        projPairs.push_back({makeKeyGen(projection), fields});
    }
    return projPairs;
}

TEST(ColKeyGen, BasicTests) {
    basicTests(__LINE__, R"({})", {}, expectedProjPairs({{{}, {}}, {BSON("a" << true), {}}}));
    basicTests(
        __LINE__,
        R"({a: 1})",
        {
            {"a", {"1", ""}},
        },
        expectedProjPairs({{{}, {"a"}}, {BSON("a" << true), {"a"}}, {BSON("b" << true), {}}}));
    basicTests(__LINE__,
               R"({a: 1, b:2})",
               {
                   {"a", {"1", ""}},
                   {"b", {"2", ""}},
               },
               expectedProjPairs({{{}, {"a", "b"}},
                                  {BSON("a" << true << "b" << true), {"a", "b"}},
                                  {BSON("b" << true), {"b"}}}));
    basicTests(__LINE__,
               R"({a: [1, 2]})",
               {
                   {"a", {"1, 2", "["}},
               },
               expectedProjPairs({{{}, {"a"}}, {BSON("a" << false), {}}}));
    basicTests(__LINE__,
               R"({a: [1, 1]})",  // Identical
               {
                   {"a", {"1, 1", "["}},
               },
               expectedProjPairs({{{}, {"a"}}, {BSON("a.b" << true), {"a"}}}));
    basicTests(__LINE__,
               R"({a: [1, [2]]})",
               {
                   {"a", {"1, 2", "[|[", kHasDoubleNestedArrays}},
               },
               expectedProjPairs({{{}, {"a"}}, {BSON("a" << true), {"a"}}}));
    basicTests(__LINE__,
               R"({a: [1, []]})",  // Empty array isn't "double nested" (unless it is...)
               {
                   {"a", {"1, []", "["}},
               },
               expectedProjPairs({{{}, {"a"}}, {BSON("r" << false), {"a"}}}));
    basicTests(__LINE__,
               R"({a: [1, [[]]]})",  // ... now it is
               {
                   {"a", {"1, []", "[|[", kHasDoubleNestedArrays}},
               },
               expectedProjPairs({{{}, {"a"}}, {BSON("a" << true), {"a"}}}));
    basicTests(__LINE__,
               R"({a: [{b:1}, {b:2}]})",
               {
                   {"a", {"", "[o1", kHasSubPath}},
                   {"a.b", {"1, 2", "["}},
               },
               expectedProjPairs({{{}, {"a", "a.b"}},
                                  {BSON("a" << true), {"a", "a.b"}},
                                  {BSON("a.b" << false), {"a"}}}));
    basicTests(__LINE__,
               R"({a: [{b:1}, {c:2}]})",
               {
                   {"a", {"", "[o1", kHasSubPath}},
                   {"a.b", {"1", "[", kIsSparse}},
                   {"a.c", {"2", "[1", kIsSparse}},
               },
               expectedProjPairs({{{}, {"a", "a.b", "a.c"}},
                                  {BSON("a" << false), {}},
                                  {BSON("a" << true), {"a", "a.b", "a.c"}},
                                  {BSON("a.c" << false), {"a", "a.b"}}}));
    basicTests(__LINE__,
               R"({a: [{b:1}, {c:[2, 3]}, null]})",
               {
                   {"a", {"null", "[o1", kHasSubPath}},
                   {"a.b", {"1", "[", kIsSparse}},
                   {"a.c", {"2, 3", "[1{[", kIsSparse}},
               },
               expectedProjPairs({{{}, {"a", "a.b", "a.c"}},
                                  {BSON("a" << false), {}},
                                  {BSON("a" << true), {"a", "a.b", "a.c"}},
                                  {BSON("a.b" << false), {"a", "a.c"}}}));
    basicTests(__LINE__,
               R"({a: [{b:1}, {b:[2, 3]}]})",
               {
                   {"a", {"", "[o1", kHasSubPath}},
                   {"a.b", {"1,2,3", "[|{["}},
               },
               expectedProjPairs({{{}, {"a", "a.b"}},
                                  {BSON("c" << false), {"a", "a.b"}},
                                  {BSON("a.b" << true), {"a", "a.b"}},
                                  {BSON("a.b" << false), {"a"}}}));
    basicTests(__LINE__,
               R"({a: [{b:1}, {b:[2, 3]}, null]})",
               {
                   {"a", {"null", "[o1", kHasSubPath}},
                   {"a.b", {"1,2,3", "[|{[", kIsSparse}},
               },
               expectedProjPairs({{{}, {"a", "a.b"}},
                                  {BSON("a" << false), {}},
                                  {BSON("a" << true), {"a", "a.b"}},
                                  {BSON("x.y" << false), {"a", "a.b"}}}));
    basicTests(__LINE__,
               R"({a: [{b:1}, [{b:[2, 3]}], null]})",
               {
                   {"a", {"null", "[o[o]", kHasSubPath}},
                   {"a.b", {"1,2,3", "[|[{[", kIsSparse | kHasDoubleNestedArrays}},
               },
               expectedProjPairs({{{}, {"a", "a.b"}},
                                  {BSON("a.b" << true), {"a", "a.b"}},
                                  {BSON("b" << true), {}},
                                  {BSON("a" << false), {}}}));
    basicTests(__LINE__,
               R"({a: [[{b: {c: 1}}]]})",
               {
                   {"a", {"", "[[o", kHasSubPath}},
                   {"a.b", {"", "[[o", kHasSubPath | kHasDoubleNestedArrays}},
                   {"a.b.c", {"1", "[[", kHasDoubleNestedArrays}},
               },
               expectedProjPairs({{{}, {"a", "a.b", "a.b.c"}},
                                  {BSON("a.b" << true), {"a", "a.b", "a.b.c"}},
                                  {BSON("a.b.c" << true), {"a", "a.b", "a.b.c"}},
                                  {BSON("a.c" << false), {"a", "a.b", "a.b.c"}}}));
    basicTests(__LINE__,
               R"({a: 1, 'dotted.field': 1})",
               {
                   {"a", {"1", ""}},
               },
               expectedProjPairs({{{}, {"a"}},
                                  {BSON("a" << false), {}},
                                  {BSON("a" << true), {"a"}},
                                  {BSON("a.c" << false), {"a"}}}));
    basicTests(__LINE__,
               R"({a: 1, b: {'dotted.field': 1}})",
               {
                   {"a", {"1", ""}},
                   {"b", {"", "", kHasSubPath}},
               },
               expectedProjPairs({{{}, {"a", "b"}},
                                  {BSON("a" << false), {"b"}},
                                  {BSON("a" << true), {"a"}},
                                  {BSON("a.c" << false), {"a", "b"}}}));
    basicTests(__LINE__,
               R"({a: 1, b: [{'dotted.field': 1}]})",
               {
                   {"a", {"1", ""}},
                   {"b", {"", "[o", kHasSubPath}},
               },
               expectedProjPairs({{{}, {"a", "b"}},
                                  {BSON("a" << true), {"a"}},
                                  {BSON("a.b" << true), {"a"}},
                                  {BSON("a" << false), {"b"}}}));
    basicTests(__LINE__,
               R"({a: 1, b: [{'dotted.field': 1, c: 1}]})",
               {
                   {"a", {"1", ""}},
                   {"b", {"", "[o", kHasSubPath}},
                   {"b.c", {"1", "["}},
               },
               expectedProjPairs({{{}, {"a", "b", "b.c"}},
                                  {BSON("a" << false), {"b", "b.c"}},
                                  {BSON("a" << true), {"a"}},
                                  {BSON("b" << false), {"a"}}}));
    basicTests(__LINE__,
               R"({'': 1})",
               {
                   {"", {"1", ""}},
               },
               expectedProjPairs({{{}, {""}},
                                  {BSON("a" << false), {""}},
                                  {BSON("a" << true), {}},
                                  {BSON("a.b.c" << true), {}}}));
    basicTests(__LINE__,
               R"({'': {'': 1}})",
               {
                   {"", {"", "", kHasSubPath}},
                   {".", {"1", ""}},
               },
               expectedProjPairs({{{}, {"", ".", ""}}}));
    basicTests(__LINE__,
               R"({'': {'': {'': 1}}})",

               {
                   {"", {"", "", kHasSubPath}},
                   {".", {"", "", kHasSubPath}},
                   {"..", {"1", ""}},
               },
               expectedProjPairs({{{}, {"", ".", ".."}}}));
    basicTests(__LINE__,
               R"({'': [{'': [{'': [1]}]}]})",
               {
                   {"", {"", "[o", kHasSubPath}},
                   {".", {"", "[{[o", kHasSubPath}},
                   {"..", {"1", "[{[{["}},
               },
               expectedProjPairs({{{}, {"", ".", ".."}}}));
    basicTests(__LINE__,
               R"({'a': [1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20]})",
               {
                   {"a", {"1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20", "["}},
               },
               expectedProjPairs({{{}, {"a"}},
                                  {BSON("a" << false), {}},
                                  {BSON("a" << true), {"a"}},
                                  {BSON("a.b" << false), {"a"}}}));
    basicTests(__LINE__,
               R"({'a': [[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20], [99]]})",
               {
                   {"a",
                    {
                        "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,99",
                        "[[|19][",
                        kHasDoubleNestedArrays,
                    }},
               },
               expectedProjPairs({{{}, {"a"}},
                                  {BSON("a" << true), {"a"}},
                                  {BSON("a.b" << true), {"a"}},
                                  {BSON("b.c" << true), {}}}));
    basicTests(
        __LINE__,
        R"({'a': [1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20, {b: 1}]})",

        {
            {"a", {"1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20", "[|19o", kHasSubPath}},
            {"a.b", {"1", "[20", kIsSparse}},
        },
        expectedProjPairs({{{}, {"a", "a.b"}},
                           {BSON("a" << false), {}},
                           {BSON("c" << true), {}},
                           {BSON("a.c" << false), {"a", "a.b"}}}));
    basicTests(
        __LINE__,
        R"({'a': [{b:1}, 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20, {b: 1}]})",
        {
            {"a", {"1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20", "[o|19o", kHasSubPath}},
            {"a.b", {"1,1", "[|+20", kIsSparse}},
        },
        expectedProjPairs({{{}, {"a", "a.b"}},
                           {BSON("a" << true), {"a", "a.b"}},
                           {BSON("a" << true), {"a", "a.b"}},
                           {BSON("x.y" << false), {"a", "a.b"}}}));
    basicTests(__LINE__,
               R"({'a':
     [{b:1},{b:2},{b:3},{b:4},{b:5},{b:6},{b:7},{b:8},{b:9},{b:10},{b:11},{b:12},{b:13},{b:14},{b:15},{b:16},{b:17},{b:18},{b:19},{b:20}]})",
               {
                   {"a", {"", "[o19", kHasSubPath}},
                   {"a.b", {"1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20", "["}},
               },
               expectedProjPairs({{{}, {"a", "a.b"}},
                                  {BSON("a" << true), {"a", "a.b"}},
                                  {BSON("a" << true), {"a", "a.b"}},
                                  {BSON("x.y" << false), {"a", "a.b"}}}));
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

            str += "{a:";
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
            std::string out = "a";
            for (int i = 1; i < length; i++) {
                out += ".a";
            }
            return out;
        }

        static void addAlternatingObjToStr(std::string& str, int depthLeft) {
            if (!depthLeft) {
                str += '1';
                return;
            }

            str += "{a:";
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
        StringSet expectedPaths;
        for (int i = 1; i < kDepth; i++) {
            expected[funcs::dottedPath(i)] = {"", "", kHasSubPath};
            expectedPaths.insert(funcs::dottedPath(i));
        }
        expected[funcs::dottedPath(kDepth)] = {"1", ""};
        expectedPaths.insert(funcs::dottedPath(kDepth));
        basicTests(__LINE__, obj, expected, expectedProjPairs({{{}, expectedPaths}}));
    }

    {  // Just array nesting
        std::string arr;
        funcs::addArrToStr(arr, kDepth);
        basicTests(__LINE__,
                   "{a: " + arr + "}",
                   {
                       {"a", {"1", std::string(kDepth, '['), kHasDoubleNestedArrays}},
                   },
                   expectedProjPairs({{{}, {"a"}}}));
    }

    // The next two tests cover a mix of object and array nesting, but differ only in which is
    // the innermost. Since the constant was even when this was written, the first tests with an
    // innermost array, and the second tests an innermost object. There may need to be slight
    // adjustments to the tests in the unlikely event that the constant ever becomes odd.
    static_assert(BSONDepth::kBSONDepthParameterCeiling % 2 == 0);  // See above if this fails.

    {  // Innermost array.
        std::string obj;
        funcs::addAlternatingObjToStr(obj, kDepth);
        StringMap<UnencodedCellValue_ForTest> expected;
        StringSet expectedPaths;
        constexpr auto kPathLen = kDepth / 2;
        for (int i = 1; i < kPathLen; i++) {
            expected[funcs::dottedPath(i)] = {
                "", funcs::alternatingArrayInfo(i) + 'o', kHasSubPath};
            expectedPaths.insert(funcs::dottedPath(i));
        }
        expected[funcs::dottedPath(kPathLen)] = {"1", funcs::alternatingArrayInfo(kPathLen)};
        expectedPaths.insert(funcs::dottedPath(kPathLen));
        basicTests(__LINE__, obj, expected, expectedProjPairs({{{}, expectedPaths}}));
    }

    {  // Innermost object.
        std::string obj;
        funcs::addAlternatingObjToStr(obj, kDepth + 1);
        StringMap<UnencodedCellValue_ForTest> expected;
        StringSet expectedPaths;
        constexpr auto kPathLen = kDepth / 2 + 1;
        for (int i = 1; i < kPathLen; i++) {
            expected[funcs::dottedPath(i)] = {
                "", funcs::alternatingArrayInfo(i) + 'o', kHasSubPath};
            expectedPaths.insert(funcs::dottedPath(i));
        }
        expected[funcs::dottedPath(kPathLen)] = {"1", funcs::alternatingArrayInfo(kPathLen - 1)};
        expectedPaths.insert(funcs::dottedPath(kPathLen));
        basicTests(__LINE__, obj, expected, expectedProjPairs({{{}, expectedPaths}}));
    }
}

TEST(ColKeyGen, DuplicateFieldTests) {
    basicTests(
        __LINE__,
        R"({a: 1, a: 2})",
        {
            {"a", {"", "", kHasDuplicateFields}},
        },
        expectedProjPairs({{{}, {"a"}}, {BSON("a" << true), {"a"}}, {BSON("b" << true), {}}}));
    basicTests(
        __LINE__,
        R"({a: [1], a: 2})",
        {
            {"a", {"", "", kHasDuplicateFields}},
        },
        expectedProjPairs({{{}, {"a"}}, {BSON("a" << true), {"a"}}, {BSON("b" << true), {}}}));
    basicTests(
        __LINE__,
        R"({a: 1, a: [2]})",
        {
            {"a", {"", "", kHasDuplicateFields}},
        },
        expectedProjPairs({{{}, {"a"}}, {BSON("a" << true), {"a"}}, {BSON("b" << true), {}}}));
    basicTests(
        __LINE__,
        R"({a: [1], a: [2]})",
        {
            {"a", {"", "", kHasDuplicateFields}},
        },
        expectedProjPairs({{{}, {"a"}}, {BSON("a" << true), {"a"}}, {BSON("b" << true), {}}}));
    basicTests(__LINE__,
               R"({a: {b:1}, a: {b:2}})",
               {
                   {"a", {"", "", kHasDuplicateFields}},
                   {"a.b", {"", "", kHasDuplicateFields}},
               },
               expectedProjPairs({{{}, {"a", "a.b"}},
                                  {BSON("c" << false), {"a", "a.b"}},
                                  {BSON("a.b" << true), {"a", "a.b"}},
                                  {BSON("a.b" << false), {"a"}}}));
    basicTests(__LINE__,
               R"({a: {b:[1]}, a: {b:[2]}})",
               {
                   {"a", {"", "", kHasDuplicateFields}},
                   {"a.b", {"", "", kHasDuplicateFields}},
               },
               expectedProjPairs({{{}, {"a", "a.b"}},
                                  {BSON("c" << false), {"a", "a.b"}},
                                  {BSON("a.b" << true), {"a", "a.b"}},
                                  {BSON("a.b" << false), {"a"}}}));
    basicTests(__LINE__,
               R"({a: [{b:[1]}], a: [{b:[2]}]})",
               {
                   {"a", {"", "", kHasDuplicateFields}},
                   {"a.b", {"", "", kHasDuplicateFields}},
               },
               expectedProjPairs({{{}, {"a", "a.b"}},
                                  {BSON("c" << false), {"a", "a.b"}},
                                  {BSON("a.b" << true), {"a", "a.b"}},
                                  {BSON("a.b" << false), {"a"}}}));
    basicTests(__LINE__,
               R"({a: {b:[1]}, a: [{b:[2]}]})",
               {
                   {"a", {"", "", kHasDuplicateFields}},
                   {"a.b", {"", "", kHasDuplicateFields}},
               },
               expectedProjPairs({{{}, {"a", "a.b"}},
                                  {BSON("c" << false), {"a", "a.b"}},
                                  {BSON("a.b" << true), {"a", "a.b"}},
                                  {BSON("a.b" << false), {"a"}}}));
    basicTests(__LINE__,
               R"({a: [{b:[1]}], a: {b:[2]}})",
               {
                   {"a", {"", "", kHasDuplicateFields}},
                   {"a.b", {"", "", kHasDuplicateFields}},
               },
               expectedProjPairs({{{}, {"a", "a.b"}},
                                  {BSON("c" << false), {"a", "a.b"}},
                                  {BSON("a.b" << true), {"a", "a.b"}},
                                  {BSON("a.b" << false), {"a"}}}));
    basicTests(__LINE__,
               R"({a: {b:[1]}, a: [null, {b:[2]}]})",

               {
                   {"a", {"", "", kHasDuplicateFields}},
                   {"a.b", {"", "", kHasDuplicateFields}},
               },
               expectedProjPairs({{{}, {"a", "a.b"}},
                                  {BSON("c" << false), {"a", "a.b"}},
                                  {BSON("a.b" << true), {"a", "a.b"}},
                                  {BSON("a.b" << false), {"a"}}}));
    basicTests(__LINE__,
               R"({a: [null, {b:[1]}], a: {b:[2]}})",
               {
                   {"a", {"", "", kHasDuplicateFields}},
                   {"a.b", {"", "", kHasDuplicateFields}},
               },
               expectedProjPairs({{{}, {"a", "a.b"}},
                                  {BSON("c" << false), {"a", "a.b"}},
                                  {BSON("a.b" << true), {"a", "a.b"}},
                                  {BSON("a.b" << false), {"a"}}}));
    basicTests(__LINE__,
               R"({a: [{b:[1, 3]}], a: [{b:[2]}]})",
               {
                   {"a", {"", "", kHasDuplicateFields}},
                   {"a.b", {"", "", kHasDuplicateFields}},
               },
               expectedProjPairs({{{}, {"a", "a.b"}},
                                  {BSON("c" << false), {"a", "a.b"}},
                                  {BSON("a.b" << true), {"a", "a.b"}},
                                  {BSON("a.b" << false), {"a"}}}));
    basicTests(__LINE__,
               R"({a: [{b:[1, 3]}, null], a: [{b:[2]}]})",
               {
                   {"a", {"", "", kHasDuplicateFields}},
                   {"a.b", {"", "", kHasDuplicateFields}},
               },
               expectedProjPairs({{{}, {"a", "a.b"}},
                                  {BSON("c" << false), {"a", "a.b"}},
                                  {BSON("a.b" << true), {"a", "a.b"}},
                                  {BSON("a.b" << false), {"a"}}}));
    basicTests(__LINE__,
               R"({a: [null, {b:[1, 3]}], a: [{b:[2]}]})",  // No index in second a.b
               {
                   {"a", {"", "", kHasDuplicateFields}},
                   {"a.b", {"", "", kHasDuplicateFields}},
               },
               expectedProjPairs({{{}, {"a", "a.b"}},
                                  {BSON("c" << false), {"a", "a.b"}},
                                  {BSON("a.b" << true), {"a", "a.b"}},
                                  {BSON("a.b" << false), {"a"}}}));
    basicTests(__LINE__,
               R"({a: [null, null, {b:[1, 3]}], a: [null, {b:[2]}]})",  // Index in second a.b
               {
                   {"a", {"", "", kHasDuplicateFields}},
                   {"a.b", {"", "", kHasDuplicateFields}},
               },
               expectedProjPairs({{{}, {"a", "a.b"}},
                                  {BSON("c" << false), {"a", "a.b"}},
                                  {BSON("a.b" << true), {"a", "a.b"}},
                                  {BSON("a.b" << false), {"a"}}}));
    basicTests(__LINE__,
               R"({a: [null, {b:[{c:1}, {c:3}]}], a: [{b:[{c:2}]}]})",
               {
                   {"a", {"", "", kHasDuplicateFields}},
                   {"a.b", {"", "", kHasDuplicateFields}},
                   {"a.b.c", {"", "", kHasDuplicateFields}},
               },
               expectedProjPairs({{{}, {"a", "a.b", "a.b.c"}},
                                  {BSON("a.b" << true), {"a", "a.b", "a.b.c"}},
                                  {BSON("a.b.c" << true), {"a", "a.b", "a.b.c"}},
                                  {BSON("a.c" << false), {"a", "a.b", "a.b.c"}}}));
    basicTests(__LINE__,
               R"({a: [null, null, {b:[{c:1}, {c:3}]}], a: [null, {b:[{c:2}]}]})",
               {
                   {"a", {"", "", kHasDuplicateFields}},
                   {"a.b", {"", "", kHasDuplicateFields}},
                   {"a.b.c", {"", "", kHasDuplicateFields}},
               },
               expectedProjPairs({{{}, {"a", "a.b", "a.b.c"}},
                                  {BSON("a.b" << true), {"a", "a.b", "a.b.c"}},
                                  {BSON("a.b.c" << true), {"a", "a.b", "a.b.c"}},
                                  {BSON("a.c" << false), {"a", "a.b", "a.b.c"}}}));
    basicTests(__LINE__,
               R"({"": 1, "": 2})",
               {
                   {"", {"", "", kHasDuplicateFields}},
               },
               expectedProjPairs({{{}, {""}}}));
    // This behaves the same as {a:[{b:[1,3]}, {b:2}]} as far as a.b can tell.
    basicTests(__LINE__,
               R"({a: [{b:[1, 3]}], a: [null, {b:[2]}]})",
               {
                   {"a", {"", "", kHasDuplicateFields}},
                   {"a.b", {"1,3,2", "[{[|1]{[", kIsSparse}},
               },
               expectedProjPairs({{{}, {"a", "a.b"}},
                                  {BSON("c" << false), {"a", "a.b"}},
                                  {BSON("a.b" << true), {"a", "a.b"}},
                                  {BSON("a.b" << false), {"a"}}}));
    // These tests only have one a.b path.
    basicTests(__LINE__,
               R"({a: [{b:1}], a: 2})",
               {
                   {"a", {"", "", kHasDuplicateFields}},
                   {"a.b", {"1", "[", kIsSparse}},
               },
               expectedProjPairs({{{}, {"a", "a.b"}},
                                  {BSON("c" << false), {"a", "a.b"}},
                                  {BSON("a.b" << true), {"a", "a.b"}},
                                  {BSON("a.b" << false), {"a"}}}));
    basicTests(__LINE__,
               R"({a: 1, a: [{b:2}]})",
               {
                   {"a", {"", "", kHasDuplicateFields}},
                   {"a.b", {"2", "[", kIsSparse}},
               },
               expectedProjPairs({{{}, {"a", "a.b"}},
                                  {BSON("c" << false), {"a", "a.b"}},
                                  {BSON("a.b" << true), {"a", "a.b"}},
                                  {BSON("a.b" << false), {"a"}}}));
    basicTests(__LINE__,
               R"({a: [{b:1}], a: [2]})",
               {
                   {"a", {"", "", kHasDuplicateFields}},
                   {"a.b", {"1", "[", kIsSparse}},
               },
               expectedProjPairs({{{}, {"a", "a.b"}},
                                  {BSON("c" << false), {"a", "a.b"}},
                                  {BSON("a.b" << true), {"a", "a.b"}},
                                  {BSON("a.b" << false), {"a"}}}));
    basicTests(__LINE__,
               R"({a: [1], a: [{b:2}]})",
               {
                   {"a", {"", "", kHasDuplicateFields}},
                   {"a.b", {"2", "[", kIsSparse}},
               },
               expectedProjPairs({{{}, {"a", "a.b"}},
                                  {BSON("c" << false), {"a", "a.b"}},
                                  {BSON("a.b" << true), {"a", "a.b"}},
                                  {BSON("a.b" << false), {"a"}}}));
}

void updateTest(
    int line,
    std::string jsonOld,
    std::string jsonNew,
    StringMap<std::pair<ColumnKeyGenerator::DiffAction, UnencodedCellValue_ForTest>> pathMap,
    ProjPairVector projPairs) {
    BSONObj owner;
    std::vector<BSONElement> elems;
    for (auto&& [keyGen, expectedPaths] : projPairs) {
        StringMap<std::pair<ColumnKeyGenerator::DiffAction, UnencodedCellValue_ForTest>> expected;
        for (auto path : expectedPaths) {
            expected.insert({path, pathMap.find(path)->second});
        }
        StringSet seenPaths;
        keyGen->visitDiffForUpdate(
            fromjson(jsonOld),
            fromjson(jsonNew),
            [&](ColumnKeyGenerator::DiffAction action,
                PathView path,
                const UnencodedCellView* cell) {
                ASSERT(!seenPaths.contains(path)) << "test:" << line << " path:" << path;
                seenPaths.insert(path.toString());

                auto it = expected.find(path);
                if (it == expected.end()) {
                    FAIL("Unexpected path in updateTest") << "action:" << action << " cell:" << cell
                                                          << " test:" << line << " path:" << path;
                }

                auto expectedAction = it->second.first;
                ASSERT_EQ(action, expectedAction) << "test:" << line << " path:" << path;
                if (action == ColumnKeyGenerator::kDelete) {
                    ASSERT(!cell) << "test:" << line << " path:" << path;
                } else {
                    ASSERT(cell) << "test:" << line << " path:" << path;
                    auto expectedCell = it->second.second.toView(owner, elems);
                    ASSERT_EQ(*cell, expectedCell) << "test:" << line << " path:" << path;
                }
            });

        for (auto&& [path, _] : expected) {
            if (seenPaths.contains(path))
                continue;

            FAIL("Expected to see path in updateTest, but didn't")
                << "test:" << line << " path:" << path;
        }
    }
}

TEST(ColKeyGen, UpdateTests) {
    updateTest(__LINE__,
               R"({a: [1, {b: 1}]})",
               R"({a: [1, {b: 2}]})",
               {
                   {"a.b", {ColumnKeyGenerator::kUpdate, {"2", "[1", kIsSparse}}},
               },
               expectedProjPairs({{{}, {"a.b"}}}));
    updateTest(__LINE__,
               R"({a: [1, {b: 1}]})",
               R"({a: [2, {b: 1}]})",
               {
                   {"a", {ColumnKeyGenerator::kUpdate, {"2", "[|o", kHasSubPath}}},
               },
               expectedProjPairs({{{}, {"a"}}}));
    updateTest(__LINE__,
               R"({a: [{b: 1}]})",  // a.b becomes isSparse
               R"({a: [{b: 1}, {c:1}]})",
               {
                   {"a", {ColumnKeyGenerator::kUpdate, {"", "[o1", kHasSubPath}}},
                   {"a.b", {ColumnKeyGenerator::kUpdate, {"1", "[", kIsSparse}}},
                   {"a.c", {ColumnKeyGenerator::kInsert, {"1", "[1", kIsSparse}}},
               },
               expectedProjPairs({{{}, {"a", "a.b", "a.c"}}}));
    updateTest(__LINE__,
               R"({a: [{b: 1}, {c:1}]})",  // a.b becomes not isSparse
               R"({a: [{b: 1}]})",
               {
                   {"a", {ColumnKeyGenerator::kUpdate, {"", "[o", kHasSubPath}}},
                   {"a.b", {ColumnKeyGenerator::kUpdate, {"1", "["}}},
                   {"a.c", {ColumnKeyGenerator::kDelete, {}}},
               },
               expectedProjPairs({{{}, {"a", "a.b", "a.c"}}}));
    updateTest(__LINE__,
               R"({'': 1})",
               R"({'': 2})",
               {
                   {"", {ColumnKeyGenerator::kUpdate, {"2", ""}}},
               },
               expectedProjPairs({{{}, {""}}}));
    updateTest(__LINE__,
               R"({'': [1, {'': 1}]})",
               R"({'': [1, {'': 2}]})",
               {
                   {".", {ColumnKeyGenerator::kUpdate, {"2", "[1", kIsSparse}}},
               },
               expectedProjPairs({{{}, {"."}}}));
}

TEST(ColKeyGen, InclusionExclusionProjection) {
    basicTests(__LINE__,
               R"({a: {b:1}, c: {d: 2}})",
               {{"a", {"", "", kHasSubPath}},
                {"a.b", {"1", ""}},
                {"c", {"", "", kHasSubPath}},
                {"c.d", {"2", ""}}},
               expectedProjPairs({{{}, {"a", "a.b", "c", "c.d"}},
                                  {BSON("a" << true), {"a", "a.b"}},
                                  {BSON("c" << false), {"a", "a.b"}},
                                  {BSON("c.d" << false), {"a", "a.b", "c"}}}));
    basicTests(__LINE__,
               R"({a: [{b:[1]}, {c:[2]}], c: {d: {e: 2}}})",
               {{"a", {"", "[o1", kHasSubPath}},
                {"a.b", {"1", "[{[", kIsSparse}},
                {"a.c", {"2", "[1{[", kIsSparse}},
                {"c", {"", "", kHasSubPath}},
                {"c.d", {"", "", kHasSubPath}},
                {"c.d.e", {"2", ""}}},
               expectedProjPairs({{{}, {"a", "a.b", "a.c", "c", "c.d", "c.d.e"}},
                                  {BSON("a.b" << true), {"a", "a.b"}},
                                  {BSON("c.d.e" << false), {"a", "a.b", "a.c", "c", "c.d"}},
                                  {BSON("c.d.e" << true), {"c", "c.d", "c.d.e"}}}));
    basicTests(
        __LINE__,
        R"({a: [{b:[1]}, {c:[2]}], c: {d: 2}, e: 1, f: {g: {h: 1}}})",
        {
            {"a", {"", "[o1", kHasSubPath}},
            {"a.b", {"1", "[{[", kIsSparse}},
            {"a.c", {"2", "[1{[", kIsSparse}},
            {"c", {"", "", kHasSubPath}},
            {"c.d", {"2", ""}},
            {"e", {"1", ""}},
            {"f", {"", "", kHasSubPath}},
            {"f.g", {"", "", kHasSubPath}},
            {"f.g.h", {"1", ""}},
        },
        expectedProjPairs({{{}, {"a", "a.b", "a.c", "c", "c.d", "e", "f", "f.g", "f.g.h"}},
                           {BSON("a.c" << true), {"a", "a.c"}},
                           {BSON("c" << true << "a" << true), {"a", "a.b", "a.c", "c", "c.d"}}}));
    basicTests(
        __LINE__,
        R"({a: {b: [{c: 2}]}, c: [{d: 3}, {d: 4}]})",
        {{"a", {"", "", kHasSubPath}},
         {"a.b", {"", "{[o", kHasSubPath}},
         {"a.b.c", {"2", "{["}},
         {"c", {"", "[o1", kHasSubPath}},
         {"c.d", {"3,4", "["}}},
        expectedProjPairs({{{}, {"a", "a.b", "a.b.c", "c", "c.d"}},
                           {BSON("c" << true << "a" << true), {"a", "a.b", "a.b.c", "c", "c.d"}},
                           {BSON("c" << false << "a" << false), {}},
                           {BSON("w" << false), {"a", "a.b", "a.b.c", "c", "c.d"}},
                           {BSON("w" << true), {}}}));
    basicTests(__LINE__,
               R"({a: [{d: 2}, {b:[1]}, {c: [2]}], c: {d: 2}})",
               {{"a", {"", "[o2", kHasSubPath}},
                {"a.b", {"1", "[1{[", kIsSparse}},
                {"a.c", {"2", "[2{[", kIsSparse}},
                {"a.d", {"2", "[", kIsSparse}},
                {"c", {"", "", kHasSubPath}},
                {"c.d", {"2", ""}}},
               expectedProjPairs({{{}, {"a", "a.b", "a.c", "a.d", "c", "c.d"}},
                                  {BSON("a" << true), {"a", "a.b", "a.c", "a.d"}},
                                  {BSON("a.b" << true), {"a", "a.b"}}}));
    basicTests(__LINE__,
               R"({a: [{b:[{e: 2}, {c: 2}]}], b: [{c:[2]}]})",
               {{"a", {"", "[o", kHasSubPath}},
                {"a.b", {"", "[{[o1", kHasSubPath}},
                {"a.b.c", {"2", "[{[1", kIsSparse}},
                {"a.b.e", {"2", "[{[", kIsSparse}},
                {"b", {"", "[o", kHasSubPath}},
                {"b.c", {"2", "[{["}}},
               expectedProjPairs({{{}, {"a", "a.b", "a.b.c", "a.b.e", "b", "b.c"}},
                                  {BSON("a.b.e" << false), {"a", "a.b", "a.b.c", "b", "b.c"}},
                                  {BSON("a.b" << false), {"a", "b", "b.c"}},
                                  {BSON("b" << true), {"b", "b.c"}},
                                  {BSON("b" << true), {"b", "b.c"}}}));
    basicTests(
        __LINE__,
        R"({a: [{b:[1, {c: 2}]}], b: [{c:[2]}]})",
        {{"a", {"", "[o", kHasSubPath}},
         {"a.b", {"1", "[{[|o", kHasSubPath}},
         {"a.b.c", {"2", "[{[1", kIsSparse}},
         {"b", {"", "[o", kHasSubPath}},
         {"b.c", {"2", "[{["}}},
        expectedProjPairs({{{}, {"a", "a.b", "a.b.c", "b", "b.c"}},
                           {BSON("b" << false), {"a", "a.b", "a.b.c"}},
                           {BSON("a" << true << "b" << true), {"a", "a.b", "a.b.c", "b", "b.c"}}}));
    basicTests(__LINE__,
               R"({a: [{b:1}, {c:[2, 3]}, null]})",
               {{"a", {"null", "[o1", kHasSubPath}},
                {"a.b", {"1", "[", kIsSparse}},
                {"a.c", {"2,3", "[1{[", kIsSparse}}},
               expectedProjPairs({{{}, {"a", "a.b", "a.c"}}, {BSON("a" << false), {}}}));
    basicTests(__LINE__,
               R"({a: [{b:1}, {b:[2, 3]}]})",
               {{"a", {"", "[o1", kHasSubPath}}, {"a.b", {"1,2,3", "[|{["}}},
               expectedProjPairs({{{}, {"a", "a.b"}}, {BSON("a.b" << false), {"a"}}}));
    basicTests(__LINE__,
               R"({a: [{b:1}, {c:2}]})",
               {{"a", {"", "[o1", kHasSubPath}},
                {"a.b", {"1", "[", kIsSparse}},
                {"a.c", {"2", "[1", kIsSparse}}},
               expectedProjPairs({{{}, {"a", "a.b", "a.c"}},
                                  {BSON("a.c" << true), {"a", "a.c"}},
                                  {BSON("a.b" << true), {"a", "a.b"}},
                                  {BSON("b" << true), {}}}));
}

static const BSONObj kDefaultIndexKey = fromjson("{'$**': 'columnstore'}");
static const BSONObj kDefaultPathProjection;

void traverseTree(ColumnProjectionNode* root, StringMap<bool>& map, std::string parentPath = "") {
    for (const auto& child : root->children()) {
        auto path = parentPath == "" ? child.first : parentPath + "." + child.first;
        map[path] = child.second.get()->isLeaf();
        traverseTree(child.second.get(), map, path);
    }
}

void getPaths(ColumnProjectionNode* root,
              stdx::unordered_set<std::string>& set,
              std::string parentPath = "") {
    for (const auto& child : root->children()) {
        std::string path = parentPath == "" ? child.first : parentPath + "." + child.first;
        set.insert(path);
        getPaths(child.second.get(), set, path);
    }
}

void testProjectionTree(const ColumnProjectionTree* tree, BSONObj fields, bool isInclusion) {
    StringMap<bool> map;
    traverseTree(tree->root(), map);
    for (auto [key, elem] : fields) {
        // We include _id into the projection only if it's inclusivity matches the projection's.
        if (key == "_id" && fields.getBoolField(key) != isInclusion) {
            continue;
        }
        // The fields in projection are leaves in the Projection AST.
        ASSERT(map.find(key) != map.end());
        ASSERT(map[key]);
        map.erase(key);
    }
    for (auto const& [key, val] : map) {
        if (isInclusion && key == "_id") {
            // If _id was not specified in an inclusion projection, then it is defaulted to true.
            ASSERT(val);
        } else {
            // Anything not in the projection should be false.
            ASSERT_FALSE(val);
        }
    }
}

void testProjectionTreeViaPathProjection(BSONObj pathProjection, bool isInclusion) {
    auto ckg = ColumnKeyGenerator(kDefaultIndexKey, pathProjection);
    auto tree = ckg.getColumnProjectionTree();
    testProjectionTree(tree, pathProjection, tree->isInclusion());
    ASSERT_EQ(isInclusion, tree->isInclusion());
}

void testProjectionTreeViaFieldPrefix(std::string field) {
    auto ckg = ColumnKeyGenerator(fromjson("{'" + field + ".$**': 'columnstore'}"), fromjson("{}"));
    auto tree = ckg.getColumnProjectionTree();
    testProjectionTree(tree, BSON(field << true), tree->isInclusion());
    // Projection specified in key pattern should always be included.
    ASSERT(tree->isInclusion());
}

TEST(ColumnProjectionTree, FullProjection) {
    auto ckg = ColumnKeyGenerator(kDefaultIndexKey, kDefaultPathProjection);
    auto tree = ckg.getColumnProjectionTree();
    testProjectionTree(tree, kDefaultPathProjection, tree->isInclusion());
    // An empty projection is treated as _excluding_ nothing in this context.
    ASSERT_FALSE(tree->isInclusion());
}

TEST(ColumnProjectionTree, ProjectionPathField) {
    // Testing {"path.to.field.$**": "columnstore"} case.
    testProjectionTreeViaFieldPrefix("path");
    testProjectionTreeViaFieldPrefix("path.to.field");
    testProjectionTreeViaFieldPrefix("many.sub.paths.to.get.to.field");
    testProjectionTreeViaFieldPrefix("_id");
}

TEST(ColumnProjectionTree, ColumnStoreProjectionField) {
    // Fields.
    BSONObj pathProjection1 = BSON("path" << true);
    testProjectionTreeViaPathProjection(pathProjection1, true);
    BSONObj pathProjection2 = BSON("path" << false);
    testProjectionTreeViaPathProjection(pathProjection2, false);

    // Fields not sharing same path.
    BSONObj pathProjection3 = BSON("path" << true << "another path" << true);
    testProjectionTreeViaPathProjection(pathProjection3, true);
    BSONObj pathProjection4 = BSON("path" << false << "another path" << false);
    testProjectionTreeViaPathProjection(pathProjection4, false);

    // Fields sharing same path
    BSONObj pathProjection5 = BSON("path.to.subfield.1" << true << "path.to.subfield.2" << true
                                                        << "another.path" << true);
    testProjectionTreeViaPathProjection(pathProjection5, true);

    BSONObj pathProjection6 = BSON("path.to.subfield.1" << false << "path.to.subfield.2" << false
                                                        << "another.path" << false);
    testProjectionTreeViaPathProjection(pathProjection6, false);

    // Various tests involving _id cases.
    BSONObj pathProjection7 =
        BSON("_id" << false << "path.to.subfield.2" << true << "another.path" << true);
    testProjectionTreeViaPathProjection(pathProjection7, true);

    BSONObj pathProjection8 =
        BSON("_id" << true << "path.to.subfield.2" << false << "another.path" << false);
    testProjectionTreeViaPathProjection(pathProjection8, false);

    BSONObj pathProjection9 =
        BSON("_id" << true << "path.to.subfield.2" << true << "another.path" << true);
    testProjectionTreeViaPathProjection(pathProjection9, true);

    BSONObj pathProjection10 =
        BSON("_id" << false << "path.to.subfield.2" << false << "another.path" << false);
    testProjectionTreeViaPathProjection(pathProjection10, false);
}

TEST(ColumnProjection, SinglePathNoId) {
    BSONObj keyPattern = BSON("a.b.$**"
                              << "columnstore");
    BSONObj pathProjection = BSONObj();
    auto ckg = ColumnKeyGenerator(BSON("a.b.$**"
                                       << "columnstore"),
                                  pathProjection);
    auto tree = ckg.getColumnProjectionTree();
    stdx::unordered_set<std::string> set;
    getPaths(tree->root(), set);
    ASSERT_EQ(2, set.size());
    ASSERT(set.count("a"));
    ASSERT(set.count("a.b"));
    ASSERT_FALSE(set.count("_id"));
    ASSERT(tree->isInclusion());
}

TEST(ColumnProjection, SinglePathWithId) {
    BSONObj keyPattern = BSON("_id.foo.$**"
                              << "columnstore");
    BSONObj pathProjection = BSONObj();
    auto ckg = ColumnKeyGenerator(keyPattern, pathProjection);
    auto tree = ckg.getColumnProjectionTree();
    stdx::unordered_set<std::string> set;
    getPaths(tree->root(), set);
    ASSERT_EQ(2, set.size());
    ASSERT(set.count("_id"));
    ASSERT(set.count("_id.foo"));
    ASSERT(tree->isInclusion());
}

TEST(ColumnProjection, IncludeProjectionWithoutId) {
    BSONObj keyPattern = BSON("$**"
                              << "columnstore");
    BSONObj pathProjection = BSON("path" << true << "another path" << true);
    auto ckg = ColumnKeyGenerator(keyPattern, pathProjection);
    auto tree = ckg.getColumnProjectionTree();
    stdx::unordered_set<std::string> set;
    getPaths(tree->root(), set);
    ASSERT_EQ(3, set.size());
    ASSERT(set.count("_id"));
    ASSERT(set.count("path"));
    ASSERT(set.count("another path"));
    ASSERT(tree->isInclusion());
}

TEST(ColumnProjection, ExcludeProjectionIncludeId) {
    BSONObj keyPattern = BSON("$**"
                              << "columnstore");
    BSONObj pathProjection = BSON("path" << false << "another path" << false);
    auto ckg = ColumnKeyGenerator(keyPattern, pathProjection);
    auto tree = ckg.getColumnProjectionTree();
    stdx::unordered_set<std::string> set;
    getPaths(tree->root(), set);
    ASSERT_EQ(2, set.size());
    ASSERT(set.count("path"));
    ASSERT(set.count("another path"));
    ASSERT_FALSE(tree->isInclusion());
}

// TODO more tests, of course! In particular, the testing of complex update changes is a bit light.
}  // namespace
}  // namespace mongo::column_keygen
