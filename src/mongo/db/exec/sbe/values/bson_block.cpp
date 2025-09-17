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
#include "mongo/db/exec/sbe/values/util.h"
#include "mongo/db/exec/sbe/values/value.h"

namespace mongo::sbe::value {

void FilterPositionInfoRecorder::recordValue(TypeTags tag, Value val) {
    auto [cpyTag, cpyVal] = copyValue(tag, val);
    outputArr->push_back(cpyTag, cpyVal);
    posInfo.back()++;
    isNewDoc = false;
}

void FilterPositionInfoRecorder::emptyArraySeen() {
    arraySeen = true;
}

void FilterPositionInfoRecorder::newDoc() {
    posInfo.push_back(0);
    isNewDoc = true;
    arraySeen = false;
}

void FilterPositionInfoRecorder::endDoc() {
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
            // indicate that there's one value for this document. This matches the behavior of the
            // scalar traverseF primitive.
            outputArr->push_back(value::TypeTags::Nothing, Value(0));
            posInfo.back()++;
        }
    }
}

std::unique_ptr<HeterogeneousBlock> FilterPositionInfoRecorder::extractValues() {
    auto out = std::move(outputArr);
    outputArr = std::make_unique<HeterogeneousBlock>();
    return out;
}

void BlockProjectionPositionInfoRecorder::newDoc() {
    isNewDoc = true;
}

void BlockProjectionPositionInfoRecorder::endDoc() {
    if (isNewDoc) {
        // We didn't record anything for the last document, so add a Nothing to our block.
        outputArr->push_back(TypeTags::Nothing, Value(0));
    }
    isNewDoc = false;
}

std::unique_ptr<HeterogeneousBlock> BlockProjectionPositionInfoRecorder::extractValues() {
    auto out = std::move(outputArr);
    outputArr = std::make_unique<HeterogeneousBlock>();
    return out;
}

MoveableValueGuard ScalarProjectionPositionInfoRecorder::extractValue() {
    tassert(10926202, "expects empty arrayStack", arrayStack.empty());
    return std::move(outputValue);
}


class BSONExtractorImpl : public BSONCellExtractor {
public:
    BSONExtractorImpl(std::vector<CellBlock::PathRequest> pathReqs);

    std::vector<std::unique_ptr<CellBlock>> extractFromBsons(
        const std::vector<BSONObj>& bsons) override;

    std::vector<std::unique_ptr<CellBlock>> extractFromTopLevelField(
        StringData topLevelField,
        const std::span<const TypeTags>& tags,
        const std::span<const Value>& vals) override;

    BsonWalkNode<BlockProjectionPositionInfoRecorder>* getRoot() {
        return &_root;
    }

private:
    std::vector<std::unique_ptr<CellBlock>> constructOutputFromRecorders();

    std::vector<CellBlock::PathRequest> _pathReqs;
    std::vector<FilterPositionInfoRecorder> _filterPositionInfoRecorders;
    std::vector<BlockProjectionPositionInfoRecorder> _projPositionInfoRecorders;
    BsonWalkNode<BlockProjectionPositionInfoRecorder> _root;
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
void visitElementExtractorCallback(BsonWalkNode<BlockProjectionPositionInfoRecorder>* node,
                                   TypeTags eltTag,
                                   Value eltVal,
                                   const char* bson) {
    if (auto rec = node->filterRecorder) {
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

        walkField<BlockProjectionPositionInfoRecorder>(node->second.get(),
                                                       tags[i],
                                                       vals[i],
                                                       nullptr /* bsonPtr */,
                                                       visitElementExtractorCallback);

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

        walkObj<BlockProjectionPositionInfoRecorder>(
            &_root, obj.objdata(), visitElementExtractorCallback);

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
    auto recordValuePointer =
        [&bsonPointers](BsonWalkNode<BlockProjectionPositionInfoRecorder>* node,
                        value::TypeTags eltTag,
                        Value eltVal,
                        const char* bson) {
            if (node->filterRecorder) {
                bsonPointers.push_back(bson::getValue(bson));
            }
            if (node->projRecorder) {
                bsonPointers.push_back(bson::getValue(bson));
            }
        };

    walkObj<BlockProjectionPositionInfoRecorder>(
        extractor.getRoot(), obj.objdata(), recordValuePointer);
    return bsonPointers;
}
}  // namespace mongo::sbe::value
