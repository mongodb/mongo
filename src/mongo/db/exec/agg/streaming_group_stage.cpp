// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/streaming_group_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/document_source_streaming_group.h"

#include <string_view>

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceStreamingGroupToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto streamingGroupDS =
        boost::dynamic_pointer_cast<DocumentSourceStreamingGroup>(documentSource);

    tassert(10422901, "expected 'DocumentSourceStreamingGroup' type", streamingGroupDS);

    return make_intrusive<exec::agg::StreamingGroupStage>(
        streamingGroupDS->kStageName,
        streamingGroupDS->getExpCtx(),
        streamingGroupDS->_groupProcessor,
        streamingGroupDS->_monotonicExpressionIndexes);
}

namespace exec {
namespace agg {

REGISTER_AGG_STAGE_MAPPING(_internalStreamingGroup,
                           DocumentSourceStreamingGroup::id,
                           documentSourceStreamingGroupToStageFn)

StreamingGroupStage::StreamingGroupStage(std::string_view stageName,
                                         const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                         const std::shared_ptr<GroupProcessor>& groupProcessor,
                                         const std::vector<size_t>& monotonicExpressionIndexes)
    : GroupBaseStage(stageName, pExpCtx, groupProcessor),
      _sourceDepleted(false),
      _monotonicExpressionIndexes(monotonicExpressionIndexes) {};

GetNextResult StreamingGroupStage::doGetNext() {
    auto result = _groupProcessor->getNext();
    if (result) {
        return GetNextResult(std::move(*result));
    } else if (_sourceDepleted) {
        dispose();
        return GetNextResult::makeEOF();
    }

    auto prepareResult = readyNextBatch();
    if (prepareResult.isPaused()) {
        return prepareResult;
    }

    result = _groupProcessor->getNext();
    if (!result) {
        return GetNextResult::makeEOF();
    }
    return GetNextResult(std::move(*result));
}

GetNextResult StreamingGroupStage::readyNextBatch() {
    _groupProcessor->reset();
    GetNextResult input = getNextDocument();
    return readyNextBatchInner(input);
}

GetNextResult StreamingGroupStage::getNextDocument() {
    if (_firstDocumentOfNextBatch) {
        GetNextResult result = std::move(_firstDocumentOfNextBatch.value());
        _firstDocumentOfNextBatch.reset();
        return result;
    }
    return pSource->getNext();
}

// This separate NOINLINE function is used here to decrease stack utilization of
// readyNextBatch() and prevent stack overflows.
MONGO_COMPILER_NOINLINE DocumentSource::GetNextResult StreamingGroupStage::readyNextBatchInner(
    GetNextResult input) {
    _groupProcessor->setExecutionStarted();
    // Calculate groups until we either exaust pSource or encounter change in monotonic id
    // expression, which means all current groups are finalized.
    for (; input.isAdvanced(); input = pSource->getNext()) {
        auto root = input.releaseDocument();
        auto groupKey = _groupProcessor->computeGroupKey(root);

        if (isBatchFinished(groupKey)) {
            _firstDocumentOfNextBatch = std::move(root);
            _groupProcessor->readyGroups();
            return input;
        }

        _groupProcessor->add(groupKey, root);
    }

    switch (input.getStatus()) {
        case DocumentSource::GetNextResult::ReturnStatus::kAdvanced: {
            MONGO_UNREACHABLE;  // We consumed all advances above.
        }
        case DocumentSource::GetNextResult::ReturnStatus::kAdvancedControlDocument: {
            MONGO_UNREACHABLE_TASSERT(
                10358903);  // No support for control events in this document source.
        }
        case DocumentSource::GetNextResult::ReturnStatus::kPauseExecution: {
            return input;  // Propagate pause.
        }
        case DocumentSource::GetNextResult::ReturnStatus::kEOF: {
            _groupProcessor->readyGroups();
            _sourceDepleted = true;
            return input;
        }
    }
    MONGO_UNREACHABLE;
}

bool StreamingGroupStage::isBatchFinished(const Value& id) {
    if (_groupProcessor->getIdExpressions().size() == 1) {
        tassert(7026706,
                "if there are no explicit id fields, it is only one monotonic expression with id 0",
                _monotonicExpressionIndexes.size() == 1 && _monotonicExpressionIndexes[0] == 0);
        return checkForBatchEndAndUpdateLastIdValues([&](size_t) { return id; });
    } else {
        tassert(7026707,
                "if there are explicit id fields, internal representation of id is an array",
                id.isArray());
        const std::vector<Value>& idValues = id.getArray();
        return checkForBatchEndAndUpdateLastIdValues([&](size_t i) { return idValues[i]; });
    }
}

template <typename IdValueGetter>
bool StreamingGroupStage::checkForBatchEndAndUpdateLastIdValues(
    const IdValueGetter& idValueGetter) {
    auto assertStreamable = [&](Value value) {
        // Nullish and array values will mess us up because they sort differently than they
        // group. A null and a missing value will compare equal in sorting, but could result in
        // different groups, e.g. {_id: {x: null, y: null}} vs {_id: {}}. An array value will
        // sort by the min or max element, with no tie breaking, but group by the whole array.
        // This means that two of the exact same array could appear in the input sequence, but
        // with a different array in the middle of them, and that would still be considered
        // sorted. That would break our batching group logic.
        uassert(7026708,
                "Monotonic value should not be missing, null or an array",
                !value.nullish() && !value.isArray());
        return value;
    };

    // If _lastMonotonicIdFieldValues is empty, it is the first document, so the only thing we
    // need to do is initialize it.
    if (_lastMonotonicIdFieldValues.empty()) {
        for (size_t i : _monotonicExpressionIndexes) {
            _lastMonotonicIdFieldValues.push_back(assertStreamable(idValueGetter(i)));
        }
        return false;
    } else {
        bool batchFinished = false;
        for (size_t index = 0; index < _monotonicExpressionIndexes.size(); ++index) {
            Value& oldId = _lastMonotonicIdFieldValues[index];
            const Value& id = assertStreamable(idValueGetter(_monotonicExpressionIndexes[index]));
            if (pExpCtx->getValueComparator().compare(oldId, id) != 0) {
                oldId = id;
                batchFinished = true;
            }
        }
        return batchFinished;
    }
}

}  // namespace agg
}  // namespace exec
}  // namespace mongo
