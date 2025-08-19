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

#pragma once

#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/exec/sbe/values/cell_interface.h"

namespace mongo::sbe::value {
/**
 * Interface for extracting CellBlocks from raw BSON. Use makeExtractor(), below, to create an
 * instance. The implementation details are hidden in the associated .cpp file.
 */
class BSONCellExtractor {
public:
    /*
     * Creates a BSON extractor which will collect the given path requests.
     */
    static std::unique_ptr<BSONCellExtractor> make(
        const std::vector<CellBlock::PathRequest>& pathReqs);


    virtual ~BSONCellExtractor() = default;

    /**
     * Given a bunch of BSON objects, extract a set of paths from them into CellBlocks.
     */
    virtual std::vector<std::unique_ptr<CellBlock>> extractFromBsons(
        const std::vector<BSONObj>& bsons) = 0;

    /**
     * Given a bunch of top-level fields (as a tag, val pair), extract the set of sub-paths from
     * them into CellBlocks. This is useful when, for example, we have the value for field 'a'
     * sitting in memory, materialized, and we want to avoid wrapping 'a' in a parent BSON object.
     */
    virtual std::vector<std::unique_ptr<CellBlock>> extractFromTopLevelField(
        StringData topLevelField,
        const std::span<const TypeTags>& tags,
        const std::span<const Value>& vals) = 0;
};

/**
 * Given a vector of PathRequests and BSON objects, produces one CellBlock
 * per path request, with data from the BSON Obj.
 *
 * All returned data is fully owned by the CellBlocks.
 */
std::vector<std::unique_ptr<CellBlock>> extractCellBlocksFromBsons(
    const std::vector<CellBlock::PathRequest>& pathReqs, const std::vector<BSONObj>& bsons);

/**
 * Given a BSONObj and PathRequest, return a vector of the value pointers requested by the path.
 */
std::vector<const char*> extractValuePointersFromBson(BSONObj& obj,
                                                      CellBlock::PathRequest pathReqs);

struct FilterPositionInfoRecorder {
    FilterPositionInfoRecorder() : outputArr(std::make_unique<HeterogeneousBlock>()) {}

    void recordValue(TypeTags tag, Value val);
    void emptyArraySeen();
    void newDoc();
    void endDoc();
    std::unique_ptr<HeterogeneousBlock> extractValues();

    std::vector<int32_t> posInfo;
    bool isNewDoc = false;
    bool arraySeen = false;
    std::unique_ptr<HeterogeneousBlock> outputArr;
};

/**
 * A utility class for building projected paths as SBE values. There is a many-to-one relationship
 * of recorders to 'BsonWalkNode.' Each recorder corresponds to a projected path.
 * 'ProjectionPositionInfoRecorder' exposes an interface; its subclasses implement the interface
 * with the 'Curiously Recurring Template Pattern' idiom.
 */
template <class T>
struct ProjectionPositionInfoRecorder {
    /**
     * Record a given typeTag and value at the current array nestedness level.
     */
    void recordValue(TypeTags tag, Value val);
    /**
     * For this projected path, enter a new level of array nestedness.
     */
    void startArray();
    /**
     * For this projected path, exit the current array nestedness level.
     */
    void endArray();
    std::vector<std::unique_ptr<value::Array>> arrayStack;
};

/**
 * Implements the 'ProjectionPositionInfoRecorder' interface for block processing. Multiple
 * documents are processed. Projected values are stored in 'outputArr.'
 */
struct BlockProjectionPositionInfoRecorder final
    : ProjectionPositionInfoRecorder<BlockProjectionPositionInfoRecorder> {
    BlockProjectionPositionInfoRecorder() : outputArr(std::make_unique<HeterogeneousBlock>()) {}
    void newDoc();
    void endDoc();
    std::unique_ptr<HeterogeneousBlock> extractValues();
    std::unique_ptr<HeterogeneousBlock> outputArr;
    bool isNewDoc = false;
    void onNewValue() {
        isNewDoc = false;
    }
    void saveValue(TypeTags tag, Value val) {
        outputArr->push_back(tag, val);
    }
};

/**
 * Implements the 'ProjectionPositionInfoRecorder' interface for efficient field path projection. A
 * single document is processed. The projected value is stored in 'outputValue.'
 */
struct ScalarProjectionPositionInfoRecorder final
    : ProjectionPositionInfoRecorder<ScalarProjectionPositionInfoRecorder> {
    MoveableValueGuard extractValue();
    MoveableValueGuard outputValue;
    void onNewValue() { /* Noop */ }
    void saveValue(TypeTags tag, Value val) {
        outputValue = MoveableValueGuard{tag, val};
    }
};

struct BsonWalkNode {
    FilterPositionInfoRecorder* filterPosInfoRecorder = nullptr;

    std::vector<BlockProjectionPositionInfoRecorder*> childProjRecorders;
    BlockProjectionPositionInfoRecorder* projRecorder = nullptr;

    // Children which are Get nodes.
    StringMap<std::unique_ptr<BsonWalkNode>> getChildren;

    // Child which is a Traverse node.
    std::unique_ptr<BsonWalkNode> traverseChild;

    void add(const CellBlock::Path& path,
             FilterPositionInfoRecorder* recorder,
             BlockProjectionPositionInfoRecorder* outProjBlockRecorder,
             size_t pathIdx = 0);
};
}  // namespace mongo::sbe::value
