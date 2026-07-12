// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/batched_enrichment_stage.h"

#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

namespace mongo::exec::agg {

BatchedEnrichmentStage::BatchedEnrichmentStage(
    std::string_view stageName,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    Limits limits)
    : Stage(stageName, pExpCtx),
      _limits(limits),
      _memTracker(
          OperationMemoryUsageTracker::createChunkedSimpleMemoryUsageTrackerForStage(*pExpCtx)) {
    tassert(12916800, "maxInputEvents must be at least 1", _limits.maxInputEvents >= 1);
    tassert(12916801, "maxInputBytes must be at least 1", _limits.maxInputBytes >= 1);
    tassert(12916802, "maxOutputBytes must be at least 1", _limits.maxOutputBytes >= 1);
}

void BatchedEnrichmentStage::trackPush(const Document& doc) {
    _memTracker.add(static_cast<int64_t>(doc.getApproximateSize()));
}

void BatchedEnrichmentStage::trackPop(const Document& doc) {
    _memTracker.add(-static_cast<int64_t>(doc.getApproximateSize()));
}

GetNextResult BatchedEnrichmentStage::doGetNext() {
    // Resume at current '_phase' and fall through the later phases, so each runs at most once and
    // the call terminates without looping.
    switch (_phase) {
        case Phase::kBuffer:
            fillBatch();
            [[fallthrough]];
        case Phase::kEnrich:
            enrichBatch();
            [[fallthrough]];
        case Phase::kEmit:
            return emit();
    }
    MONGO_UNREACHABLE_TASSERT(12916804);
}

void BatchedEnrichmentStage::fillBatch() {
    tassert(12916805,
            "fillBatch() runs only in the kBuffer phase with nothing buffered or pending",
            _phase == Phase::kBuffer && _outputBuffer.empty() && _inputBuffer.empty() &&
                !_bufferedNonAdvancedResult.has_value() && !_bufferedException.has_value());

    while (_inputBuffer.size() < _limits.maxInputEvents) {
        // Stop once the buffered input crosses the byte budget, so a few large events cannot blow
        // memory before enrichment. Always admit at least one event.
        if (!_inputBuffer.empty() &&
            static_cast<size_t>(_memTracker.inUseTrackedMemoryBytes()) >= _limits.maxInputBytes) {
            break;
        }

        // Catch change stream specific exceptions, which need to be emitted after all buffered
        // events.
        boost::optional<GetNextResult> upstream;
        try {
            upstream = pSource->getNext();
        } catch (const ExceptionFor<ErrorCodes::ChangeStreamInvalidated>& ex) {
            _bufferedException = ex.toStatus();
            break;
        } catch (const ExceptionFor<ErrorCodes::CloseChangeStream>& ex) {
            _bufferedException = ex.toStatus();
            break;
        }

        if (!upstream->isAdvanced()) {
            // Only data events are buffered. Anything else (a control event, pause, or EOF) ends
            // the batch: cache it to surface in order once the buffered data drains, then stop
            // filling. A non-advanced event is thus a natural boundary, never buffered or enriched.
            _bufferedNonAdvancedResult = std::move(*upstream);
            break;
        }

        trackPush(upstream->getDocument());
        _inputBuffer.push_back(std::move(*upstream));
    }

    _phase = Phase::kEnrich;
}

void BatchedEnrichmentStage::enrichBatch() {
    tassert(12916806, "enrichBatch() runs only in the kEnrich phase", _phase == Phase::kEnrich);

    // Advance the state. Even in case of an exception we should stay in kEmit, not kEnrich.
    _phase = Phase::kEmit;

    // Early exit if there is nothing to enrich.
    if (_inputBuffer.empty()) {
        return;
    }

    bool batchOpened = false;
    ScopeGuard closeBatchOnExit{[&] {
        if (batchOpened) {
            closeBatch();
        }
    }};
    beginBatch();
    batchOpened = true;

    size_t outputBytes = 0;
    while (!_inputBuffer.empty() && outputBytes < _limits.maxOutputBytes) {
        // Per-event interrupt check: a window can be large and pass-through events do no other
        // check. checkForInterrupt() only throws on a killed operation, it never yields.
        pExpCtx->checkForInterrupt();

        auto event = std::move(_inputBuffer.front());
        _inputBuffer.pop_front();
        tassert(12916803, "only advanced events are buffered for enrichment", event.isAdvanced());

        // The event leaves the input buffer and its enriched form enters the output buffer.
        // Track both so the net delta reflects any size change from enrichment.
        trackPop(event.getDocument());
        auto enriched = enrich(event.releaseDocument());
        trackPush(enriched);

        outputBytes += static_cast<size_t>(enriched.getApproximateSize());
        _outputBuffer.push_back(GetNextResult{std::move(enriched)});
    }
}

GetNextResult BatchedEnrichmentStage::emit() {
    tassert(12916807, "emit() runs only in the kEmit phase", _phase == Phase::kEmit);

    // When entering kEmit phase the output buffer is empty only when fill stopped on a non-advanced
    // result or a buffered exception, so one of the two must be set here.
    if (_outputBuffer.empty()) {
        _phase = Phase::kBuffer;

        if (_bufferedException.has_value()) {
            // Rethrow the buffered exception. Every event buffered ahead of it has been emitted.
            auto status = std::move(*_bufferedException);
            _bufferedException.reset();
            uassertStatusOK(status);
        }

        tassert(12916808,
                "a buffered non-advanced result must exist when the output buffer is empty",
                _bufferedNonAdvancedResult.has_value());
        auto result = std::move(*_bufferedNonAdvancedResult);
        _bufferedNonAdvancedResult.reset();
        return result;
    }

    auto next = std::move(_outputBuffer.front());
    _outputBuffer.pop_front();
    trackPop(next.getDocument());

    // Resume the next call from what is still buffered: more output to drain, a window
    // suspended by the output-byte cap, a buffered non-advanced result or exception to surface, or
    // nothing left so refill.
    if (!_outputBuffer.empty()) {
        _phase = Phase::kEmit;
    } else if (!_inputBuffer.empty()) {
        _phase = Phase::kEnrich;
    } else if (_bufferedNonAdvancedResult.has_value() || _bufferedException.has_value()) {
        _phase = Phase::kEmit;
    } else {
        _phase = Phase::kBuffer;
    }
    return next;
}

void BatchedEnrichmentStage::doDispose() {
    // No scope can be open here: enrichBatch()'s ScopeGuard closes it before any getNext() returns
    // or throws, and dispose() runs only between getNext() calls.
    _inputBuffer.clear();
    _outputBuffer.clear();
    _memTracker.set(0);
    _bufferedNonAdvancedResult.reset();
    _bufferedException.reset();
    _phase = Phase::kBuffer;
}

}  // namespace mongo::exec::agg
