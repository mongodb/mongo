/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/bson/json.h"
#include "mongo/db/exec/sbe/values/bson_block.h"
#include "mongo/db/exec/sbe/values/cell_interface.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/unittest/unittest.h"

#include <boost/algorithm/string/join.hpp>

namespace mongo::sbe {

using Get = value::CellBlock::Get;
using Traverse = value::CellBlock::Traverse;
using Id = value::CellBlock::Id;

struct PathTestCase {
    value::CellBlock::Path path;
    BSONObj projectValue;
};

void dummyCallBack(value::BsonWalkNode<value::ScalarProjectionPositionInfoRecorder>* node,
                   value::TypeTags eltTag,
                   value::Value eltVal,
                   const char* bson) {
    if (auto rec = node->projRecorder) {
        rec->recordValue(eltTag, eltVal);
    }
}

class BsonWalkNodeScalarTest : public mongo::unittest::Test {
public:
    void testPaths(const std::vector<PathTestCase>& testCases,
                   const BSONObj& data,
                   bool mayHaveArrayValue = true) {
        value::BsonWalkNode<value::ScalarProjectionPositionInfoRecorder> root;
        // Construct extractor.
        std::vector<value::ScalarProjectionPositionInfoRecorder> recorders;
        recorders.reserve(testCases.size());
        for (auto& tc : testCases) {
            recorders.emplace_back();
            root.add(tc.path, nullptr, &recorders.back());
        }
        auto [inputTag, inputVal] = stage_builder::makeValue(data);
        value::ValueGuard vg{inputTag, inputVal};  // Free input value's memory on exit.

        // Extract paths from input data in a single pass.
        value::walkField<value::ScalarProjectionPositionInfoRecorder>(
            &root, inputTag, inputVal, nullptr /* bsonPtr */, dummyCallBack);

        // Verify the extracted values are correct.
        size_t idx = 0;
        for (auto& tc : testCases) {
            value::MoveableValueGuard value = recorders[idx].extractValue();
            auto [resultsTag, resultsVal] = value.get();
            BSONObjBuilder tmp;
            bson::appendValueToBsonObj(tmp, "result", resultsTag, resultsVal);
            BSONObj resultObj = tmp.obj();  // Frees memory in tmp.
            ASSERT_TRUE(SimpleBSONObjComparator::kInstance.evaluate(resultObj == tc.projectValue))
                << "Incorrect projection for path " << tc.path << " on input " << data
                << ". Expected " << tc.projectValue << ", got " << resultObj << ".";
            ++idx;
        }
    }
};

TEST_F(BsonWalkNodeScalarTest, Sanity) {
    {
        BSONObj inputObj = fromjson("{a: [{b: 1}, {b: [{c: 3}]}]}");
        std::vector<PathTestCase> tests{
            PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"b"}, Id{}},
                         .projectValue = fromjson("{result: [1, [{c : 3}]]}")}};
        testPaths(tests, inputObj);
    }
    {
        BSONObj inputObj = fromjson("{a: [{b: [1]}]}");
        std::vector<PathTestCase> tests{PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"b"}, Id{}},
                                                     .projectValue = fromjson("{result: [[1]]}")}};
        testPaths(tests, inputObj);
    }
    {
        BSONObj inputObj = fromjson("{a: [{b: [{c: [1]}]}]}");
        std::vector<PathTestCase> tests{
            PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"b"}, Id{}},
                         .projectValue = fromjson("{result: [[{c : [1]}]]}")},
            PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"b"}, Traverse{}, Get{"c"}, Id{}},
                         .projectValue = fromjson("{result: [[[1]]]}")}};
        testPaths(tests, inputObj);
    }
    {
        // Simple toplevel field.
        BSONObj inputObj = fromjson("{a:1, b:2}");
        std::vector<PathTestCase> tests{
            PathTestCase{.path = {Get{"a"}, Id{}}, .projectValue = fromjson("{result: 1}")}};
        testPaths(tests, inputObj);
    }
    {
        // Two toplevel fields.
        BSONObj inputObj = fromjson("{a:1, b:2}");
        std::vector<PathTestCase> tests{
            PathTestCase{.path = {Get{"a"}, Id{}}, .projectValue = fromjson("{result: 1}")},
            PathTestCase{.path = {Get{"b"}, Id{}}, .projectValue = fromjson("{result: 2}")}};
        testPaths(tests, inputObj);
    }
    {
        // Simple nested path.
        BSONObj inputObj = fromjson("{a: {b:1}}");
        std::vector<PathTestCase> tests{PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"b"}, Id{}},
                                                     .projectValue = fromjson("{result: 1}")}};
        testPaths(tests, inputObj);
    }
    {
        // Document in an array of length 1.
        BSONObj inputObj = fromjson("{a: [{b:1}]}");
        std::vector<PathTestCase> tests{PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"b"}, Id{}},
                                                     .projectValue = fromjson("{result: [1]}")}};
        testPaths(tests, inputObj);
    }
    {
        // Documents in an array of length 2.
        BSONObj inputObj = fromjson("{a: [{b:1}, {b:2}]}");
        std::vector<PathTestCase> tests{PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"b"}, Id{}},
                                                     .projectValue = fromjson("{result: [1, 2]}")}};
        testPaths(tests, inputObj);
    }
    {
        // Three paths.
        BSONObj inputObj = fromjson("{a: {b: 1, c: 3}, d: 5}");
        std::vector<PathTestCase> tests{
            PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"b"}, Id{}},
                         .projectValue = fromjson("{result: 1}")},
            PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"c"}, Id{}},
                         .projectValue = fromjson("{result: 3}")},
            PathTestCase{.path = {Get{"d"}, Id{}}, .projectValue = fromjson("{result: 5}")}};
        testPaths(tests, inputObj);
    }
    {
        // Document, array, document, array, document.
        BSONObj inputObj = fromjson("{a: [{b:[{c:1}]}]}");
        std::vector<PathTestCase> tests{
            PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"b"}, Traverse{}, Get{"c"}, Id{}},
                         .projectValue = fromjson("{result: [[1]]}")}};
        testPaths(tests, inputObj);
    }
    {
        // Toplevel field does not exist.
        BSONObj inputObj = fromjson("{a:1}");
        std::vector<PathTestCase> tests{
            PathTestCase{.path = {Get{"b"}, Id{}}, .projectValue = fromjson("{}")}};
        testPaths(tests, inputObj);
    }
    {
        // Toplevel field is null.
        BSONObj inputObj = fromjson("{a:null}");
        std::vector<PathTestCase> tests{
            PathTestCase{.path = {Get{"a"}, Id{}}, .projectValue = fromjson("{result: null}")}};
        testPaths(tests, inputObj);
    }
    {
        // Toplevel field is the empty array.
        BSONObj inputObj = fromjson("{a:[]}");
        std::vector<PathTestCase> tests{
            PathTestCase{.path = {Get{"a"}, Id()}, .projectValue = fromjson("{result: []}")}};
        testPaths(tests, inputObj);
    }
    {
        // Toplevel field is the empty document.
        BSONObj inputObj = fromjson("{a:{}}");
        std::vector<PathTestCase> tests{
            PathTestCase{.path = {Get{"a"}, Id{}}, .projectValue = fromjson("{result: {}}")}};
        testPaths(tests, inputObj);
    }
    {
        // Empty array in array.
        BSONObj inputObj = fromjson("{a:[{b: []}]}");
        std::vector<PathTestCase> tests{PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"b"}, Id{}},
                                                     .projectValue = fromjson("{result: [[]]}")}};
        testPaths(tests, inputObj);
    }
}

TEST_F(BsonWalkNodeScalarTest, NestedArrays) {
    {
        BSONObj inputObj = fromjson("{a: [[{b: 1}], {b: 2}]}");
        std::vector<PathTestCase> tests{PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"b"}, Id{}},
                                                     .projectValue = fromjson("{result: [2]}")}};
        testPaths(tests, inputObj);
    }
    {
        BSONObj inputObj = fromjson("{a: [[{b: 1}]]}");
        std::vector<PathTestCase> tests{
            PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"b"}, Traverse{}, Get{"c"}, Id{}},
                         .projectValue = fromjson("{result: []}")}};
        testPaths(tests, inputObj);
    }
    {
        // Document, array, document, array, array, document.
        BSONObj inputObj = fromjson("{a: [{b: [{c: 1}, [{c:2}]]}]}");
        std::vector<PathTestCase> tests{
            PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"b"}, Traverse{}, Get{"c"}, Id{}},
                         .projectValue = fromjson("{result: [[1]]}")}};
        testPaths(tests, inputObj);
    }
}

TEST_F(BsonWalkNodeScalarTest, DottedFieldNames) {
    {
        // Dotted toplevel field name.
        BSONObj inputObj = BSON("a.b" << 1);
        std::vector<PathTestCase> tests{
            PathTestCase{.path = {Get{"a.b"}, Id{}}, .projectValue = fromjson("{result: 1}")}};
        testPaths(tests, inputObj);
    }
    {
        // Dotted nested field name.
        BSONObj inputObj = BSON("a" << BSON("b.c" << 1));
        std::vector<PathTestCase> tests{
            PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"b.c"}, Id{}},
                         .projectValue = fromjson("{result: 1}")}};
        testPaths(tests, inputObj);
    }
    {
        // Dotted field name 'conflicts' with nested field names.
        BSONObj inputObj = BSON("a.b" << 1 << "a" << BSON("b" << 2));
        std::vector<PathTestCase> tests{
            PathTestCase{.path = {Get{"a.b"}, Id{}}, .projectValue = fromjson("{result: 1}")},
            PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"b"}, Id{}},
                         .projectValue = fromjson("{result: 2}")},
        };
        testPaths(tests, inputObj);
    }
}

TEST_F(BsonWalkNodeScalarTest, RandomlyGenerated) {
    {
        BSONObj inputObj = fromjson(
            "{ f0 : [ 1, { f0 : 2, f1 : [ [ 3, 4 ] ] }, [ [ 5, [ 6, 7 ] ] ] ], f1 : [ { f0 : [ [ "
            "8, 9, 10 ], [ 11, 12, 13 ], [ 14 ] ] }, [ { f0 : 15 } ], { f0 : { f0 : [ 16, 17, 18 ] "
            "}, f1 : 19, f2 : 20 } ] }");
        std::vector<PathTestCase> tests{
            PathTestCase{.path = {Get{"f1"},
                                  Traverse{},
                                  Get{"f0"},
                                  Traverse{},
                                  Get{"f0"},
                                  Traverse{},
                                  Get{"f0"},
                                  Id{}},
                         .projectValue = fromjson("{result : [ [ ], [ ] ] }")}};
        testPaths(tests, inputObj);
    }
    {
        BSONObj inputObj = fromjson(
            "{ f0 : { f1 : [ [ 4, 5 ], { f0 : 6, f1 : 7, f2 : 8 } ] }, f1 : [ [ 9 ], 10, [ { f0 : "
            "11 }, 12 ] ] }");
        {
            std::vector<PathTestCase> tests{PathTestCase{
                .path = {Get{"f1"}, Id{}},
                .projectValue = fromjson("{result: [ [ 9 ], 10, [ { f0 : 11 }, 12 ] ] }")}};
            testPaths(tests, inputObj);
        }
        {
            std::vector<PathTestCase> tests{PathTestCase{
                .path = {Get{"f0"}, Traverse{}, Get{"f1"}, Id{}},
                .projectValue = fromjson("{result: [ [ 4, 5 ], { f0 : 6, f1 : 7, f2 : 8 } ] }")}};
            testPaths(tests, inputObj);
        }
    }
    {
        BSONObj inputObj = fromjson(
            "{f0: [1, {f0: 2, f1: 3, f2: [4]}], f1: {f0: {f0: {f0: 5, f1: 6}, f1: 7}, f1: 8, f2: "
            "9}, f2: 10}");
        std::vector<PathTestCase> tests{
            PathTestCase{.path = {Get{"f0"}, Traverse{}, Get{"f0"}, Id{}},
                         .projectValue = fromjson("{result: [ 2 ]}")},
            PathTestCase{.path = {Get{"f0"}, Traverse{}, Get{"f1"}, Id{}},
                         .projectValue = fromjson("{result: [ 3 ]}")},
            PathTestCase{.path = {Get{"f1"},
                                  Traverse{},
                                  Get{"f0"},
                                  Traverse{},
                                  Get{"f0"},
                                  Traverse{},
                                  Get{"f0"},
                                  Id{}},
                         .projectValue = fromjson("{result: 5}")},
        };
        testPaths(tests, inputObj);
    }
    {
        BSONObj inputObj = fromjson("{f0: {f0: 1, f1: 2, f2: [3, 4, {f0: 5, f1: 6}]}}");
        std::vector<PathTestCase> tests{
            PathTestCase{.path = {Get{"f0"}, Traverse{}, Get{"f0"}, Id{}},
                         .projectValue = fromjson("{result: 1}")},
            PathTestCase{.path = {Get{"f0"}, Traverse{}, Get{"f1"}, Id{}},
                         .projectValue = fromjson("{result: 2}")},
            PathTestCase{.path = {Get{"f0"}, Traverse{}, Get{"f2"}, Traverse{}, Get{"f0"}, Id{}},
                         .projectValue = fromjson("{result: [5]}")},
            PathTestCase{.path = {Get{"f0"}, Traverse{}, Get{"f2"}, Traverse{}, Get{"f1"}, Id{}},
                         .projectValue = fromjson("{result: [6]}")},
        };
        testPaths(tests, inputObj);
    }
}

TEST_F(BsonWalkNodeScalarTest, DuplicateFields) {
    {
        // Duplicate toplevel field names
        BSONObj inputObj = BSON("a" << 1 << "a" << 2);
        std::vector<PathTestCase> tests{
            PathTestCase{.path = {Get{"a"}, Id{}}, .projectValue = fromjson("{result: 2}")}};
        testPaths(tests, inputObj);
    }
    {
        // Duplicate toplevel field names, values in the opposite order
        BSONObj inputObj = BSON("a" << 2 << "a" << 1);
        std::vector<PathTestCase> tests{
            PathTestCase{.path = {Get{"a"}, Id{}}, .projectValue = fromjson("{result: 1}")}};
        testPaths(tests, inputObj);
    }
    {
        // Duplicate nested field names
        BSONObj inputObj = BSON("a" << BSON("a" << 1 << "a" << 2));
        std::vector<PathTestCase> tests{PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"a"}, Id{}},
                                                     .projectValue = fromjson("{result: 2}")}};
        testPaths(tests, inputObj);
    }
    {
        // Duplicate field names at multiple levels.
        BSONObj inputObj =
            BSON("a" << BSON("a" << 1 << "a" << 2) << "a" << BSON("a" << 3 << "a" << 4));
        std::vector<PathTestCase> tests{PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"a"}, Id{}},
                                                     .projectValue = fromjson("{result: 4}")}};
        testPaths(tests, inputObj);
    }
    {
        // We accumulate all duplicate field values for documents in arrays.
        BSONObj inputObj =
            BSON("a" << BSON_ARRAY(BSON("a" << 1 << "a" << 2) << BSON("a" << 3 << "a" << 4)));
        std::vector<PathTestCase> tests{
            PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"a"}, Id{}},
                         .projectValue = fromjson("{result: [1, 2, 3, 4]}")}};
        testPaths(tests, inputObj);
    }
    {
        // We accumulate all duplicate field values for documents in arrays.
        BSONObj inputObj = BSON("a" << BSON_ARRAY(BSON("a" << 1) << BSON("a" << 3 << "a" << 4)));
        std::vector<PathTestCase> tests{
            PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"a"}, Id{}},
                         .projectValue = fromjson("{result: [1, 3, 4]}")}};
        testPaths(tests, inputObj);
    }
    {
        // We accumulate all duplicate field values for documents in arrays.
        BSONObj inputObj =
            BSON("a" << BSON_ARRAY(BSON("a" << 1) << BSON("a" << BSON("a" << 3) << "a" << 4)));
        std::vector<PathTestCase> tests{
            PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"a"}, Id{}},
                         .projectValue = fromjson("{result: [1, {a: 3}, 4]}")}};
        testPaths(tests, inputObj);
    }
    {
        // The semantics of projecting duplicate field names with nested arrays is asinine. We don't
        // guarantee any particular behavior here; this is just a regression test to verify the
        // behavior does not change from its currently-rather-dumb behavior.
        BSONObj inputObj =
            BSON("a" << BSON_ARRAY(BSON("a" << 1)
                                   << BSON("a" << BSON_ARRAY(BSON("a" << 3)) << "a" << 4)));
        std::vector<PathTestCase> tests{
            PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"a"}, Id{}},
                         .projectValue = fromjson("{result: [1, [{a: 3}], 4]}")}};
        testPaths(tests, inputObj);
    }
}

// Test accessing every field from a single level document with many fields.
TEST_F(BsonWalkNodeScalarTest, BigFlat) {
    int n = 1000;
    BSONObjBuilder bob;
    std::vector<PathTestCase> tests;
    for (int i = 0; i < n; i++) {
        std::stringstream s;
        s << "f" << i;
        std::string fieldName = s.str();
        bob << fieldName << i;
        tests.push_back(
            PathTestCase{.path = {Get{fieldName}, Id{}},
                         .projectValue = fromjson("{result: " + std::to_string(i) + "}")});
    }
    testPaths(tests, bob.obj());
}

// Project every leaf node of a perfect N-ary tree document.
TEST_F(BsonWalkNodeScalarTest, PerfectTree) {
    BSONObjBuilder bob;
    int curIdx = 0;
    int depth = 4;
    int branchingFactor = 3;
    std::vector<value::CellBlock::Path> paths;
    value::CellBlock::Path curPath;
    std::function<void(BSONObjBuilder&, int)> rec = [&](BSONObjBuilder& curBob, int curDepth) {
        if (curDepth == depth) {
            curPath.push_back(Id{});
            BSONObj ret = curBob.done();
            paths.push_back(curPath);
            curPath.pop_back();
            return;
        }
        for (int i = 0; i < branchingFactor; i++) {
            std::stringstream curField;
            curField << "f" << (curIdx++);
            if (curDepth > 0) {
                curPath.push_back(Traverse{});
            }
            curPath.push_back(Get{curField.str()});
            BSONObjBuilder childBob(curBob.subobjStart(curField.str()));
            rec(childBob, curDepth + 1);
            if (curDepth > 0) {
                curPath.pop_back();
            }
            curPath.pop_back();
        }
    };
    BSONObjBuilder root;
    rec(root, 0);
    std::vector<PathTestCase> tests;
    for (const auto& path : paths) {
        tests.push_back(PathTestCase{.path = path, .projectValue = fromjson("{result: {}}")});
    }
    testPaths(tests, root.done());
}

}  // namespace mongo::sbe
