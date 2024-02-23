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

#include "mongo/db/exec/sbe/values/bson_block.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/value.h"

namespace mongo::sbe::value {
namespace {
struct FilterPositionInfoRecorder {
    FilterPositionInfoRecorder() : outputArr(std::make_unique<HeterogeneousBlock>()) {}

    void recordValue(TypeTags tag, Value val) {
        auto [cpyTag, cpyVal] = copyValue(tag, val);
        outputArr->push_back(cpyTag, cpyVal);
        posInfo.back()++;
        isNewDoc = false;
    }

    void emptyArraySeen() {
        arraySeen = true;
    }

    void newDoc() {
        posInfo.push_back(0);
        isNewDoc = true;
        arraySeen = false;
    }

    void endDoc() {
        if (isNewDoc) {
            // We recorded no values for this doc. There are two possibilities:
            // (1) there is/are only empty array(s) at this path, which were traversed.
            //     Example: document {a: {b: []}}, path Get(a)/Traverse/Get(b)/Traverse/Id.
            // (2) There are no values at this path.
            //     Example: document {a: {b:1}} searching for path Get(a)/Traverse/Get(c)/Id.
            //
            // It is important that we distinguish these two cases for MQL's sake.
            if (arraySeen) {
                // This represents case (1). We record this by adding no values to our output and
                // leaving the position info value as '0'.

                // Do nothing.
            } else {
                // This represents case (2). We record this by adding an explicit Nothing value and
                // indicate that there's one value for this document. This matches the behavior of
                // the scalar traverseF primitive.
                outputArr->push_back(value::TypeTags::Nothing, Value(0));
                posInfo.back()++;
            }
        }
    }

    std::unique_ptr<HeterogeneousBlock> extractValues() {
        auto out = std::move(outputArr);
        outputArr = std::make_unique<HeterogeneousBlock>();
        return out;
    }

    std::vector<int32_t> posInfo;
    bool isNewDoc = false;
    bool arraySeen = false;
    std::unique_ptr<HeterogeneousBlock> outputArr;
};

struct ProjectionPositionInfoRecorder {
    ProjectionPositionInfoRecorder() : outputArr(std::make_unique<HeterogeneousBlock>()) {}

    void recordValue(TypeTags tag, Value val) {
        isNewDoc = false;

        auto [cpyTag, cpyVal] = copyValue(tag, val);
        if (arrayStack.empty()) {
            outputArr->push_back(cpyTag, cpyVal);
        } else {
            arrayStack.back()->push_back(cpyTag, cpyVal);
        }
    }

    void newDoc() {
        isNewDoc = true;
    }

    void endDoc() {
        if (isNewDoc) {
            // We didn't record anything for the last document, so add a Nothing to our block.
            outputArr->push_back(TypeTags::Nothing, Value(0));
        }
        isNewDoc = false;
    }

    void startArray() {
        isNewDoc = false;
        arrayStack.push_back(std::make_unique<Array>());
    }

    void endArray() {
        invariant(!arrayStack.empty());

        if (arrayStack.size() > 1) {
            // For a nested array, we cram it into its parent.
            auto releasedArray = arrayStack.back().release();
            arrayStack.pop_back();
            arrayStack.back()->push_back(TypeTags::Array, bitcastFrom<Array*>(releasedArray));
        } else {
            outputArr->push_back(TypeTags::Array,
                                 bitcastFrom<Array*>(arrayStack.front().release()));

            arrayStack.clear();
        }
    }

    std::unique_ptr<HeterogeneousBlock> extractValues() {
        auto out = std::move(outputArr);
        outputArr = std::make_unique<HeterogeneousBlock>();
        return out;
    }

    std::unique_ptr<HeterogeneousBlock> outputArr;
    std::vector<std::unique_ptr<value::Array>> arrayStack;
    bool isNewDoc = false;
};

struct BsonWalkNode {
    bool isTraverse = false;

    FilterPositionInfoRecorder* filterPosInfoRecorder = nullptr;

    std::vector<ProjectionPositionInfoRecorder*> childProjRecorders;
    ProjectionPositionInfoRecorder* projRecorder = nullptr;

    // Children which are Get nodes.
    StringMap<std::unique_ptr<BsonWalkNode>> getChildren;

    // Child which is a Traverse node.
    std::unique_ptr<BsonWalkNode> traverseChild;

    void add(const CellBlock::Path& path,
             FilterPositionInfoRecorder* recorder,
             ProjectionPositionInfoRecorder* outProjBlockRecorder,
             size_t pathIdx = 0) {
        if (pathIdx == 0) {
            // Check some invariants about the path.
            tassert(7953501, "Cannot be given empty path", !path.empty());
            tassert(
                7953502, "Path must end with Id", holds_alternative<CellBlock::Id>(path.back()));
        }

        if (holds_alternative<CellBlock::Get>(path[pathIdx])) {
            auto& get = std::get<CellBlock::Get>(path[pathIdx]);
            auto [it, inserted] =
                getChildren.insert(std::pair(get.field, std::make_unique<BsonWalkNode>()));
            it->second->add(path, recorder, outProjBlockRecorder, pathIdx + 1);
        } else if (holds_alternative<CellBlock::Traverse>(path[pathIdx])) {
            invariant(pathIdx != 0);
            if (!traverseChild) {
                traverseChild = std::make_unique<BsonWalkNode>();
                traverseChild->isTraverse = true;
            }
            if (outProjBlockRecorder) {
                // Each node must know about all projection recorders below it, not just ones
                // directly below.
                childProjRecorders.push_back(outProjBlockRecorder);
            }

            traverseChild->add(path, recorder, outProjBlockRecorder, pathIdx + 1);
        } else if (holds_alternative<CellBlock::Id>(path[pathIdx])) {
            invariant(pathIdx != 0);

            if (recorder) {
                filterPosInfoRecorder = recorder;
            }
            if (outProjBlockRecorder) {
                projRecorder = outProjBlockRecorder;
            }
            invariant(pathIdx == path.size() - 1);
        }
    }
};

template <class Cb>
void walkField(
    BsonWalkNode* node, TypeTags eltTag, Value eltVal, const char* bsonPtr, const Cb& cb);

template <class Cb>
void walkObj(BsonWalkNode* node, const char* be, const Cb& cb) {
    const auto end = bson::bsonEnd(be);
    // Skip document length.
    be += 4;

    while (be != end - 1) {
        auto fieldName = bson::fieldNameAndLength(be);
        auto it = node->getChildren.find(fieldName);
        if (it != node->getChildren.end()) {
            auto [eltTag, eltVal] = bson::convertFrom<true>(be, end, fieldName.size());
            walkField(it->second.get(), eltTag, eltVal, be, cb);
        }

        be = bson::advance(be, fieldName.size());
    }
}

template <class Cb>
void walkField(
    BsonWalkNode* node, TypeTags eltTag, Value eltVal, const char* bsonPtr, const Cb& cb) {
    if (value::isObject(eltTag)) {
        invariant(eltTag == TypeTags::bsonObject);  // Only BSON is supported for now.

        walkObj(node, value::bitcastTo<const char*>(eltVal), cb);
        if (node->traverseChild) {
            walkField(node->traverseChild.get(), eltTag, eltVal, bsonPtr, cb);
        }
    } else if (value::isArray(eltTag)) {
        invariant(eltTag == TypeTags::bsonArray);
        if (node->traverseChild) {
            // The projection traversal semantics are "special" in that the leaf must know
            // when there is an array higher up in the tree.
            for (auto& projRecorder : node->childProjRecorders) {
                projRecorder->startArray();
            }

            bool isArrayEmpty = true;
            {
                auto arrayBson = value::bitcastTo<const char*>(eltVal);
                const auto arrayEnd = bson::bsonEnd(arrayBson);

                // Follow "traverse" semantics by invoking our children on direct array elements.
                arrayBson += 4;

                while (arrayBson != arrayEnd - 1) {
                    auto sv = bson::fieldNameAndLength(arrayBson);
                    auto [arrEltTag, arrEltVal] =
                        bson::convertFrom<true>(arrayBson, arrayEnd, sv.size());
                    walkField(node->traverseChild.get(), arrEltTag, arrEltVal, arrayBson, cb);

                    arrayBson = bson::advance(arrayBson, sv.size());
                    isArrayEmpty = false;
                }
            }

            if (isArrayEmpty && node->traverseChild->filterPosInfoRecorder) {
                // If the array was empty, indicate that we saw an empty array to the
                // filterPosInfoRecorder.
                node->traverseChild->filterPosInfoRecorder->emptyArraySeen();
            }

            for (auto& projRecorder : node->childProjRecorders) {
                projRecorder->endArray();
            }
        }
    } else if (node->traverseChild) {
        // We didn't see an array, so we apply the node below the traverse to this scalar.
        walkField(node->traverseChild.get(), eltTag, eltVal, bsonPtr, cb);
    }
    cb(node, eltTag, eltVal, bsonPtr);
}

class BSONExtractorImpl : public BSONCellExtractor {
public:
    BSONExtractorImpl(std::vector<CellBlock::PathRequest> pathReqs);

    std::vector<std::unique_ptr<CellBlock>> extractFromBsons(const std::vector<BSONObj>& bsons);

    std::vector<std::unique_ptr<CellBlock>> extractFromTopLevelField(
        StringData topLevelField,
        const std::span<const TypeTags>& tags,
        const std::span<const Value>& vals);

    BsonWalkNode* getRoot() {
        return &_root;
    }

private:
    std::vector<std::unique_ptr<CellBlock>> constructOutputFromRecorders();

    std::vector<CellBlock::PathRequest> _pathReqs;
    std::vector<FilterPositionInfoRecorder> _filterPositionInfoRecorders;
    std::vector<ProjectionPositionInfoRecorder> _projPositionInfoRecorders;
    BsonWalkNode _root;
};

BSONExtractorImpl::BSONExtractorImpl(std::vector<CellBlock::PathRequest> pathReqsIn)
    : _pathReqs(std::move(pathReqsIn)) {
    // Ensure we don't reallocate and move the address of these objects, since the path tree
    // contains pointers to them.
    _filterPositionInfoRecorders.reserve(_pathReqs.size());
    _projPositionInfoRecorders.reserve(_pathReqs.size());
    {
        for (auto& pathReq : _pathReqs) {
            if (pathReq.type == CellBlock::PathRequestType::kFilter) {
                _filterPositionInfoRecorders.emplace_back();
                _root.add(pathReq.path, &_filterPositionInfoRecorders.back(), nullptr);
            } else {
                _projPositionInfoRecorders.emplace_back();
                _root.add(pathReq.path, nullptr, &_projPositionInfoRecorders.back());
            }
        }
    }
}

/*
 * Callback used in the extractor code when walking the bson. This simply records leave values
 * depending on which records are present.
 */
void visitElementExtractorCallback(BsonWalkNode* node,
                                   TypeTags eltTag,
                                   Value eltVal,
                                   const char* bson) {
    if (auto rec = node->filterPosInfoRecorder) {
        rec->recordValue(eltTag, eltVal);
    }

    if (auto rec = node->projRecorder) {
        rec->recordValue(eltTag, eltVal);
    }
}

std::vector<std::unique_ptr<CellBlock>> BSONExtractorImpl::extractFromTopLevelField(
    StringData topLevelField,
    const std::span<const TypeTags>& tags,
    const std::span<const Value>& vals) {
    invariant(tags.size() == vals.size());

    auto node = _root.getChildren.find(topLevelField);

    // Caller should always ask us to extract a top level field that's in the reqs.  We could
    // relax this if needed, and return a bunch of Nothing CellBlocks, but it's a non-use case
    // for now.
    invariant(node != _root.getChildren.end());

    for (size_t i = 0; i < tags.size(); ++i) {
        for (auto& rec : _filterPositionInfoRecorders) {
            rec.newDoc();
        }
        for (auto& rec : _projPositionInfoRecorders) {
            rec.newDoc();
        }

        walkField(node->second.get(), tags[i], vals[i], nullptr, visitElementExtractorCallback);

        for (auto& rec : _filterPositionInfoRecorders) {
            rec.endDoc();
        }
        for (auto& rec : _projPositionInfoRecorders) {
            rec.endDoc();
        }
    }

    return constructOutputFromRecorders();
}

std::vector<std::unique_ptr<CellBlock>> BSONExtractorImpl::extractFromBsons(
    const std::vector<BSONObj>& bsons) {
    for (auto& obj : bsons) {
        for (auto& rec : _filterPositionInfoRecorders) {
            rec.newDoc();
        }
        for (auto& rec : _projPositionInfoRecorders) {
            rec.newDoc();
        }

        walkObj(&_root, obj.objdata(), visitElementExtractorCallback);

        for (auto& rec : _filterPositionInfoRecorders) {
            rec.endDoc();
        }
        for (auto& rec : _projPositionInfoRecorders) {
            rec.endDoc();
        }
    }

    return constructOutputFromRecorders();
}

std::vector<std::unique_ptr<CellBlock>> BSONExtractorImpl::constructOutputFromRecorders() {
    std::vector<std::unique_ptr<CellBlock>> ret;
    size_t filterRecorderIdx = 0;
    size_t projRecorderIdx = 0;

    for (auto&& path : _pathReqs) {
        auto matBlock = std::make_unique<MaterializedCellBlock>();
        if (path.type == CellBlock::PathRequestType::kFilter) {
            auto& recorder = _filterPositionInfoRecorders[filterRecorderIdx];
            matBlock->_deblocked = recorder.extractValues();
            matBlock->_filterPosInfo = std::move(recorder.posInfo);
            ++filterRecorderIdx;
        } else if (path.type == CellBlock::PathRequestType::kProject) {
            auto& recorder = _projPositionInfoRecorders[projRecorderIdx];
            matBlock->_deblocked = recorder.extractValues();
            // No associated position info since we already have one value per document.

            ++projRecorderIdx;
        } else {
            MONGO_UNREACHABLE_TASSERT(8463101);
        }
        ret.push_back(std::move(matBlock));
    }
    tassert(8463102,
            "Number of filter and projection recorders must sum to number of paths",
            projRecorderIdx + filterRecorderIdx == _pathReqs.size());
    return ret;
}

}  // namespace

std::unique_ptr<BSONCellExtractor> BSONCellExtractor::make(
    const std::vector<CellBlock::PathRequest>& pathReqs) {
    return std::make_unique<BSONExtractorImpl>(pathReqs);
}

std::vector<std::unique_ptr<CellBlock>> extractCellBlocksFromBsons(
    const std::vector<CellBlock::PathRequest>& pathReqs, const std::vector<BSONObj>& bsons) {

    auto extractor = BSONCellExtractor::make(pathReqs);
    return extractor->extractFromBsons(bsons);
}

std::vector<const char*> extractValuePointersFromBson(BSONObj& obj,
                                                      value::CellBlock::PathRequest pathRequest) {
    std::vector<value::CellBlock::PathRequest> pathrequests{pathRequest};
    auto extractor = BSONExtractorImpl(pathrequests);

    std::vector<const char*> bsonPointers;

    // Callback to record pointer values in bsonPointers.
    auto recordValuePointer = [&bsonPointers](BsonWalkNode* node,
                                              value::TypeTags eltTag,
                                              Value eltVal,
                                              const char* bson) {
        if (node->filterPosInfoRecorder) {
            bsonPointers.push_back(bson::getValue(bson));
        }
    };

    walkObj(extractor.getRoot(), obj.objdata(), recordValuePointer);
    return bsonPointers;
}
}  // namespace mongo::sbe::value
