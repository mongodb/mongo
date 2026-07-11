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
    // A dropping subclass ($search idLookup skipping orphans / unresolved _ids) can enrich an
    // entire window to nothing. That is not EOF, so refill rather than reporting EOF early. The
    // loop is bounded: it refills only after the input buffer is drained (asserted below), so each
    // pass consumes >=1 fresh upstream event and stops once there is output, a buffered
    // non-advanced result (EOF/pause/control), or a buffered exception to surface.
    while (_phase != Phase::kEmit) {
        if (_phase == Phase::kBuffer) {
            fillBatch();
        }
        enrichBatch();
        if (_outputBuffer.empty() && !_bufferedNonAdvancedResult.has_value() &&
            !_bufferedException.has_value()) {
            tassert(12916809,
                    "input must be drained before refilling after a fully-dropped window",
                    _inputBuffer.empty());
            _phase = Phase::kBuffer;
        }
    }
    return emit();
}

void BatchedEnrichmentStage::fillBatch() {
    tassert(12916805,
            "fillBatch() runs only in the kBuffer phase with nothing buffered or pending",
            _phase == Phase::kBuffer && _outputBuffer.empty() && _inputBuffer.empty() &&
                !_bufferedNonAdvancedResult.has_value() && !_bufferedException.has_value());

    // The subclass has capped its own output (e.g. idLookup's 'limit' is already met): don't pull
    // any more upstream. Queue EOF so emit() surfaces it (and fires onExhausted()). enrichBatch()
    // then no-ops on the empty input.
    if (shouldStopEmittingDocuments()) {
        _bufferedNonAdvancedResult = GetNextResult::makeEOF();
        _phase = Phase::kEnrich;
        return;
    }

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
            // If it is EOF, emit() fires onExhausted() when it surfaces it.
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

    // Input is empty when fill only cached a non-advanced result (EOF/pause) or the subclass
    // capped its output up front; nothing to enrich.
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

        // The subclass hit its cap mid-window: stop before enriching (and paying for) more.
        if (shouldStopEmittingDocuments()) {
            break;
        }

        auto event = std::move(_inputBuffer.front());
        _inputBuffer.pop_front();
        tassert(12916803, "only advanced events are buffered for enrichment", event.isAdvanced());

        // Track pop then push so the net memory delta reflects any size change from enrichment.
        // enrich() may drop the event (boost::none), in which case nothing enters the output.
        trackPop(event.getDocument());
        auto enriched = enrich(event.releaseDocument());
        if (!enriched) {
            continue;
        }

        trackPush(*enriched);
        outputBytes += static_cast<size_t>(enriched->getApproximateSize());
        _outputBuffer.push_back(GetNextResult{std::move(*enriched)});
    }

    // The cap tripped mid-window with input still buffered: drop the remainder so the refill
    // invariant (input drained before refilling) holds. The doGetNext() loop then re-enters
    // fillBatch(), which queues EOF; emit() surfaces it and fires onExhausted(). Only the
    // mandatory leftover-drain lives here.
    if (shouldStopEmittingDocuments()) {
        discardInputBuffer();
    }
}

GetNextResult BatchedEnrichmentStage::emit() {
    tassert(12916807, "emit() runs only in the kEmit phase", _phase == Phase::kEmit);

    // When entering kEmit phase the output buffer is empty only when fill stopped on a non-advanced
    // result or a buffered exception, or the subclass capped its output; one of those must be set.
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

        // Fire onExhausted() exactly as the terminating EOF is surfaced -- the single place that
        // signals exhaustion, whether it came from upstream EOF or the subclass's own cap. A
        // control event or pause is not exhaustion, so it does not fire. The stage may return EOF
        // more than once, so onExhausted() must be idempotent.
        if (result.isEOF()) {
            onExhausted();
        }
        return result;
    }

    auto next = std::move(_outputBuffer.front());
    _outputBuffer.pop_front();
    trackPop(next.getDocument());

    // Resume the next call from what is still buffered: more output to drain, a window suspended by
    // the output-byte cap, a buffered non-advanced result or exception to surface, or nothing left
    // so refill.
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

void BatchedEnrichmentStage::discardInputBuffer() {
    for (const auto& event : _inputBuffer) {
        trackPop(event.getDocument());
    }
    _inputBuffer.clear();
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
