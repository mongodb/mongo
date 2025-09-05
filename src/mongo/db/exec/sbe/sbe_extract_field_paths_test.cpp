/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/extract_field_paths.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"

#include <vector>

#include <fmt/format.h>

namespace mongo::sbe {

namespace {
using Path = value::CellBlock::Path;
using Get = value::CellBlock::Get;
using Traverse = value::CellBlock::Traverse;
using Id = value::CellBlock::Id;
}  // namespace

class ExtractFieldPathsStageTest : public PlanStageTestFixture {
public:
    const std::string expectedFieldName = "expected";

    std::string makeExpectedArr(std::string expectedArr) {
        auto tempObj = fmt::format("{{{}: {}}}", expectedFieldName, expectedArr);
        return tempObj;
    }

    std::vector<Path> makePathReqs(std::vector<FieldPath> pathReqFieldPaths) {
        std::vector<Path> pathReqs;
        for (const auto& fieldPath : pathReqFieldPaths) {
            Path path;
            for (size_t i = 0; i < fieldPath.getPathLength() - 1; ++i) {
                path.emplace_back(Get{.field = std::string(fieldPath.getFieldName(i))});
                path.emplace_back(Traverse{});
            }
            // Omit the Traverse for the last path component.
            if (fieldPath.getPathLength() != 0) {
                path.emplace_back(Get{
                    .field = std::string(fieldPath.getFieldName(fieldPath.getPathLength() - 1))});
            }
            path.emplace_back(Id{});
            pathReqs.push_back(std::move(path));
        }
        return pathReqs;
    }

    /**
     * @param paths: A vector of FieldPath's that we want to read from the input documents.
     * @param input: A vector of size n, containing string representations of BSON documents
     * that we want to read from.
     * @param outputs: A vector of size n, containing string representations of BSON arrays that
     * correspond to the expected output for each input document. The size of each sub array is
     * equal to the number of paths that exist in the corresponding input document.
     *
     * @note Since missing paths map to TypeTags::Nothing which can't easily be represented in
     * BSON, the lack of an array entry corresponds to a missing path. This also means that if
     * we are looking for paths "a" and "b", the expected outputs for {a: 1} and {b: 1} will
     * both be [1] even though the values in each specific slot will be different depending on
     * which path they correspond to.
     *
     * @note See the function comment for runTestMulti for an explanation of the array nesting
     * of the inputs and expected outputs.
     */
    void runExtractFieldPathsTest(std::vector<FieldPath> paths,
                                  std::vector<std::string> inputs,
                                  std::vector<std::string> outputs,
                                  bool includeCellBlockTraverse = true) {
        tassert(
            10757501, "expected inputs and outputs size to match", inputs.size() == outputs.size());

        BSONArrayBuilder inputBab;
        BSONArrayBuilder outputBab;
        for (size_t i = 0; i < inputs.size(); ++i) {
            inputBab << BSON_ARRAY(fromjson(inputs[i]));
            outputBab << fromjson(makeExpectedArr(outputs[i]))[expectedFieldName];
        }
        auto [inputTag, inputVal] = stage_builder::makeValue(inputBab.arr());
        value::ValueGuard inputGuard{inputTag, inputVal};
        auto [expectedTag, expectedVal] = stage_builder::makeValue(outputBab.arr());
        value::ValueGuard expectedGuard{expectedTag, expectedVal};

        auto makeStageFn = [&, this](value::SlotVector scanSlots,
                                     std::unique_ptr<PlanStage> scanStage) {
            auto pathReqs = makePathReqs(paths);
            value::SlotVector outputSlots = generateMultipleSlotIds(pathReqs.size());

            auto extractFieldPathsStage = makeS<ExtractFieldPathsStage>(
                std::move(scanStage), scanSlots[0], pathReqs, outputSlots, kEmptyPlanNodeId);

            return std::make_pair(outputSlots, std::move(extractFieldPathsStage));
        };

        inputGuard.reset();
        expectedGuard.reset();
        runTestMulti(1, inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
    }
};

// Tests where all paths can be found in the input documents.
TEST_F(ExtractFieldPathsStageTest, SinglePathNonNestedNonArrayTest) {
    std::vector<FieldPath> paths{"a"};
    std::vector<std::string> inputs{"{a: 1}", "{a: 2}"};
    std::vector<std::string> outputs{"[1]", "[2]"};

    runExtractFieldPathsTest(paths, inputs, outputs);
}

TEST_F(ExtractFieldPathsStageTest, MultiPathNonNestedNonArrayTest) {
    std::vector<FieldPath> paths{"a", "b"};
    std::vector<std::string> inputs{"{a: 1, b: 3}", "{b: 4, a: 2}"};
    std::vector<std::string> outputs{"[1, 3]", "[2, 4]"};

    runExtractFieldPathsTest(paths, inputs, outputs);
}

TEST_F(ExtractFieldPathsStageTest, SinglePathNestedNonArrayTest) {
    std::vector<FieldPath> paths{"a.b"};
    std::vector<std::string> inputs{"{a: {b: 1}}", "{a: {b: 2}}"};
    std::vector<std::string> outputs{"[1]", "[2]"};

    runExtractFieldPathsTest(paths, inputs, outputs);
}

TEST_F(ExtractFieldPathsStageTest, MultiPathNestedNonArrayTest) {
    std::vector<FieldPath> paths{"a.b", "a.c", "d"};
    std::vector<std::string> inputs{"{a: {b: 1, c: 3}, d: 5}", "{d :6, a: {c: 4, b: 2}}"};
    std::vector<std::string> outputs{"[1, 3, 5]", "[2, 4, 6]"};

    runExtractFieldPathsTest(paths, inputs, outputs);
}

TEST_F(ExtractFieldPathsStageTest, SinglePathNonNestedNonArrayDeepValueTest) {
    std::vector<FieldPath> paths{"a"};
    std::vector<std::string> inputs{"{a: \"not a StringSmall\"}", "{a: \"is a StringBig\"}"};
    std::vector<std::string> outputs{"[ \"not a StringSmall\"]", "[\"is a StringBig\"]"};

    runExtractFieldPathsTest(paths, inputs, outputs);
}

TEST_F(ExtractFieldPathsStageTest, SinglePathArrayTest) {
    std::vector<FieldPath> paths{"a.b"};
    std::vector<std::string> inputs{"{a: [{b: 1}]}", "{a: [{b: 2}]}"};
    std::vector<std::string> outputs{"[[1]]", "[[2]]"};

    runExtractFieldPathsTest(paths, inputs, outputs);
}

TEST_F(ExtractFieldPathsStageTest, SinglePathArrayLength2Test) {
    std::vector<FieldPath> paths{"a.b"};
    std::vector<std::string> inputs{"{a: [{b: 1}, {b: 2}]}"};
    std::vector<std::string> outputs{"[[1, 2]]"};

    runExtractFieldPathsTest(paths, inputs, outputs);
}

TEST_F(ExtractFieldPathsStageTest, TwoPathsArrayLength2Test) {
    std::vector<FieldPath> paths{"a.b", "a.c"};
    std::vector<std::string> inputs{"{a: [{b: 1, c: 2}, {b: 3, c: 4}]}"};
    std::vector<std::string> outputs{"[[1, 3], [2, 4]]"};

    runExtractFieldPathsTest(paths, inputs, outputs);
}

TEST_F(ExtractFieldPathsStageTest, SinglePathNestedArrayTest) {
    std::vector<FieldPath> paths{"a.b.c"};
    std::vector<std::string> inputs{"{a: [{b: [{c: 1}, {c: 2}]}]}",
                                    "{a: [{b: [{c: 3}]}, {b: [{c: 4}]}]}"};
    std::vector<std::string> outputs{"[[[1, 2]]]", "[[[3], [4]]]"};

    runExtractFieldPathsTest(paths, inputs, outputs);
}

TEST_F(ExtractFieldPathsStageTest, TwoPathsNestedArrayTest) {
    std::vector<FieldPath> paths{"a.b.c", "a.d.e"};
    std::vector<std::string> inputs{"{a: [{b: [{c: 1}]}, {d: [{e: 2}]}]}"};
    std::vector<std::string> outputs{"[[[1]], [[2]]]"};

    runExtractFieldPathsTest(paths, inputs, outputs);
}

TEST_F(ExtractFieldPathsStageTest, TwoPathsVariedArrayNestingTest) {
    std::vector<FieldPath> paths{"a.b.c", "a.b.d"};
    std::vector<std::string> inputs{
        "{a: [{b: [{c: 1}, {c: 2}, {d: 3}]}, {b: {d: 4, c: 5}}, {b: [{d: 6}, {c: 7}, {d: "
        "8}]}]}"};
    std::vector<std::string> outputs{"[[[1, 2], 5, [7]], [[3], 4, [6, 8]]]"};

    runExtractFieldPathsTest(paths, inputs, outputs);
}

TEST_F(ExtractFieldPathsStageTest, ParentAndChildPathTest) {
    std::vector<FieldPath> paths{"a", "a.b"};
    std::vector<std::string> inputs{"{a: {b: 1}}", "{a: {b: 2}}"};
    std::vector<std::string> outputs{"[{b: 1}, 1]", "[{b: 2}, 2]"};

    runExtractFieldPathsTest(paths, inputs, outputs);
}

// Tests where some or all of the paths are not present in one or more of the input documents.
TEST_F(ExtractFieldPathsStageTest, PathDoesNotExistTest) {
    std::vector<FieldPath> paths{"b"};
    std::vector<std::string> inputs{"{a: 1}", "{a: 2}"};
    std::vector<std::string> outputs{"[]", "[]"};

    runExtractFieldPathsTest(paths, inputs, outputs);
}

TEST_F(ExtractFieldPathsStageTest, NestedPathDoesNotExistTest) {
    std::vector<FieldPath> paths{"a.c"};
    std::vector<std::string> inputs{"{a: 1}", "{a: 2}", "{a: {b: 3}}"};
    std::vector<std::string> outputs{"[]", "[]", "[]"};

    runExtractFieldPathsTest(paths, inputs, outputs);
}

TEST_F(ExtractFieldPathsStageTest, OnePathExistsOneDoesntTest) {
    std::vector<FieldPath> paths{"a", "a.c"};
    std::vector<std::string> inputs{"{a: 1}", "{a: 2}", "{a: {b: 3}}"};
    std::vector<std::string> outputs{"[1]", "[2]", "[{b: 3}]"};

    runExtractFieldPathsTest(paths, inputs, outputs);
}

TEST_F(ExtractFieldPathsStageTest, PathsExistInSomeDocumentsTest) {
    std::vector<FieldPath> paths{"a", "b", "c"};
    std::vector<std::string> inputs{"{a: 1, c: 2}", "{b: 2, a: 3}", "{c: {a: 4}, b: 5}"};
    std::vector<std::string> outputs{"[1, 2]", "[3, 2]", "[5, {a: 4}]"};

    runExtractFieldPathsTest(paths, inputs, outputs);
}

TEST_F(ExtractFieldPathsStageTest, PathInArrayDoesNotExistTest) {
    std::vector<FieldPath> paths{"a.c"};
    std::vector<std::string> inputs{"{a: [{b: 1}]}", "{a: [{b: 2}]}"};
    std::vector<std::string> outputs{"[[]]", "[[]]"};

    runExtractFieldPathsTest(paths, inputs, outputs);
}

TEST_F(ExtractFieldPathsStageTest, OnlyTraverseOneArrayPerPathComponentTest) {
    std::vector<FieldPath> paths{"a.b"};
    std::vector<std::string> inputs{"{a: [{b: 1}]}", "{a: [[{b: 2}]]}", "{a: [[{b: 3}], {b: 4}]}"};
    std::vector<std::string> outputs{"[[1]]", "[[]]", "[[4]]"};

    runExtractFieldPathsTest(paths, inputs, outputs);
}

TEST_F(ExtractFieldPathsStageTest, SinglePathLeafArray) {
    std::vector<FieldPath> paths{"a"};
    std::vector<std::string> inputs{"{a: [1, 2]}", "{a: [{b: 3}]}", "{a: []}"};
    std::vector<std::string> outputs{"[[1, 2]]", "[[{b: 3}]]", "[[]]"};

    runExtractFieldPathsTest(paths, inputs, outputs);
}

TEST_F(ExtractFieldPathsStageTest, MultiPathLeafArrays) {
    std::vector<FieldPath> paths{"a", "b.c"};
    std::vector<std::string> inputs{"{a: [1, 2], b: [{c: [3, 4]}]}"};
    std::vector<std::string> outputs{"[[1, 2], [[3, 4]]]"};

    runExtractFieldPathsTest(paths, inputs, outputs);
}

}  // namespace mongo::sbe
