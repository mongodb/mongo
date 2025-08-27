/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/bson/bsonelement_comparator.h"
#include "mongo/bson/column/bsoncolumn.h"
#include "mongo/bson/column/bsoncolumnbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/sbe/sbe_block_test_helpers.h"
#include "mongo/db/exec/sbe/sbe_unittest.h"
#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/exec/sbe/values/bson_block.h"
#include "mongo/db/exec/sbe/values/cell_interface.h"
#include "mongo/db/exec/sbe/values/scalar_mono_cell_block.h"
#include "mongo/db/exec/sbe/values/ts_block.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"

namespace mongo::sbe {

using TypeTags = value::TypeTags;
using Value = value::Value;
using ColumnOpType = value::ColumnOpType;

// Tests that copyValue() behaves correctly when given a TypeTags::valueBlock. Uses MonoBlock as
// the concrete block type.
TEST(SbeBlockTest, SbeValueBlockTypeIsCopyable) {
    value::MonoBlock block(1, TypeTags::NumberInt32, value::bitcastFrom<int32_t>(123));

    auto [cpyTag, cpyValue] =
        value::copyValue(TypeTags::valueBlock, value::bitcastFrom<value::MonoBlock*>(&block));
    value::ValueGuard cpyGuard(cpyTag, cpyValue);
    ASSERT_EQ(cpyTag, TypeTags::valueBlock);
    auto cpy = value::getValueBlock(cpyValue);

    auto extracted = cpy->extract();
    ASSERT_EQ(extracted.count(), 1);
}

// Tests that copyValue() behaves correctly when given a TypeTags::valueBlock. Uses MonoBlock as
// the concrete block type.
TEST(SbeBlockTest, SbeCellBlockTypeIsCopyable) {
    value::ScalarMonoCellBlock block(1, TypeTags::NumberInt32, value::bitcastFrom<int32_t>(123));

    auto [cpyTag, cpyValue] = value::copyValue(
        TypeTags::cellBlock, value::bitcastFrom<value::ScalarMonoCellBlock*>(&block));
    value::ValueGuard cpyGuard(cpyTag, cpyValue);
    ASSERT_EQ(cpyTag, TypeTags::cellBlock);
    auto cpy = value::getCellBlock(cpyValue);

    auto& vals = cpy->getValueBlock();
    auto extracted = vals.extract();
    ASSERT_EQ(extracted.count(), 1);
}

namespace {
// Other types and helpers for testing.
struct PathTestCase {
    value::CellBlock::Path path;
    BSONObj filterValues;
    std::vector<int32_t> filterPosInfo;

    BSONObj projectValues;
};

// Converts a block to a BSON object of the form {"result": <array of values>}.
// Nothing values (which are valid in a block) are represented with bson NULL.
BSONObj blockToBsonArr(value::ValueBlock& block) {
    auto extracted = block.extract();
    BSONArrayBuilder arr;
    for (size_t i = 0; i < extracted.count(); ++i) {
        auto tag = extracted.tags()[i];
        auto val = extracted.vals()[i];

        BSONObjBuilder tmp;

        if (tag == TypeTags::Nothing) {
            // Use null as a fill value.
            tmp.appendNull("foo");
        } else {
            bson::appendValueToBsonObj(tmp, "foo", tag, val);
        }

        arr.append(tmp.asTempObj().firstElement());
    }
    return BSON("result" << arr.arr());
}

// Converts a vector of char storing char(1)/char(0) into an ascii string of '1' and '0'.
std::string posInfoToString(const std::vector<int32_t>& posInfo) {
    std::string out;
    for (auto c : posInfo) {
        out += std::to_string(c);
        out += " ";
    }
    return out;
}
}  // namespace

class BsonBlockDecodingTest : public mongo::unittest::Test {
public:
    void run() {
        auto base = static_cast<mongo::unittest::Test*>(this);

        // Run the tests using the block-based BSON implementation for extracting paths.
        base->run();
    }

    // Given an object that matches what we might see in the data field of a bucket, produce the
    // corresonding vector of objects.
    std::vector<BSONObj> columnsToObjs(BSONObj bsonColumns) {
        using mongo::BSONColumn;
        std::vector<BSONObj> objs;

        std::vector<StringData> fieldNames;
        std::vector<BSONColumn> columns;
        for (BSONElement elem : bsonColumns) {
            fieldNames.push_back(elem.fieldNameStringData());
            int dataLen;
            const char* data = elem.binDataClean(dataLen);
            columns.emplace_back(data, dataLen);
        }

        std::vector<BSONColumn::Iterator> iters;
        for (auto& col : columns) {
            iters.emplace_back(col.begin());
        }

        invariant(!iters.empty());
        const auto end = columns[0].end();
        while (iters[0] != end) {
            BSONObjBuilder builder;
            size_t i = 0;
            for (auto& iter : iters) {
                BSONElement elem = *iter;
                if (!elem.eoo()) {
                    builder.appendAs(elem, fieldNames[i]);
                }
                ++iter;
                ++i;
            }

            objs.push_back(builder.obj());
        }

        // Every BSONColumn should have the same number of elements.
        for (size_t i = 0; i < iters.size(); ++i) {
            invariant(iters[i] == columns[i].end());
        }

        return objs;
    }

    // Shred the bsons producing a time series "bucket"-like thing, which can be used by the TS
    // decoding implementation.
    BSONObj objsToColumns(const std::vector<BSONObj>& bsons) {
        StringMap<std::unique_ptr<BSONColumnBuilder<>>> shredMap;

        size_t bsonIdx = 0;
        for (auto&& bson : bsons) {
            // Keep track of which fields we visited for this bson, so we can pad the
            // unvisited fields with 'gaps'.
            StringDataSet fieldsVisited;
            for (auto elt : bson) {
                auto [it, inserted] = shredMap.insert(
                    std::pair(elt.fieldName(), std::make_unique<BSONColumnBuilder<>>()));

                if (inserted) {
                    // Backfill with missing values.
                    for (size_t i = 0; i < bsonIdx; ++i) {
                        it->second->append(BSONElement());
                    }
                }

                it->second->append(elt);
                fieldsVisited.insert(elt.fieldNameStringData());
            }

            // Fill in missings for fields not present in this document.
            for (auto& [k, v] : shredMap) {
                if (!fieldsVisited.count(k)) {
                    v->append(BSONElement());
                }
            }

            ++bsonIdx;
        }
        BSONObjBuilder dataFieldBuilder;
        for (auto& [fieldName, builder] : shredMap) {
            dataFieldBuilder.append(fieldName, builder->finalize());
        }
        return dataFieldBuilder.obj();
    }

    // Given a collection of documents that might appear in a bucket, produce the corresponding min
    // and max objects that would appear in the control block.
    std::pair<BSONObj, BSONObj> computeMinMax(const std::vector<BSONObj>& bsons) {
        StringMap<std::pair<BSONElement, BSONElement>> minMaxMap;

        SimpleStringDataComparator strCmpor;
        BSONElementComparator cmpor{BSONElementComparator::FieldNamesMode::kIgnore, &strCmpor};
        auto bsonCmp = [&cmpor](BSONElement a, BSONElement b) {
            return cmpor.evaluate(a < b);
        };

        for (auto&& bson : bsons) {
            for (auto elt : bson) {
                if (auto&& it = minMaxMap.find(elt.fieldName()); it != minMaxMap.end()) {
                    it->second.first = std::min(it->second.first, elt, bsonCmp);
                    it->second.second = std::max(it->second.first, elt, bsonCmp);
                } else {
                    minMaxMap.insert({elt.fieldName(), {elt, elt}});
                }
            }
        }

        BSONObjBuilder controlMinBuilder, controlMaxBuilder;
        for (auto& [_, minMax] : minMaxMap) {
            controlMinBuilder.append(minMax.first);
            controlMaxBuilder.append(minMax.second);
        }

        return std::pair{controlMinBuilder.obj(), controlMaxBuilder.obj()};
    }

    std::pair<std::vector<std::unique_ptr<value::TsBlock>>,
              std::vector<std::unique_ptr<value::CellBlock>>>
    extractCellBlocks(const std::vector<value::CellBlock::PathRequest>& paths,
                      const std::variant<std::vector<BSONObj>, BSONObj>& data) {
        // Get the de-blocked collection of BSON documents for the bucket.
        std::vector<BSONObj> bsons;
        if (auto* docs = std::get_if<std::vector<BSONObj>>(&data)) {
            // The caller gave us the documents directly, just use them.
            bsons = std::move(*docs);
        } else {
            // The caller gave us a compressed representation of the bucket data. Decompress them to
            // produce the logical collection of documents.
            BSONObj bsonColumns = std::get<BSONObj>(data);
            bsons = columnsToObjs(bsonColumns);
        }

        if (useTsImpl) {
            BSONObj dataField;
            if (auto obj = std::get_if<BSONObj>(&data)) {
                dataField = *obj;
            } else {
                dataField = objsToColumns(bsons);
            }

            BSONObj minData, maxData;
            std::tie(minData, maxData) = computeMinMax(bsons);

            // Store the bucket into a member variable so that the memory remains valid for the
            // rest of the test.
            _bucketStorage =
                BSON(timeseries::kBucketControlFieldName
                     << BSON(timeseries::kBucketControlCountFieldName
                             << (long long)bsons.size() << timeseries::kBucketControlMinFieldName
                             << minData << timeseries::kBucketControlMaxFieldName << maxData)
                     << timeseries::kBucketDataFieldName << dataField);

            // Now call into the time series extractor.
            value::TsBucketPathExtractor extractor(paths, "time");
            auto [n, storageBlocks, cellBlocks] = extractor.extractCellBlocks(_bucketStorage);
            return {std::move(storageBlocks), std::move(cellBlocks)};
        } else {
            return std::pair(std::vector<std::unique_ptr<value::TsBlock>>(),
                             value::extractCellBlocksFromBsons(paths, bsons));
        }
    }

    // Run the given testcases against the specified data. Data will be one of:
    // - A vector of the collection of documents represented by a bucket
    // - An object that is consistent with the data field of a bucket, e.g.
    //     {"topLevelField0": <BinData of type Column>, "topLevelField1": <BinData of type Column>}
    void testPaths(const std::vector<PathTestCase>& testCases,
                   const std::variant<std::vector<BSONObj>, BSONObj>& data,
                   bool skipProjectPath = false);

private:
    BSONObj _bucketStorage;

    bool useTsImpl = false;
};

void BsonBlockDecodingTest::testPaths(const std::vector<PathTestCase>& testCases,
                                      const std::variant<std::vector<BSONObj>, BSONObj>& data,
                                      bool skipProjectPath) {
    std::vector<value::CellBlock::PathRequest> pathReqs;
    for (auto& tc : testCases) {
        pathReqs.push_back(
            value::CellBlock::PathRequest(value::CellBlock::PathRequestType::kFilter, tc.path));

        if (!skipProjectPath) {
            pathReqs.push_back(value::CellBlock::PathRequest(
                value::CellBlock::PathRequestType::kProject, tc.path));
        }
    }

    auto [tsBlocks, cellBlocks] = extractCellBlocks(pathReqs, data);
    ASSERT_EQ(cellBlocks.size(), pathReqs.size());

    size_t idx = 0;
    const size_t step = skipProjectPath ? 1 : 2;
    for (auto& tc : testCases) {
        const auto filterIdx = idx * step;

        auto& valsOut = cellBlocks[filterIdx]->getValueBlock();
        auto numObj = blockToBsonArr(valsOut);

        ASSERT_TRUE(SimpleBSONObjComparator::kInstance.evaluate(numObj == tc.filterValues))
            << "Incorrect values for filter path " << pathReqs[filterIdx].toString() << " got "
            << numObj << " expected " << tc.filterValues;

        ASSERT_EQ(cellBlocks[filterIdx]->filterPositionInfo(), tc.filterPosInfo)
            << "Incorrect position info for filter path " << pathReqs[filterIdx].toString()
            << posInfoToString(cellBlocks[filterIdx]->filterPositionInfo())
            << " == " << posInfoToString(tc.filterPosInfo);

        if (!skipProjectPath) {
            const auto projectIdx = idx * step + 1;
            auto projectValues = blockToBsonArr(cellBlocks[projectIdx]->getValueBlock());
            ASSERT_TRUE(
                SimpleBSONObjComparator::kInstance.evaluate(projectValues == tc.projectValues))
                << "Incorrect values for project path " << pathReqs[projectIdx].toString()
                << " got " << projectValues << " expected " << tc.projectValues;
        }

        ++idx;
    }
}


using Get = value::CellBlock::Get;
using Traverse = value::CellBlock::Traverse;
using Id = value::CellBlock::Id;

TEST_F(BsonBlockDecodingTest, BSONDocumentBlockSimple) {
    std::vector<BSONObj> bsons{
        fromjson("{a:1, b:1}"),
        fromjson("{a:2, b:2}"),
        fromjson("{a:[3,4], b:3}"),
        fromjson("{x: 123}"),
        fromjson("{a:6}"),
    };

    std::vector<PathTestCase> tests{
        PathTestCase{.path = {Get{"a"}, Id{}},
                     .filterValues = fromjson("{result: [1,2,[3,4], null, 6]}"),
                     .filterPosInfo = {1, 1, 1, 1, 1},
                     .projectValues = fromjson("{result: [1,2,[3,4], null, 6]}")},

        PathTestCase{.path = {Get{"b"}, Id{}},
                     .filterValues = fromjson("{result: [1,2,3,null,null]}"),
                     .filterPosInfo = {1, 1, 1, 1, 1},
                     .projectValues = fromjson("{result: [1,2,3,null,null]}")}};
    testPaths(tests, bsons);
}

TEST_F(BsonBlockDecodingTest, BSONDocumentBlockMissings) {
    // A bunch of documents missing 'a' at the beginning and end.
    std::vector<BSONObj> bsons{
        fromjson("{OtherField: 1}"),
        fromjson("{OtherField: 1}"),
        fromjson("{a:1}"),
        fromjson("{a:2}"),
        fromjson("{a:[[3],4]}"),
        fromjson("{OtherField: 1}"),
        fromjson("{a:6}"),
        fromjson("{OtherField: 1}"),
    };

    std::vector<PathTestCase> tests{PathTestCase{
        .path = {Get{"a"}, Id{}},
        .filterValues = fromjson("{result: [null, null, 1,2,[[3],4], null, 6, null]}"),
        .filterPosInfo = {1, 1, 1, 1, 1, 1, 1, 1},
        .projectValues = fromjson("{result: [null, null, 1,2,[[3],4], null, 6, null]}")}};
    testPaths(tests, bsons);
}


TEST_F(BsonBlockDecodingTest, BSONDocumentBlockGetTraverse) {
    std::vector<BSONObj> bsons{
        fromjson("{a:[1,2,[3]], b:1}"),
        fromjson("{a:[4,5,[6]], b:2}"),
        fromjson("{a:[7,8,[999]], b:2}"),
        fromjson("{a:[10, 11, [12]], b:2}"),
    };

    std::vector<PathTestCase> tests{PathTestCase{
        .path = {Get{"a"}, Traverse{}, Id{}},
        .filterValues = fromjson("{result: [1,2,[3],4,5,[6],7,8,[999],10,11,[12]]}"),
        .filterPosInfo = {3, 3, 3, 3},
        .projectValues = fromjson("{result:[[1,2,[3]],[4,5,[6]],[7,8,[999]],[10,11,[12]]]}")}};
    testPaths(tests, bsons);
}

TEST_F(BsonBlockDecodingTest, BSONDocumentBlockSubfield) {
    std::vector<BSONObj> bsons{
        fromjson("{a: {b: 1}}"),
        fromjson("{a: {b: [999, 999]}}"),
        fromjson("{a: [{b: [2, 3]}, {b: [4, 5]}]}"),
        fromjson("{OtherField: 1}"),
        fromjson("{a: [{b: [[999]]}]}"),
    };

    const auto getFieldAResult = fromjson(
        "{result: [{b:1}, {b: [999,999]}, [{b: [2,3]}, {b: [4,5]}], "
        "null, [{b: [[999]]}]]}");

    std::vector<PathTestCase> tests{
        // Get(A)/Id case.
        PathTestCase{.path = {Get{"a"}, Id{}},
                     .filterValues = getFieldAResult,
                     .filterPosInfo = {1, 1, 1, 1, 1},
                     .projectValues = getFieldAResult},
        // Get(A)/Traverse/Id case.
        PathTestCase{.path = {Get{"a"}, Traverse{}, Id{}},
                     .filterValues =
                         fromjson("{result: [{b: 1}, {b: [999, 999]}, {b: [2,3]}, {b: [4,5]}, "
                                  "null, {b: [[999]]}]}"),
                     .filterPosInfo = {1, 1, 2, 1, 1},
                     .projectValues = getFieldAResult},
        // Get(A)/Traverse/Get(b)/Id case.
        PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"b"}, Id{}},
                     .filterValues =
                         fromjson("{result: [1, [999,999], [2,3], [4,5], null, [[999]]]}"),
                     .filterPosInfo = {1, 1, 2, 1, 1},
                     .projectValues =
                         fromjson("{result: [1, [999,999], [[2, 3], [4,5]], null, [[[999]]]]}")},
        // Get(a)/Get(b)/Id case. This case does not correspond to any MQL equivalent, but we
        // still want it to work.
        PathTestCase{.path = {Get{"a"}, Get{"b"}, Id{}},
                     .filterValues = fromjson("{result: [1, [999,999], null, null, null]}"),
                     .filterPosInfo = {1, 1, 1, 1, 1},
                     .projectValues = fromjson("{result: [1, [999,999], null, null, null]}")},
        // Get(A)/Traverse/Get(b)/Traverse/Id case.
        PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"b"}, Traverse{}, Id{}},
                     .filterValues = fromjson("{result: [1,999,999,2,3,4,5, null, [999]]}"),
                     .filterPosInfo = {1, 2, 4, 1, 1},
                     .projectValues =
                         fromjson("{result: [1, [999,999], [[2, 3], [4,5]], null, [[[999]]]]}")}};
    testPaths(tests, bsons);
}

TEST_F(BsonBlockDecodingTest, DoublyNestedArrays) {
    std::vector<BSONObj> bsons{
        fromjson("{a: [[{b: 1}], {b:2}]}"),
        fromjson("{a: [{b: [[3,4]]}, {b: [5, 6]}, {b:7}]}"),
    };

    const auto getFieldAResult =
        fromjson("{result: [[[{b: 1}], {b:2}], [{b: [[3,4]]}, {b: [5, 6]}, {b:7}]]}");

    std::vector<PathTestCase> tests{
        // Get(A)/Id case.
        PathTestCase{.path = {Get{"a"}, Id{}},
                     .filterValues = getFieldAResult,
                     .filterPosInfo = {1, 1},
                     .projectValues = getFieldAResult},
        // Get(A)/Traverse/Id case.
        PathTestCase{.path = {Get{"a"}, Traverse{}, Id{}},
                     .filterValues =
                         fromjson("{result: [[{b: 1}], {b:2}, {b: [[3,4]]}, {b: [5, 6]}, {b:7}]}"),
                     .filterPosInfo = {2, 3},
                     .projectValues = getFieldAResult},
        // Get(A)/Traverse/Get(b)/Id case.
        PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"b"}, Id{}},
                     // We expect that objects within doubly nested arrays (e.g. {b:1}) are NOT
                     // traversed. Arrays directly nested within arrays (e.g. [3,4]) are treated
                     // as "blobs" and are not traversed.
                     .filterValues = fromjson("{result: [2, [[3,4]], [5, 6], 7]}"),
                     .filterPosInfo = {1, 3},
                     .projectValues = fromjson("{result: [[2], [[[3,4]], [5, 6], 7]]}")},
        // Get(a)/Get(b)/Id case. This case does not correspond to any MQL equivalent, but we
        // still want it to work.
        PathTestCase{.path = {Get{"a"}, Get{"b"}, Id{}},
                     .filterValues = fromjson("{result: [null, null]}"),
                     .filterPosInfo = {1, 1},
                     .projectValues = fromjson("{result: [null, null]}")},
        // Get(A)/Traverse/Get(b)/Traverse/Id case.
        PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"b"}, Traverse{}, Id{}},
                     .filterValues = fromjson("{result: [2, [3,4], 5, 6, 7]}"),
                     .filterPosInfo = {1, 4},
                     .projectValues = fromjson("{result: [[2], [[[3,4]], [5, 6], 7]]}")}};
    testPaths(tests, bsons);
}

TEST_F(BsonBlockDecodingTest, BSONDocumentEmptyArrays) {
    {
        std::vector<BSONObj> bsons{
            fromjson("{a: {b: 1}}"),
            fromjson("{a: {b: []}}"),
            fromjson("{a: {b: [2, 3]}}"),
        };
        std::vector<PathTestCase> tests{
            PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"b"}, Id{}},
                         .filterValues = fromjson("{result: [1, [], [2,3]]}"),
                         .filterPosInfo = {1, 1, 1},
                         .projectValues = fromjson("{result: [1, [], [2,3]]}")},
            PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"b"}, Traverse{}, Id{}},
                         .filterValues = fromjson("{result: [1, 2,3]}"),
                         .filterPosInfo = {1, 0, 2},
                         .projectValues = fromjson("{result: [1, [], [2,3]]}")},
        };
        testPaths(tests, bsons);
    }

    {
        std::vector<BSONObj> bsons{
            fromjson("{a: [{b: []}, {b: [1,2]}, {b: []}]}"),
            fromjson("{a: [{b: []}, {b: []}, {b: []}]}"),
            fromjson("{a: {b: [3, 4]}}"),
        };
        std::vector<PathTestCase> tests{
            PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"b"}, Id{}},
                         .filterValues = fromjson("{result: [[], [1, 2], [], [], [], [], [3, 4]]}"),
                         .filterPosInfo = {3, 3, 1},
                         .projectValues =
                             fromjson("{result: [[[], [1, 2], []], [[], [], []], [3, 4]]}")},
            PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"b"}, Traverse{}, Id{}},
                         .filterValues = fromjson("{result: [1,2,3,4]}"),
                         .filterPosInfo = {2, 0, 2},
                         .projectValues =
                             fromjson("{result: [[[], [1, 2], []], [[], [], []], [3, 4]]}")},

        };
        testPaths(tests, bsons);
    }
}

TEST_F(BsonBlockDecodingTest, BSONDocumentBlockFieldDoesNotExist) {
    std::vector<BSONObj> bsons{
        fromjson("{a: {b: 1}}"),

        // These documents have no values at a.b, however MQL semantics demand
        // they must know about the array in 'a' for "$a.b" but not for $match.
        //
        // This tests that the information kept in Get A Traverse Get B Traverse projection
        // position info is enough to recover the fact that 'a' had an array.
        fromjson("{a: [{OtherField: 123}]}"),
        fromjson("{a: [1, 2, 3]}"),

        // These have a.b values with the missing value in the middle.
        fromjson("{a: [{b: [4, 5]}, {b: []}, {b: [6, 7]}]}")};


    std::vector<PathTestCase> tests{
        PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"b"}, Traverse{}, Id{}},
                     .filterValues = fromjson("{result: [1, null, null, 4, 5, 6, 7]}"),
                     .filterPosInfo = {1, 1, 1, 4},
                     .projectValues = fromjson("{result: [1, [], [], [[4, 5], [], [6, 7]]]}")}};
    testPaths(tests, bsons);
}

TEST_F(BsonBlockDecodingTest, PathBasedDecompressionBasic) {
    std::vector<BSONObj> bsons{
        fromjson("{a: {b: 0, c: 10}}"),
        fromjson("{a: {b: 1, c: 20}}"),
        fromjson("{a: {b: 2, c: 30}}"),
        fromjson("{a: {b: 3, c: 40}}"),
    };

    // These test cases can use path-based decompression (when the feature flag is enabled) for both
    // the filter paths and the project paths since the bsons contain no arrays.
    std::vector<PathTestCase> tests{
        PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"b"}, Traverse{}, Id{}},
                     .filterValues = fromjson("{result: [0, 1, 2, 3]}"),
                     .filterPosInfo = {1, 1, 1, 1},
                     .projectValues = fromjson("{result: [0, 1, 2, 3]}")},
        PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"c"}, Traverse{}, Id{}},
                     .filterValues = fromjson("{result: [10, 20, 30, 40]}"),
                     .filterPosInfo = {1, 1, 1, 1},
                     .projectValues = fromjson("{result: [10, 20, 30, 40]}")},
        PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"d"}, Traverse{}, Id{}},
                     .filterValues = fromjson("{result: [null, null, null, null]}"),
                     .filterPosInfo = {1, 1, 1, 1},
                     .projectValues = fromjson("{result: [null, null, null, null]}")}};
    testPaths(tests, bsons);
}

TEST_F(BsonBlockDecodingTest, PathBasedDecompressionOneElementArrays) {
    std::vector<BSONObj> bsons{
        fromjson("{a: {b: 0, c: [10]}}"),
        fromjson("{a: {b: 1, c: [20]}}"),
        fromjson("{a: {b: 2, c: [30]}}"),
        fromjson("{a: {b: 3, c: [40]}}"),
    };

    // In these tests, traversing "c" is interesting:
    //   - If we omit the project paths, then we can use fast path-based decompression for the
    //     filter paths.
    //   - If we don't omit the projects, we need to use the normal (slow) path and won't use
    //     path-based decompression at all, since we need to decompress and shred the whole column
    //     anyways.
    // Therefore, we run the tests with and wihtout skipping the project paths, below.
    std::vector<PathTestCase> tests{
        PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"b"}, Traverse{}, Id{}},
                     .filterValues = fromjson("{result: [0, 1, 2, 3]}"),
                     .filterPosInfo = {1, 1, 1, 1},
                     .projectValues = fromjson("{result: [0, 1, 2, 3]}")},
        PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"c"}, Traverse{}, Id{}},
                     .filterValues = fromjson("{result: [10, 20, 30, 40]}"),
                     .filterPosInfo = {1, 1, 1, 1},
                     .projectValues = fromjson("{result: [[10], [20], [30], [40]]}")}};
    testPaths(tests, bsons);
    testPaths(tests, bsons, true /* skip project paths */);
}

TEST_F(BsonBlockDecodingTest, PathBasedDecompressionDoublyNestedArrays) {
    std::vector<BSONObj> bsons{
        fromjson("{a: [{b: [[3]], c: 4}]}"),
        fromjson("{a: [{b: [[30]], c: 5}]}"),
        fromjson("{a: [{b: [[300]], c: 6}]}"),
        fromjson("{a: [{b: [[3000]], c: 7}]}"),
    };

    std::vector<PathTestCase> tests{
        PathTestCase{
            .path = {Get{"a"}, Traverse{}, Get{"b"}, Traverse{}, Id{}},
            .filterValues = fromjson("{result: [[3], [30], [300], [3000]]}"),
            .filterPosInfo = {1, 1, 1, 1},
            .projectValues = fromjson("{result: [[[[3]]], [[[30]]], [[[300]]], [[[3000]]]]}"),
        },
    };

    testPaths(tests, bsons);
    testPaths(tests, bsons, true /* skip project paths */);
}

TEST_F(BsonBlockDecodingTest, PathBasedDecompressionEmptyObject) {
    // Include different combinations of missing elements, empty objects, and objects with empty
    // sub-objects.
    std::vector<BSONObj> bsons{
        fromjson("{}"),                      // 0
        fromjson("{tlf: {}}"),               // 1
        fromjson("{}"),                      // 2
        fromjson("{tlf: {a: 100, b: {}}}"),  // 3
        fromjson("{tlf: {a: 200, b: {}}}"),  // 4
        fromjson("{}"),                      // 5
        fromjson("{tlf: {a: 300, b: {}}}"),  // 6
        fromjson("{tlf: {a: 400, b: {}}}"),  // 7
        fromjson("{}"),                      // 8
        fromjson("{tlf: {}}"),               // 9
        fromjson("{}"),                      // 10
    };

    // Test the paths for the root of 'tlf', the scalars at 'a' and the empty objects at 'b'.
    std::vector<PathTestCase> tests{
        PathTestCase{.path = {Get{"tlf"}, Traverse{}, Id{}},
                     .filterValues = fromjson("{result: ["
                                              "null, {}, null, "
                                              "{a: 100, b: {}}, {a: 200, b: {}}, "
                                              "null, "
                                              "{a: 300, b: {}}, {a: 400, b: {}}, "
                                              "null, {}, null"
                                              "]}"),
                     .filterPosInfo = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
                     .projectValues = fromjson("{result: ["
                                               "null, {}, null, "
                                               "{a: 100, b: {}}, {a: 200, b: {}}, "
                                               "null, "
                                               "{a: 300, b: {}}, {a: 400, b: {}}, "
                                               "null, {}, null"
                                               "]}")},
        PathTestCase{.path = {Get{"tlf"}, Traverse{}, Get{"a"}, Traverse{}, Id{}},
                     .filterValues = fromjson("{result: ["
                                              "null, null, null, "
                                              "100, 200, "
                                              "null, "
                                              "300, 400, "
                                              "null, null, null"
                                              "]}"),
                     .filterPosInfo = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
                     .projectValues = fromjson("{result: ["
                                               "null, null, null, "
                                               "100, 200, "
                                               "null, "
                                               "300, 400, "
                                               "null, null, null"
                                               "]}")},
        PathTestCase{.path = {Get{"tlf"}, Traverse{}, Get{"b"}, Traverse{}, Id{}},
                     .filterValues = fromjson("{result: ["
                                              "null, null, null, "
                                              "{}, {}, "
                                              "null, "
                                              "{}, {}, "
                                              "null, null, null"
                                              "]}"),
                     .filterPosInfo = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
                     .projectValues = fromjson("{result: ["
                                               "null, null, null, "
                                               "{}, {}, "
                                               "null, "
                                               "{}, {}, "
                                               "null, null, null"
                                               "]}")},
    };
    // Test each path separately so that we get as much coverage for path-based decompression as
    // possible.
    for (const PathTestCase& tc : tests) {
        testPaths({tc}, bsons);
        testPaths({tc}, bsons, true /* skip project paths */);
    }
}

TEST_F(BsonBlockDecodingTest, PathBasedDecompressionOneBadPath) {
    std::vector<BSONObj> bsons{
        fromjson("{a: {b: [{c: 10}], d: {e: [20, 30]}}}"),
        fromjson("{a: {b: [{c: 11}], d: {e: [21, 31]}}}"),
        fromjson("{a: {b: [{c: 12}], d: {e: [22, 32]}}}"),
        fromjson("{a: {b: [{c: 13}], d: {e: [23, 33]}}}"),
    };

    // This tests the case where one path can use path-based decompression (the first), but the
    // second cannot because it needs to get interleaved elements from two scalar streams. In this
    // case we don't use path-based compression since we need to decompress and shred the whole
    // BSONColumn anyways.
    std::vector<PathTestCase> tests{
        PathTestCase{
            .path = {Get{"a"}, Traverse{}, Get{"b"}, Traverse{}, Get{"c"}, Traverse{}, Id{}},
            .filterValues = fromjson("{result: [10, 11, 12, 13]}"),
            .filterPosInfo = {1, 1, 1, 1},
            .projectValues = fromjson("{result: [[10], [11], [12], [13]]}")},
        PathTestCase{
            .path = {Get{"a"}, Traverse{}, Get{"d"}, Traverse{}, Get{"e"}, Traverse{}, Id{}},
            .filterValues = fromjson("{result: [20, 30, 21, 31, 22, 32, 23, 33]}"),
            .filterPosInfo = {2, 2, 2, 2},
            .projectValues = fromjson("{result: [[20, 30], [21, 31], [22, 32], [23, 33]]}")},
    };
    testPaths(tests, bsons);
    // Path-based decompression not applied even when only extracting filter paths because of the
    // array traversal in the second path.
    testPaths(tests, bsons, true /* skip project paths */);
}

TEST_F(BsonBlockDecodingTest, PathBasedLegacyInterleaved) {
    // base 64-encoded BSONColumn of objects:
    //   {a: [10], b: 20}
    //   {a: [10], b: 20}
    //   {a: [10], b: 20}
    //   {a: [10], b: 20}
    // Legacy interleaved encoding is used, meaning we will get an error if we try to use path-based
    // decompression to decompress the scalar 10s. We can use path-based decompression for the 20s
    // though.
    StringData b64Col = "8BsAAAAEYQAMAAAAEDAACgAAAAAQYgAUAAAAAIALAAAAAAAAAIALAAAAAAAAAAAA"_sd;

    std::string compressedCol = base64::decode(b64Col);
    BSONBinData bd{
        compressedCol.data(), static_cast<int>(compressedCol.size()), BinDataType::Column};
    BSONObjBuilder builder;
    builder.append("fld"_sd, bd);
    BSONObj bucketData = builder.obj();

    std::vector<PathTestCase> tests{
        // We won't use path-based decompression for this path. If we did, the BSONColumn layer
        // would produce an error, "unknown element".
        PathTestCase{.path = {Get{"fld"}, Traverse{}, Get{"a"}, Traverse{}, Id{}},
                     .filterValues = fromjson("{result: [10, 10, 10, 10]}"),
                     .filterPosInfo = {1, 1, 1, 1},
                     .projectValues = fromjson("{result: [[10], [10], [10], [10]]}")},
        // We will use path-based decompression for this path.
        PathTestCase{.path = {Get{"fld"}, Traverse{}, Get{"b"}, Traverse{}, Id{}},
                     .filterValues = fromjson("{result: [20, 20, 20, 20]}"),
                     .filterPosInfo = {1, 1, 1, 1},
                     .projectValues = fromjson("{result: [20, 20, 20, 20]}")},
    };

    for (auto& test : tests) {
        testPaths({test}, bucketData);
        testPaths({test}, bucketData, true);
    }
}

class ValueBlockTest : public mongo::unittest::Test {
public:
    ValueBlockTest() = default;
};

static const auto testOp1 =
    value::makeColumnOp<ColumnOpType::kNoFlags>([](TypeTags tag, Value val) {
        return std::pair(TypeTags::Boolean, value::bitcastFrom<bool>(value::isString(tag)));
    });

static const auto testOp2 =
    value::makeColumnOp<ColumnOpType::kNoFlags>([](TypeTags tag, Value val) {
        if (tag == TypeTags::NumberDouble) {
            double d = value::bitcastTo<double>(val);
            return std::pair(TypeTags::Boolean, value::bitcastFrom<bool>(d >= 5.0));
        } else {
            return std::pair(TypeTags::Nothing, Value{0u});
        }
    });

static const auto testOp3 = value::makeColumnOp<ColumnOpType::kNoFlags>(
    [](TypeTags tag, Value val) {
        return std::pair(TypeTags::Boolean, value::bitcastFrom<bool>(tag != TypeTags::Nothing));
    },
    [](TypeTags tag, const Value* vals, TypeTags* outTags, Value* outVals, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            outTags[i] = TypeTags::Boolean;
            outVals[i] = value::bitcastFrom<bool>(tag != TypeTags::Nothing);
        }
    });

static const auto testOp4 = value::makeColumnOp<ColumnOpType::kNoFlags>(
    [](TypeTags tag, Value val) {
        if (tag == TypeTags::NumberDouble) {
            double d = value::bitcastTo<double>(val);
            return std::pair(TypeTags::NumberDouble, value::bitcastFrom<double>(d * 2.0 + 1.0));
        } else {
            return std::pair(TypeTags::Nothing, Value{0u});
        }
    },
    [](TypeTags tag, const Value* vals, TypeTags* outTags, Value* outVals, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            if (tag == TypeTags::NumberDouble) {
                double d = value::bitcastTo<double>(vals[i]);
                outTags[i] = TypeTags::NumberDouble;
                outVals[i] = value::bitcastFrom<double>(d * 2.0 + 1.0);
            } else {
                outTags[i] = TypeTags::Nothing;
                outVals[i] = Value{0u};
            }
        }
    });

// Test HeterogenousBlock::map().
TEST_F(ValueBlockTest, HeterogeneousBlockMap) {
    auto block = std::make_unique<value::HeterogeneousBlock>();

    block->push_back(TypeTags::Nothing, Value{0u});
    block->push_back(TypeTags::Boolean, value::bitcastFrom<bool>(false));
    block->push_back(TypeTags::NumberDouble, value::bitcastFrom<double>(3.0));
    block->push_back(TypeTags::NumberDouble, value::bitcastFrom<double>(10.0));

    auto [strTag, strVal] = value::makeNewString("not a small string");
    block->push_back(strTag, strVal);

    auto outBlock1 = block->map(testOp1);
    auto output1 = blockToBsonArr(*outBlock1);
    ASSERT_BSONOBJ_EQ(output1, fromjson("{result: [false, false, false, false, true]}"));

    auto outBlock2 = block->map(testOp2);
    auto output2 = blockToBsonArr(*outBlock2);
    ASSERT_BSONOBJ_EQ(output2, fromjson("{result: [null, null, false, true, null]}"));

    auto outBlock3 = block->map(testOp3);
    auto output3 = blockToBsonArr(*outBlock3);
    ASSERT_BSONOBJ_EQ(output3, fromjson("{result: [false, true, true, true, true]}"));

    auto outBlock4 = block->map(testOp4);
    auto output4 = blockToBsonArr(*outBlock4);
    ASSERT_BSONOBJ_EQ(output4, fromjson("{result: [null, null, 7.0, 21.0, null]}"));
}

// Test HomogeneousBlock::map().
TEST_F(ValueBlockTest, HomogeneousBlockMap) {
    auto block = std::make_unique<value::DoubleBlock>();

    block->pushNothing();
    block->push_back(3.0);
    block->push_back(10.0);
    block->pushNothing();

    auto outBlock1 = block->map(testOp1);
    auto output1 = blockToBsonArr(*outBlock1);
    ASSERT_BSONOBJ_EQ(output1, fromjson("{result: [false, false, false, false]}"));

    auto outBlock2 = block->map(testOp2);
    auto output2 = blockToBsonArr(*outBlock2);
    ASSERT_BSONOBJ_EQ(output2, fromjson("{result: [null, false, true, null]}"));

    auto outBlock3 = block->map(testOp3);
    auto output3 = blockToBsonArr(*outBlock3);
    ASSERT_BSONOBJ_EQ(output3, fromjson("{result: [false, true, true, false]}"));

    auto outBlock4 = block->map(testOp4);
    auto output4 = blockToBsonArr(*outBlock4);
    ASSERT_BSONOBJ_EQ(output4, fromjson("{result: [null, 7.0, 21.0, null]}"));
}

// Test MonoBlock::map().
TEST_F(ValueBlockTest, MonoBlockMap) {
    {
        auto block = std::make_unique<value::MonoBlock>(3, TypeTags::Nothing, Value{0u});

        auto outBlock1 = block->map(testOp1);
        auto output1 = blockToBsonArr(*outBlock1);
        ASSERT_BSONOBJ_EQ(output1, fromjson("{result: [false, false, false]}"));

        auto outBlock2 = block->map(testOp2);
        auto output2 = blockToBsonArr(*outBlock2);
        ASSERT_BSONOBJ_EQ(output2, fromjson("{result: [null, null, null]}"));

        auto outBlock3 = block->map(testOp3);
        auto output3 = blockToBsonArr(*outBlock3);
        ASSERT_BSONOBJ_EQ(output3, fromjson("{result: [false, false, false]}"));

        auto outBlock4 = block->map(testOp4);
        auto output4 = blockToBsonArr(*outBlock4);
        ASSERT_BSONOBJ_EQ(output4, fromjson("{result: [null, null, null]}"));
    }

    {
        auto block = std::make_unique<value::MonoBlock>(
            3, TypeTags::NumberDouble, value::bitcastFrom<double>(3.0));

        auto outBlock1 = block->map(testOp1);
        auto output1 = blockToBsonArr(*outBlock1);
        ASSERT_BSONOBJ_EQ(output1, fromjson("{result: [false, false, false]}"));

        auto outBlock2 = block->map(testOp2);
        auto output2 = blockToBsonArr(*outBlock2);
        ASSERT_BSONOBJ_EQ(output2, fromjson("{result: [false, false, false]}"));

        auto outBlock3 = block->map(testOp3);
        auto output3 = blockToBsonArr(*outBlock3);
        ASSERT_BSONOBJ_EQ(output3, fromjson("{result: [true, true, true]}"));

        auto outBlock4 = block->map(testOp4);
        auto output4 = blockToBsonArr(*outBlock4);
        ASSERT_BSONOBJ_EQ(output4, fromjson("{result: [7.0, 7.0, 7.0]}"));
    }

    {
        auto block = std::make_unique<value::MonoBlock>(
            2, TypeTags::NumberDouble, value::bitcastFrom<double>(10.0));

        auto outBlock1 = block->map(testOp1);
        auto output1 = blockToBsonArr(*outBlock1);
        ASSERT_BSONOBJ_EQ(output1, fromjson("{result: [false, false]}"));

        auto outBlock2 = block->map(testOp2);
        auto output2 = blockToBsonArr(*outBlock2);
        ASSERT_BSONOBJ_EQ(output2, fromjson("{result: [true, true]}"));

        auto outBlock3 = block->map(testOp3);
        auto output3 = blockToBsonArr(*outBlock3);
        ASSERT_BSONOBJ_EQ(output3, fromjson("{result: [true, true]}"));

        auto outBlock4 = block->map(testOp4);
        auto output4 = blockToBsonArr(*outBlock4);
        ASSERT_BSONOBJ_EQ(output4, fromjson("{result: [21.0, 21.0]}"));
    }

    {
        auto [strTag, strVal] = value::makeNewString("not a small string");
        auto block = std::make_unique<value::MonoBlock>(4, strTag, strVal);

        auto outBlock1 = block->map(testOp1);
        auto output1 = blockToBsonArr(*outBlock1);
        ASSERT_BSONOBJ_EQ(output1, fromjson("{result: [true, true, true, true]}"));

        auto outBlock2 = block->map(testOp2);
        auto output2 = blockToBsonArr(*outBlock2);
        ASSERT_BSONOBJ_EQ(output2, fromjson("{result: [null, null, null, null]}"));

        auto outBlock3 = block->map(testOp3);
        auto output3 = blockToBsonArr(*outBlock3);
        ASSERT_BSONOBJ_EQ(output3, fromjson("{result: [true, true, true, true]}"));

        auto outBlock4 = block->map(testOp4);
        auto output4 = blockToBsonArr(*outBlock4);
        ASSERT_BSONOBJ_EQ(output4, fromjson("{result: [null, null, null, null]}"));
    }
}

// Test ValueBlock::defaultMapImpl().
TEST_F(ValueBlockTest, TestBlockMap) {
    auto block = std::make_unique<TestBlock>();

    block->push_back(TypeTags::Nothing, Value{0u});
    block->push_back(TypeTags::Boolean, value::bitcastFrom<bool>(false));
    block->push_back(TypeTags::NumberDouble, value::bitcastFrom<double>(3.0));
    block->push_back(TypeTags::NumberDouble, value::bitcastFrom<double>(10.0));

    auto [strTag, strVal] = value::makeNewString("not a small string");
    block->push_back(strTag, strVal);

    auto outBlock1 = block->map(testOp1);
    auto output1 = blockToBsonArr(*outBlock1);
    ASSERT_BSONOBJ_EQ(output1, fromjson("{result: [false, false, false, false, true]}"));

    auto outBlock2 = block->map(testOp2);
    auto output2 = blockToBsonArr(*outBlock2);
    ASSERT_BSONOBJ_EQ(output2, fromjson("{result: [null, null, false, true, null]}"));

    auto outBlock3 = block->map(testOp3);
    auto output3 = blockToBsonArr(*outBlock3);
    ASSERT_BSONOBJ_EQ(output3, fromjson("{result: [false, true, true, true, true]}"));

    auto outBlock4 = block->map(testOp4);
    auto output4 = blockToBsonArr(*outBlock4);
    ASSERT_BSONOBJ_EQ(output4, fromjson("{result: [null, null, 7.0, 21.0, null]}"));
}

// Test monotonic shortcut in ValueBlock::defaultMapImpl().
static const auto testOp5 = value::makeColumnOp<ColumnOpType::kMonotonic>(
    [](TypeTags tag, Value val) { return value::makeBigString("fake result from map"); });

TEST_F(ValueBlockTest, TestBlockMapFast) {
    auto block = std::make_unique<TestBlock>();

    auto [strTag1, strVal1] = value::makeNewString("not a small string");
    block->push_back(strTag1, strVal1);
    auto [strTag2, strVal2] = value::makeNewString("a slightly longer string");
    block->push_back(strTag2, strVal2);

    // Verify that the fast path won't be taken if we don't have a lower and upper bound for the
    // block.
    ASSERT_EQ(block->mapMonotonicFastPath(testOp5), nullptr);
    block->setMin(strTag2, strVal2);
    ASSERT_EQ(block->mapMonotonicFastPath(testOp5), nullptr);
    block->setMax(strTag1, strVal1);

    // Verify that correct results are produced.
    auto outBlock1 = block->mapMonotonicFastPath(testOp5);
    auto output1 = blockToBsonArr(*outBlock1);
    ASSERT_BSONOBJ_EQ(output1,
                      fromjson("{result: ['fake result from map', 'fake result from map']}"));

    // Verify that it actually used the fast path.
    ASSERT_NE(block->mapMonotonicFastPath(testOp5), nullptr);

    // Verify that the fast path won't be taken if the block isn't dense.
    block->push_back(makeNothing());
    ASSERT_EQ(block->mapMonotonicFastPath(testOp5), nullptr);
}

TEST_F(ValueBlockTest, EmptyBlockMapTest) {
    auto emptyTestBlock = std::make_unique<TestBlock>();
    auto emptyHeterogeneousBlock = std::make_unique<value::HeterogeneousBlock>(
        std::vector<value::TypeTags>{} /* tags */, std::vector<value::Value>{} /* vals */);
    auto emptyIntBlock =
        std::make_unique<value::Int32Block>(std::vector<value::Value>{} /* vals */);

    std::vector<std::unique_ptr<value::ValueBlock>> emptyBlocks;
    emptyBlocks.push_back(std::move(emptyTestBlock));           // ValueBlock::defaultMapImpl
    emptyBlocks.push_back(std::move(emptyHeterogeneousBlock));  // HeterogeneousBlock::map
    emptyBlocks.push_back(std::move(emptyIntBlock));            // HomogeneousBlock::map
    for (size_t i = 0; i < emptyBlocks.size(); ++i) {
        auto outBlock = emptyBlocks[i]->map(testOp5);
        auto output = blockToBsonArr(*outBlock);
        ASSERT_BSONOBJ_EQ(output, fromjson("{result: []}"));
    }
}

// Test ValueBlock::tokenize().
TEST_F(ValueBlockTest, TestTokenize) {
    auto block = std::make_unique<TestBlock>();

    auto [tag1, val1] = value::makeNewString("foofoofoo"_sd);
    block->push_back(tag1, val1);
    auto [tag2, val2] = value::makeNewString("bar"_sd);  // StringSmall
    block->push_back(tag2, val2);
    auto [tag3, val3] = value::makeNewString("bazbazbaz"_sd);
    block->push_back(tag3, val3);
    auto [tag4, val4] = value::makeNewString("bar"_sd);  // StringSmall
    block->push_back(tag4, val4);
    auto [tag5, val5] = value::makeNewString("bar"_sd);  // StringSmall
    block->push_back(tag5, val5);
    block->push_back(TypeTags::NumberInt32, value::bitcastFrom<int32_t>(999));
    block->push_back(TypeTags::Nothing, Value{0u});
    auto [tag6, val6] = value::makeNewString("foofoofoo"_sd);
    block->push_back(tag6, val6);
    block->push_back(TypeTags::Nothing, Value{0u});

    auto [outTokens, outIdxs] = block->tokenize();
    ASSERT_EQ(outTokens->count(), 5);

    auto outTokensBson = blockToBsonArr(*outTokens);
    ASSERT_BSONOBJ_EQ(outTokensBson,
                      fromjson("{result: [\"foofoofoo\", \"bar\", \"bazbazbaz\", 999, null]}"));

    std::vector<size_t> expIdxs{0, 1, 2, 1, 1, 3, 4, 0, 4};
    ASSERT_EQ(outIdxs, expIdxs);
}

// Test MonoBlock::tokenize().
TEST_F(ValueBlockTest, MonoBlockTokenize) {
    {
        auto [strTag, strVal] = value::makeNewString("not a small string"_sd);
        auto block = std::make_unique<value::MonoBlock>(4, strTag, strVal);

        auto [outTokens, outIdxs] = block->tokenize();
        ASSERT_EQ(outTokens->count(), 1);

        auto outTokensBson = blockToBsonArr(*outTokens);
        ASSERT_BSONOBJ_EQ(outTokensBson, fromjson("{result: [\"not a small string\"]}"));

        std::vector<size_t> expIdxs{0, 0, 0, 0};
        ASSERT_EQ(outIdxs, expIdxs);
    }

    {
        auto block = std::make_unique<value::MonoBlock>(4, TypeTags::Nothing, Value{0u});

        auto [outTokens, outIdxs] = block->tokenize();
        ASSERT_EQ(outTokens->count(), 1);

        auto outTokensBson = blockToBsonArr(*outTokens);
        ASSERT_BSONOBJ_EQ(outTokensBson, fromjson("{result: [null]}"));

        std::vector<size_t> expIdxs{0, 0, 0, 0};
        ASSERT_EQ(outIdxs, expIdxs);
    }
}

// Int32Block::tokenize(), Int64Block::tokenize(), and DateBlock::tokenize() are effectively
// identical so they are combined into 1 test.
TEST_F(ValueBlockTest, IntBlockTokenize) {
    {
        // Test that first token is Nothing for non-dense blocks.
        auto block = std::make_unique<value::Int32Block>();

        block->push_back(value::bitcastFrom<int32_t>(2));
        block->pushNothing();
        block->push_back(value::bitcastFrom<int32_t>(0));
        block->push_back(value::bitcastFrom<int32_t>(2));
        block->pushNothing();
        block->pushNothing();
        block->push_back(value::bitcastFrom<int32_t>(1));
        block->pushNothing();
        block->pushNothing();

        auto [outTokens, outIdxs] = block->tokenize();
        ASSERT_EQ(outTokens->count(), 4);

        auto outTokensBson = blockToBsonArr(*outTokens);
        ASSERT_BSONOBJ_EQ(outTokensBson, fromjson("{result: [null, 2, 0, 1]}"));

        std::vector<size_t> expIdxs{1, 0, 2, 1, 0, 0, 3, 0, 0};
        ASSERT_EQ(outIdxs, expIdxs);
    }

    {
        // Test on leading Nothing.
        auto block = std::make_unique<value::Int32Block>();

        block->pushNothing();
        block->push_back(value::bitcastFrom<int32_t>(0));

        auto [outTokens, outIdxs] = block->tokenize();
        ASSERT_EQ(outTokens->count(), 2);

        auto outTokensBson = blockToBsonArr(*outTokens);
        ASSERT_BSONOBJ_EQ(outTokensBson, fromjson("{result: [null, 0]}"));

        std::vector<size_t> expIdxs{0, 1};
        ASSERT_EQ(outIdxs, expIdxs);
    }

    {
        // Test on dense input.
        auto block = std::make_unique<value::Int32Block>();

        block->push_back(value::bitcastFrom<int32_t>(2));
        block->push_back(value::bitcastFrom<int32_t>(0));
        block->push_back(value::bitcastFrom<int32_t>(2));
        block->push_back(value::bitcastFrom<int32_t>(1));

        auto [outTokens, outIdxs] = block->tokenize();
        ASSERT_EQ(outTokens->count(), 3);

        auto outTokensBson = blockToBsonArr(*outTokens);
        ASSERT_BSONOBJ_EQ(outTokensBson, fromjson("{result: [2, 0, 1]}"));

        std::vector<size_t> expIdxs{0, 1, 0, 2};
        ASSERT_EQ(outIdxs, expIdxs);
    }
}

// Test that default implementation still works for DoubleBlock's.
TEST_F(ValueBlockTest, DoubleBlockTokenize) {
    auto block = std::make_unique<value::DoubleBlock>();

    block->pushNothing();
    block->push_back(1.1);
    block->push_back(std::numeric_limits<double>::quiet_NaN());
    block->pushNothing();
    block->pushNothing();
    block->push_back(std::numeric_limits<double>::signaling_NaN());
    block->push_back(2.2);
    block->push_back(1.1);
    block->push_back(std::numeric_limits<double>::quiet_NaN());

    auto [outTokens, outIdxs] = block->tokenize();
    ASSERT_EQ(outTokens->count(), 4);

    auto outTokensBson = blockToBsonArr(*outTokens);
    ASSERT_BSONOBJ_EQ(outTokensBson, fromjson("{result: [null, 1.1, NaN, 2.2]}"));

    std::vector<size_t> expIdxs{0, 1, 2, 0, 0, 2, 3, 1, 2};
    ASSERT_EQ(outIdxs, expIdxs);
}

// Test that default implementation still works for BoolBlock's.
TEST_F(ValueBlockTest, BoolBlockTokenize) {
    auto block = std::make_unique<value::BoolBlock>();

    block->pushNothing();
    block->push_back(false);
    block->pushNothing();
    block->pushNothing();
    block->push_back(true);
    block->push_back(true);
    block->push_back(false);
    block->push_back(true);

    auto [outTokens, outIdxs] = block->tokenize();
    ASSERT_EQ(outTokens->count(), 3);

    auto outTokensBson = blockToBsonArr(*outTokens);
    ASSERT_BSONOBJ_EQ(outTokensBson, fromjson("{result: [null, false, true]}"));

    std::vector<size_t> expIdxs{0, 1, 0, 0, 2, 2, 1, 2};
    ASSERT_EQ(outIdxs, expIdxs);
}

template <typename BlockType, typename T>
std::unique_ptr<BlockType> makeArgMinMaxBlock(
    bool inclNothing = false, std::unique_ptr<BlockType> block = std::make_unique<BlockType>()) {
    if (inclNothing) {
        block->pushNothing();
        block->pushNothing();
    }
    block->push_back(static_cast<T>(1));
    block->push_back(static_cast<T>(-1));
    if (inclNothing) {
        block->pushNothing();
    }
    block->push_back(static_cast<T>(-3));
    block->push_back(static_cast<T>(4));
    block->push_back(static_cast<T>(-2));
    if (inclNothing) {
        block->pushNothing();
    }

    return std::move(block);
}

TEST_F(ValueBlockTest, ArgMinMaxGetAt) {
    {
        auto block = makeArgMinMaxBlock<value::Int32Block, int32_t>();
        ASSERT_EQ(block->argMin(), boost::optional<size_t>(2));
        ASSERT_EQ(block->argMax(), boost::optional<size_t>(3));

        block->push_back(std::numeric_limits<int32_t>::max());
        block->push_back(std::numeric_limits<int32_t>::min());
        ASSERT_EQ(block->argMin(), boost::optional<size_t>(6));
        ASSERT_EQ(block->argMax(), boost::optional<size_t>(5));

        auto blockWNothing = makeArgMinMaxBlock<value::Int32Block, int32_t>(true /* inclNothing */);
        ASSERT_EQ(blockWNothing->argMin(), boost::none);
        ASSERT_EQ(blockWNothing->argMax(), boost::none);
    }

    {
        auto block = makeArgMinMaxBlock<value::DoubleBlock, double>();
        ASSERT_EQ(block->argMin(), boost::optional<size_t>(2));
        ASSERT_EQ(block->argMax(), boost::optional<size_t>(3));

        block->push_back(std::numeric_limits<double>::max());
        // std::numeric_limits<double>::min() returns the smallest positive finite value.
        block->push_back(std::numeric_limits<double>::lowest());
        ASSERT_EQ(block->argMin(), boost::optional<size_t>(6));
        ASSERT_EQ(block->argMax(), boost::optional<size_t>(5));

        block->push_back(-1 * std::numeric_limits<double>::infinity());
        block->push_back(std::numeric_limits<double>::infinity());
        ASSERT_EQ(block->argMin(), boost::optional<size_t>(7));
        ASSERT_EQ(block->argMax(), boost::optional<size_t>(8));

        auto tempBlockWLHSNaN = std::make_unique<value::DoubleBlock>();
        tempBlockWLHSNaN->push_back(std::numeric_limits<double>::quiet_NaN());
        auto blockWLHSNaN = makeArgMinMaxBlock<value::DoubleBlock, double>(
            false /* inclNothing */, std::move(tempBlockWLHSNaN));
        ASSERT_EQ(blockWLHSNaN->argMin(), boost::optional<size_t>(0));
        ASSERT_EQ(blockWLHSNaN->argMax(), boost::optional<size_t>(4));

        auto blockWRHSNaN = makeArgMinMaxBlock<value::DoubleBlock, double>();
        blockWRHSNaN->push_back(std::numeric_limits<double>::signaling_NaN());
        ASSERT_EQ(blockWRHSNaN->argMin(), boost::optional<size_t>(5));
        ASSERT_EQ(blockWRHSNaN->argMax(), boost::optional<size_t>(3));
    }

    {
        auto block = std::make_unique<value::Int64Block>();
        block->pushNothing();
        block->pushNothing();
        block->pushNothing();
        ASSERT_EQ(block->argMin(), boost::none);
        ASSERT_EQ(block->argMax(), boost::none);
    }

    {
        auto block = std::make_unique<value::Int64Block>();
        block->push_back(static_cast<int64_t>(10));
        block->push_back(static_cast<int64_t>(20));
        block->push_back(static_cast<int64_t>(30));

        // Dense homogeneous blocks access _vals directly.
        ASSERT_THAT(
            block->at(0),
            ValueEq(std::pair{value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(10)}));
        ASSERT_THAT(
            block->at(1),
            ValueEq(std::pair{value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(20)}));
        ASSERT_THAT(
            block->at(2),
            ValueEq(std::pair{value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(30)}));

        // Homogeneous blocks with Nothings call ValueBlock::getAt() which extracts first.
        block->pushNothing();
        block->pushNothing();
        ASSERT_EQ(block->at(3).first, value::TypeTags::Nothing);
        ASSERT_EQ(block->at(4).first, value::TypeTags::Nothing);
    }

    {
        auto bools = std::make_unique<value::BoolBlock>();
        bools->push_back(true);
        bools->push_back(false);
        ASSERT_EQ(bools->argMin(), boost::optional<size_t>(1));
        ASSERT_EQ(bools->argMax(), boost::optional<size_t>(0));

        auto boolsWNothing = std::make_unique<value::BoolBlock>();
        boolsWNothing->pushNothing();
        boolsWNothing->push_back(false);
        boolsWNothing->pushNothing();
        boolsWNothing->push_back(true);
        boolsWNothing->pushNothing();
        ASSERT_EQ(boolsWNothing->argMin(), boost::none);
        ASSERT_EQ(boolsWNothing->argMax(), boost::none);
    }

    {
        auto monoblock = std::make_unique<value::MonoBlock>(
            5, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1));
        ASSERT_EQ(monoblock->argMin(), boost::none);
        ASSERT_EQ(monoblock->argMax(), boost::none);
    }
}

TEST_F(ValueBlockTest, AllTrueFalseTest) {
    {
        value::MonoBlock intMonoBlock(
            3, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1));

        ASSERT_EQ(intMonoBlock.allFalse(), boost::none);
        ASSERT_EQ(intMonoBlock.allTrue(), boost::none);
    }

    {
        value::MonoBlock trueMonoBlock(
            3, value::TypeTags::Boolean, value::bitcastFrom<int32_t>(true));

        ASSERT_EQ(trueMonoBlock.allFalse(), boost::optional<bool>{false});
        ASSERT_EQ(trueMonoBlock.allTrue(), boost::optional<bool>{true});
    }

    {
        value::MonoBlock falseMonoBlock(
            3, value::TypeTags::Boolean, value::bitcastFrom<int32_t>(false));

        ASSERT_EQ(falseMonoBlock.allFalse(), boost::optional<bool>{true});
        ASSERT_EQ(falseMonoBlock.allTrue(), boost::optional<bool>{false});
    }

    {
        value::BoolBlock boolBlock{std::vector<bool>{true, false}};

        ASSERT_EQ(boolBlock.allFalse(), boost::optional<bool>{false});
        ASSERT_EQ(boolBlock.allTrue(), boost::optional<bool>{false});

        boolBlock.pushNothing();

        ASSERT_EQ(boolBlock.allFalse(), boost::none);
        ASSERT_EQ(boolBlock.allTrue(), boost::none);
    }

    {
        value::BoolBlock trueBoolBlock{std::vector<bool>{true, true}};

        ASSERT_EQ(trueBoolBlock.allFalse(), boost::optional<bool>{false});
        ASSERT_EQ(trueBoolBlock.allTrue(), boost::optional<bool>{true});

        trueBoolBlock.pushNothing();

        ASSERT_EQ(trueBoolBlock.allFalse(), boost::none);
        ASSERT_EQ(trueBoolBlock.allTrue(), boost::none);
    }

    {
        value::BoolBlock falseBoolBlock{std::vector<bool>{false, false}};

        ASSERT_EQ(falseBoolBlock.allFalse(), boost::optional<bool>{true});
        ASSERT_EQ(falseBoolBlock.allTrue(), boost::optional<bool>{false});

        falseBoolBlock.pushNothing();

        ASSERT_EQ(falseBoolBlock.allFalse(), boost::none);
        ASSERT_EQ(falseBoolBlock.allTrue(), boost::none);
    }

    {
        value::Int32Block intBlock{std::vector<value::Value>{value::bitcastFrom<int32_t>(1),
                                                             value::bitcastFrom<int32_t>(2)}};

        ASSERT_EQ(intBlock.allFalse(), boost::none);
        ASSERT_EQ(intBlock.allTrue(), boost::none);

        intBlock.pushNothing();

        ASSERT_EQ(intBlock.allFalse(), boost::none);
        ASSERT_EQ(intBlock.allTrue(), boost::none);
    }
}
}  // namespace mongo::sbe
