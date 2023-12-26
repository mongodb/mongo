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
        posInfo.push_back(char(isNewDoc));
        isNewDoc = false;
    }

    void newDoc() {
        isNewDoc = true;
    }

    void endDoc() {
        if (isNewDoc) {
            outputArr->push_back(value::TypeTags::Nothing, Value(0));
            posInfo.push_back(char(true));
        }
    }

    std::vector<char> posInfo;
    bool isNewDoc = false;
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

void walkField(BsonWalkNode* node, const BSONElement& elem);

void walkObj(BsonWalkNode* node, const BSONObj& obj) {
    for (auto elem : obj) {
        auto fieldName = elem.fieldNameStringData();
        auto it = node->getChildren.find(fieldName);

        if (it != node->getChildren.end()) {
            walkField(it->second.get(), elem);
        }
    }
}

void walkField(BsonWalkNode* node, const BSONElement& elem) {
    if (elem.type() == BSONType::Object) {
        walkObj(node, elem.embeddedObject());
        if (node->traverseChild) {
            walkField(node->traverseChild.get(), elem);
        }
    } else if (elem.type() == BSONType::Array) {
        if (node->traverseChild) {
            // The projection traversal semantics are "special" in that the leaf must know
            // when there is an array higher up in the tree.
            for (auto& projRecorder : node->childProjRecorders) {
                projRecorder->startArray();
            }
            // Follow "traverse" semantics by invoking our children on direct array elements.
            size_t idx = 0;
            for (auto arrElem : elem.embeddedObject()) {
                walkField(node->traverseChild.get(), arrElem);

                ++idx;
            }

            for (auto& projRecorder : node->childProjRecorders) {
                projRecorder->endArray();
            }
        }
    } else if (node->traverseChild) {
        // We didn't see an array, so we apply the node below the traverse to this scalar.
        walkField(node->traverseChild.get(), elem);
    }

    if (node->filterPosInfoRecorder || node->projRecorder) {
        auto [tag, val] = bson::convertFrom<true>(elem);

        if (auto rec = node->filterPosInfoRecorder) {
            rec->recordValue(tag, val);
        }

        if (auto rec = node->projRecorder) {
            rec->recordValue(tag, val);
        }
    }
}
}  // namespace

std::vector<std::unique_ptr<CellBlock>> extractCellBlocksFromBsons(
    const std::vector<CellBlock::PathRequest>& pathReqs, const std::vector<BSONObj>& bsons) {

    std::vector<FilterPositionInfoRecorder> filterPositionInfoRecorders(pathReqs.size());
    std::vector<ProjectionPositionInfoRecorder> projPositionInfoRecorders(pathReqs.size());
    BsonWalkNode root;
    {
        size_t idx = 0;
        for (auto& pathReq : pathReqs) {
            if (pathReq.type == CellBlock::PathRequestType::kFilter) {
                root.add(pathReq.path, &filterPositionInfoRecorders[idx], nullptr);
            } else {
                root.add(pathReq.path, nullptr, &projPositionInfoRecorders[idx]);
            }
            ++idx;
        }
    }

    for (auto& obj : bsons) {
        for (size_t idx = 0; idx < pathReqs.size(); ++idx) {
            filterPositionInfoRecorders[idx].newDoc();
            projPositionInfoRecorders[idx].newDoc();
        }

        walkObj(&root, obj);

        for (size_t idx = 0; idx < pathReqs.size(); ++idx) {
            filterPositionInfoRecorders[idx].endDoc();
            projPositionInfoRecorders[idx].endDoc();
        }
    }

    std::vector<std::unique_ptr<CellBlock>> ret;
    for (size_t idx = 0; idx < pathReqs.size(); ++idx) {
        auto matBlock = std::make_unique<MaterializedCellBlock>();
        if (pathReqs[idx].type == CellBlock::PathRequestType::kFilter) {
            matBlock->_deblocked = std::move(filterPositionInfoRecorders[idx].outputArr);
            matBlock->_filterPosInfo = std::move(filterPositionInfoRecorders[idx].posInfo);
        } else if (pathReqs[idx].type == CellBlock::PathRequestType::kProject) {
            auto& block = projPositionInfoRecorders[idx].outputArr;
            invariant(block->size() == bsons.size());
            matBlock->_deblocked = std::move(block);
            // No associated position info since we already have one value per document.
        }
        ret.push_back(std::move(matBlock));
    }

    return ret;
}
}  // namespace mongo::sbe::value
