// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/values/bson_block.h"

#include "mongo/db/exec/sbe/values/object_walk_node.h"

#include <string_view>

namespace mongo::sbe::value {
class BSONExtractorImpl : public BSONCellExtractor {
public:
    BSONExtractorImpl(std::vector<PathRequest> pathReqs);

    std::vector<std::unique_ptr<CellBlock>> extractFromBsons(
        const std::vector<BSONObj>& bsons) override;

    std::vector<std::unique_ptr<CellBlock>> extractFromTopLevelField(
        std::string_view topLevelField,
        const std::span<const TypeTags>& tags,
        const std::span<const Value>& vals) override;

    ObjectWalkNode<BlockProjectionPositionInfoRecorder>* getRoot() {
        return &_root;
    }

private:
    std::vector<std::unique_ptr<CellBlock>> constructOutputFromRecorders();

    std::vector<PathRequest> _pathReqs;
    std::vector<FilterPositionInfoRecorder> _filterPositionInfoRecorders;
    std::vector<BlockProjectionPositionInfoRecorder> _projPositionInfoRecorders;
    ObjectWalkNode<BlockProjectionPositionInfoRecorder> _root;
};

BSONExtractorImpl::BSONExtractorImpl(std::vector<PathRequest> pathReqsIn)
    : _pathReqs(std::move(pathReqsIn)) {
    // Ensure we don't reallocate and move the address of these objects, since the path tree
    // contains pointers to them.
    _filterPositionInfoRecorders.reserve(_pathReqs.size());
    _projPositionInfoRecorders.reserve(_pathReqs.size());
    {
        for (auto& pathReq : _pathReqs) {
            if (pathReq.type == PathRequestType::kFilter) {
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
void visitElementExtractorCallback(ObjectWalkNode<BlockProjectionPositionInfoRecorder>* node,
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
    std::string_view topLevelField,
    const std::span<const TypeTags>& tags,
    const std::span<const Value>& vals) {
    tassert(
        11089616, "Number of Tags doesn't match the number of Values", tags.size() == vals.size());

    auto node = _root.findChild(topLevelField);

    // Caller should always ask us to extract a top level field that's in the reqs.  We could
    // relax this if needed, and return a bunch of Nothing CellBlocks, but it's a non-use case
    // for now.
    tassert(11093600, "Top level field doesn't exist", node != nullptr);

    for (size_t i = 0; i < tags.size(); ++i) {
        for (auto& rec : _filterPositionInfoRecorders) {
            rec.newDoc();
        }
        for (auto& rec : _projPositionInfoRecorders) {
            rec.newDoc();
        }

        walkField<BlockProjectionPositionInfoRecorder>(node,
                                                       tags[i],
                                                       vals[i],
                                                       nullptr /* bsonPtr */,
                                                       visitElementExtractorCallback,
                                                       /*traverseArrays*/ true);

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

        walkBsonObj<BlockProjectionPositionInfoRecorder>(&_root,
                                                         bitcastFrom<const char*>(obj.objdata()),
                                                         obj.objdata(),
                                                         visitElementExtractorCallback,
                                                         true /* traverseArrays */);

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
        if (path.type == PathRequestType::kFilter) {
            auto& recorder = _filterPositionInfoRecorders[filterRecorderIdx];
            matBlock->_deblocked = recorder.extractValues();
            matBlock->_filterPosInfo = std::move(recorder.posInfo);
            ++filterRecorderIdx;
        } else if (path.type == PathRequestType::kProject) {
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
    const std::vector<PathRequest>& pathReqs) {
    return std::make_unique<BSONExtractorImpl>(pathReqs);
}

std::vector<std::unique_ptr<CellBlock>> extractCellBlocksFromBsons(
    const std::vector<PathRequest>& pathReqs, const std::vector<BSONObj>& bsons) {

    auto extractor = BSONCellExtractor::make(pathReqs);
    return extractor->extractFromBsons(bsons);
}

std::vector<const char*> extractValuePointersFromBson(BSONObj& obj,
                                                      value::PathRequest pathRequest,
                                                      bool traverseArrays) {
    std::vector<value::PathRequest> pathrequests{pathRequest};
    auto extractor = BSONExtractorImpl(pathrequests);

    std::vector<const char*> bsonPointers;

    // Callback to record pointer values in bsonPointers.
    auto recordValuePointer =
        [&bsonPointers](ObjectWalkNode<BlockProjectionPositionInfoRecorder>* node,
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

    walkBsonObj<BlockProjectionPositionInfoRecorder>(extractor.getRoot(),
                                                     bitcastFrom<const char*>(obj.objdata()),
                                                     obj.objdata(),
                                                     recordValuePointer,
                                                     traverseArrays);
    return bsonPointers;
}
}  // namespace mongo::sbe::value
