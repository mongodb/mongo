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

namespace mongo::sbe::value {
namespace {
struct FilterPositionInfoRecorder {
    std::vector<char> result;

    bool isNewDoc = false;

    void recordValue() {
        result.push_back(char(isNewDoc));
        isNewDoc = false;
    }

    void newDoc() {
        isNewDoc = true;
    }
};

struct BsonWalkNode {
    bool isTraverse = false;

    HeterogeneousBlock* outBlock = nullptr;
    FilterPositionInfoRecorder* filterPosInfoRecorder = nullptr;

    // Children which are Get nodes.
    StringMap<std::unique_ptr<BsonWalkNode>> getChildren;

    // Child which is a Traverse node.
    std::unique_ptr<BsonWalkNode> traverseChild;

    void add(const CellBlock::Path& path,
             HeterogeneousBlock* out,
             FilterPositionInfoRecorder* recorder,
             size_t pathIdx = 0) {
        if (pathIdx == 0) {
            // Check some invariants about the path.
            tassert(7953501, "Cannot be given empty path", !path.empty());
            tassert(7953502,
                    "Path must end with Id",
                    std::holds_alternative<CellBlock::Id>(path.back()));
        }

        if (std::holds_alternative<CellBlock::Get>(path[pathIdx])) {
            auto& get = std::get<CellBlock::Get>(path[pathIdx]);
            auto [it, inserted] =
                getChildren.insert(std::pair(get.field, std::make_unique<BsonWalkNode>()));
            it->second->add(path, out, recorder, pathIdx + 1);
        } else if (std::holds_alternative<CellBlock::Traverse>(path[pathIdx])) {
            invariant(pathIdx != 0);
            if (!traverseChild) {
                traverseChild = std::make_unique<BsonWalkNode>();
                traverseChild->isTraverse = true;
            }
            traverseChild->add(path, out, recorder, pathIdx + 1);
        } else if (std::holds_alternative<CellBlock::Id>(path[pathIdx])) {
            invariant(pathIdx != 0);
            filterPosInfoRecorder = recorder;
            outBlock = out;
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
            walkObj(node->traverseChild.get(), elem.embeddedObject());
        }
    } else if (elem.type() == BSONType::Array) {
        if (node->traverseChild) {
            // Follow "traverse" semantics by invoking our children on direct array elements.
            size_t idx = 0;
            for (auto arrElem : elem.embeddedObject()) {
                walkField(node->traverseChild.get(), arrElem);

                ++idx;
            }
        }
    } else if (node->traverseChild) {
        // We didn't see an array but we will apply the traverse to this scalar anyway.
        walkField(node->traverseChild.get(), elem);
    }

    if (node->outBlock) {
        auto [tag, val] = bson::convertFrom<false>(elem);

        node->outBlock->push_back(tag, val);

        if (auto rec = node->filterPosInfoRecorder) {
            rec->recordValue();
            invariant(node->outBlock->size() == rec->result.size());
        }
    }
}
}  // namespace

std::vector<std::unique_ptr<CellBlock>> extractCellBlocksFromBsons(
    const std::vector<CellBlock::PathRequest>& pathReqs, const std::vector<BSONObj>& bsons) {

    std::vector<std::unique_ptr<HeterogeneousBlock>> out(pathReqs.size());
    for (auto& req : out) {
        req = std::make_unique<HeterogeneousBlock>();
    }

    std::vector<FilterPositionInfoRecorder> filterPositionInfoRecorders(out.size());
    BsonWalkNode root;
    {
        size_t idx = 0;
        for (auto& pathReq : pathReqs) {
            auto* filterRecorder = &filterPositionInfoRecorders[idx];

            root.add(pathReq.path, out[idx].get(), filterRecorder);
            invariant(out[idx]);
            ++idx;
        }
    }

    // Track which blocks were added to. This allows us to "fill" in explicit Nothings
    // for missing fields.
    std::vector<size_t> outSizes(out.size());

    for (auto& obj : bsons) {
        {
            size_t idx = 0;
            for (auto& block : out) {
                outSizes[idx] = block->size();

                // Indicate that we're starting a new doc.
                filterPositionInfoRecorders[idx].newDoc();

                ++idx;
            }
        }

        walkObj(&root, obj);

        {
            size_t idx = 0;
            for (auto& block : out) {
                if (outSizes[idx] == block->size()) {
                    // Nothing was added to the block for this document, so we pad with an explicit
                    // Nothing.
                    block->push_back(value::TypeTags::Nothing, Value(0));
                    filterPositionInfoRecorders[idx].recordValue();
                    invariant(block->size() == filterPositionInfoRecorders[idx].result.size());
                }
                ++idx;
            }
        }
    }

    std::vector<std::unique_ptr<CellBlock>> ret;
    size_t idx = 0;
    for (auto& block : out) {
        auto matBlock = std::make_unique<MaterializedCellBlock>();
        matBlock->_deblocked = std::move(block);
        matBlock->_filterPosInfo = std::move(filterPositionInfoRecorders[idx].result);
        ret.push_back(std::move(matBlock));

        ++idx;
    }

    return ret;
}
}  // namespace mongo::sbe::value
