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
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/cell_interface.h"
#include "mongo/db/exec/sbe/values/util.h"
#include "mongo/db/exec/sbe/values/value.h"

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

template <class T>
void ProjectionPositionInfoRecorder<T>::recordValue(TypeTags tag, Value val) {
    static_cast<T*>(this)->onNewValue();
    if (arrayStack.empty()) {
        // Each recorder type will handle ownership.
        static_cast<T*>(this)->saveValue(tag, val);
    } else {
        // We always need value ownership when adding to an array, regardless of recorder type.
        auto [cpyTag, cpyVal] = copyValue(tag, val);
        arrayStack.back()->push_back(cpyTag, cpyVal);
    }
}

template <class T>
void ProjectionPositionInfoRecorder<T>::startArray() {
    static_cast<T*>(this)->onNewValue();
    arrayStack.push_back(std::make_unique<Array>());
}

template <class T>
void ProjectionPositionInfoRecorder<T>::endArray() {
    tassert(10926200, "expects nonempty arrayStack", !arrayStack.empty());
    if (arrayStack.size() > 1) {
        // For a nested array, we cram it into its parent.
        auto releasedArray = arrayStack.back().release();
        arrayStack.pop_back();
        arrayStack.back()->push_back(TypeTags::Array, bitcastFrom<Array*>(releasedArray));
    } else {
        static_cast<T*>(this)->saveOwnedArray(std::move(arrayStack.front()));
        arrayStack.clear();
    }
}

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
    void saveOwnedArray(std::unique_ptr<Array> arr) {
        outputArr->push_back(TypeTags::Array, bitcastFrom<Array*>(arr.release()));
    }
    void saveValue(TypeTags tag, Value val) {
        // HeterogenousBlock's must own the values they contain.
        auto [cpyTag, cpyVal] = value::copyValue(tag, val);
        outputArr->push_back(cpyTag, cpyVal);
    }
};

/**
 * Implements the 'ProjectionPositionInfoRecorder' interface for efficient field path projection. A
 * single document is processed. The projected value is stored in 'outputValue.'
 */
struct ScalarProjectionPositionInfoRecorder final
    : ProjectionPositionInfoRecorder<ScalarProjectionPositionInfoRecorder> {
    TagValueMaybeOwned extractValue();
    TagValueMaybeOwned outputValue;
    void onNewValue() { /* Noop */ }
    void saveValue(TypeTags tag, Value val) {
        // If we have traversed an array, then this recorder owns the array and it will be saved
        // with saveOwnedArray(). Otherwise the recorder just holds an unowned view into the input
        // so we return an unowned view here.
        outputValue = TagValueMaybeOwned::fromRaw(false /* owned */, tag, val);
    }
    void saveOwnedArray(std::unique_ptr<Array> arr) {
        outputValue = TagValueMaybeOwned::fromRaw(
            true /* owned */, TypeTags::Array, value::bitcastFrom<Array*>(arr.release()));
    }
};

template <class ProjectionRecorder>
struct BsonWalkNode {
    FilterPositionInfoRecorder* filterRecorder = nullptr;

    std::vector<ProjectionRecorder*> childProjRecorders;
    ProjectionRecorder* projRecorder = nullptr;

    // Children which are Get nodes.
    StringMap<std::unique_ptr<BsonWalkNode>> getChildren;

    // Child which is a Traverse node.
    std::unique_ptr<BsonWalkNode> traverseChild;

    void add(const CellBlock::Path& path,
             FilterPositionInfoRecorder* filterRecorder,
             ProjectionRecorder* outProjBlockRecorder,
             size_t pathIdx = 0);
};

template <class ProjectionRecorder>
void BsonWalkNode<ProjectionRecorder>::add(const CellBlock::Path& path,
                                           FilterPositionInfoRecorder* outFilterRecorder,
                                           ProjectionRecorder* outProjRecorder,
                                           size_t pathIdx /*= 0*/) {
    if (pathIdx == 0) {
        // Check some invariants about the path.
        tassert(7953501, "Cannot be given empty path", !path.empty());
        tassert(7953502, "Path must end with Id", holds_alternative<CellBlock::Id>(path.back()));
    }

    if (holds_alternative<CellBlock::Get>(path[pathIdx])) {
        auto& get = std::get<CellBlock::Get>(path[pathIdx]);
        auto [it, inserted] = getChildren.insert(
            std::pair(get.field, std::make_unique<BsonWalkNode<ProjectionRecorder>>()));
        it->second->add(path, outFilterRecorder, outProjRecorder, pathIdx + 1);
    } else if (holds_alternative<CellBlock::Traverse>(path[pathIdx])) {
        tassert(11089614, "Unexpected pathIdx", pathIdx != 0);
        if (!traverseChild) {
            traverseChild = std::make_unique<BsonWalkNode<ProjectionRecorder>>();
        }
        if (outProjRecorder) {
            // Each node must know about all projection recorders below it, not just ones directly
            // below.
            childProjRecorders.push_back(outProjRecorder);
        }

        traverseChild->add(path, outFilterRecorder, outProjRecorder, pathIdx + 1);
    } else if (holds_alternative<CellBlock::Id>(path[pathIdx])) {
        tassert(11089610, "Unexpected pathIdx", pathIdx != 0);
        if (outFilterRecorder) {
            filterRecorder = outFilterRecorder;
        }
        if (outProjRecorder) {
            projRecorder = outProjRecorder;
        }
        tassert(11089612, "Unexpected pathIdx", pathIdx == path.size() - 1);
    }
}

template <class ProjectionRecorder, class Cb>
requires std::invocable<Cb&, BsonWalkNode<ProjectionRecorder>*, TypeTags, Value, const char*>
void walkField(BsonWalkNode<ProjectionRecorder>* node,
               TypeTags eltTag,
               Value eltVal,
               const char* bsonPtr,
               const Cb& cb);

template <class ProjectionRecorder, class Cb>
requires std::invocable<Cb&, BsonWalkNode<ProjectionRecorder>*, TypeTags, Value, const char*>
void walkObj(BsonWalkNode<ProjectionRecorder>* node,
             value::TypeTags inputTag,
             value::Value inputVal,
             const char* bsonPtr,
             const Cb& cb) {
    size_t numChildrenWalked = 0;
    auto callback = [&](StringData currFieldName,
                        value::TypeTags tag,
                        value::Value val,
                        const char* cur) -> bool {
        if (numChildrenWalked >= node->getChildren.size()) {
            // Early exit because we've walked every child for this node.
            return true;
        }
        if (auto it = node->getChildren.find(currFieldName); it != node->getChildren.end()) {
            walkField<ProjectionRecorder, Cb>(it->second.get(), tag, val, cur, cb);
            numChildrenWalked++;
        }
        return false;
    };
    value::objectForEach(inputTag, inputVal, callback);
}

template <class ProjectionRecorder, class Cb>
requires std::invocable<Cb&, BsonWalkNode<ProjectionRecorder>*, TypeTags, Value, const char*>
void walkField(BsonWalkNode<ProjectionRecorder>* node,
               TypeTags eltTag,
               Value eltVal,
               const char* bsonPtr,
               const Cb& cb) {
    if (value::isObject(eltTag)) {
        walkObj<ProjectionRecorder, Cb>(node, eltTag, eltVal, bsonPtr, cb);
        if (node->traverseChild) {
            walkField<ProjectionRecorder, Cb>(
                node->traverseChild.get(), eltTag, eltVal, bsonPtr, cb);
        }
    } else if (value::isArray(eltTag)) {
        if (node->traverseChild) {
            // The projection traversal semantics are "special" in that the leaf must know
            // when there is an array higher up in the tree.
            for (auto& projRecorder : node->childProjRecorders) {
                projRecorder->startArray();
            }

            bool isArrayEmpty = true;

            auto arrEltCb =
                [&](value::TypeTags arrEltTag, value::Value arrEltVal, const char* arrayBson) {
                    walkField<ProjectionRecorder>(
                        node->traverseChild.get(), arrEltTag, arrEltVal, arrayBson, cb);
                    isArrayEmpty = false;
                };
            value::arrayForEach(eltTag, eltVal, arrEltCb);

            if (isArrayEmpty && node->traverseChild->filterRecorder) {
                // If the array was empty, indicate that we saw an empty array to the
                // filterRecorder.
                node->traverseChild->filterRecorder->emptyArraySeen();
            }

            for (auto& projRecorder : node->childProjRecorders) {
                projRecorder->endArray();
            }
        }
    } else if (node->traverseChild) {
        // We didn't see an array, so we apply the node below the traverse to this scalar.
        walkField<ProjectionRecorder>(node->traverseChild.get(), eltTag, eltVal, bsonPtr, cb);
    }
    // Some callbacks use the raw bson pointer, not just the tag and value.
    cb(node, eltTag, eltVal, bsonPtr);
}

}  // namespace mongo::sbe::value
