// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <deque>
#include <string_view>

#include <boost/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {

/**
 * Reusable base for stages that enrich each event with the result of a point lookup (change-stream
 * post-image / pre-image, $search idLookup), without holding resources across getNext() calls.
 *
 * Three phases:
 *   fill:   buffer upstream events while holding nothing (bounded by maxInputEvents/maxInputBytes).
 *   enrich: inside an open resource scope, transform buffered events into the output buffer until
 *           maxOutputBytes trips or the input drains.
 *   drain:  emit enriched events one per getNext(), holding nothing.
 *
 * enrich() is the subclass's only execution hook and runs only inside an open scope, so resources
 * can never be held while other stage code runs. The base names no lookup executor or scope type;
 * the subclass implements beginBatch()/closeBatch() and enrich(). One fill batch may open several
 * scopes, since the output-byte cap can suspend enrichment mid-input, so "batch" in
 * beginBatch/closeBatch is the enrich sub-batch, not the fill batch.
 *
 * enrich() may drop an event by returning boost::none (e.g. $search idLookup drops a document whose
 * _id no longer resolves, or an orphan). A subclass may also cap its own output via
 * shouldStopEmittingDocuments(): once it returns true the stage stops pulling upstream, drops any
 * still-buffered input, and surfaces EOF after the already-enriched output drains. Because a whole
 * fill window can be dropped, doGetNext() refills (loops back to the fill phase) instead of
 * assuming every fill yields at least one emittable result; the loop is bounded because each fill
 * consumes at least one upstream event or records a boundary (EOF/pause/control) and upstream is
 * finite.
 */
class BatchedEnrichmentStage : public Stage {
public:
    /**
     * Construct with designated initializers, e.g.
     * 'Limits{.maxInputEvents = N, .maxInputBytes = M, .maxOutputBytes = K}'.
     */
    struct Limits {
        /**
         * Stop filling after this many buffered events.
         */
        size_t maxInputEvents;

        /**
         * Stop filling after the buffered input crosses this many bytes.
         */
        size_t maxInputBytes;

        /**
         * Suspend the enrich scope after the event that crosses this many output bytes. Enrichment
         * can inflate an event (a post-image up to 16MB), so the meaningful memory bound is on the
         * output. There is no output event-count cap: output count is already bounded by
         * maxInputEvents (enrich emits at most one output per input, and may emit none).
         */
        size_t maxOutputBytes;
    };

    /**
     * Test-only: bytes currently buffered across both deques.
     */
    int64_t bufferedMemoryBytes_forTest() const {
        return _memTracker.inUseTrackedMemoryBytes();
    }

    /**
     * Test-only: peak bytes buffered across both deques.
     */
    int64_t peakBufferedMemoryBytes_forTest() const {
        return _memTracker.peakTrackedMemoryBytes();
    }

    /**
     * Release / re-bind the memory tracker's operation-scoped base across getMore opCtx swaps. The
     * base lives on the OperationContext and is not carried across the $search results pipeline's
     * reattach, so it must be dropped on detach and re-bound on reattach to avoid a dangling
     * pointer. A subclass overriding these must call the base implementation.
     *
     * TODO SERVER-131203: these overrides (and the SimpleMemoryUsageTracker::resetBase() /
     * OperationMemoryUsageTracker::rebindToOperation() helpers they rely on) are a stopgap for the
     * getMore opCtx-swap crash and are not a pattern to reuse; remove them once this stage's memory
     * tracking is properly integrated with the operation memory tracker.
     */
    void detachFromOperationContext() override;
    void reattachToOperationContext(OperationContext* opCtx) override;

protected:
    BatchedEnrichmentStage(std::string_view stageName,
                           const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                           Limits limits);

    /**
     * Open / close the resource scope for one enrich sub-batch. enrich() runs only while a scope is
     * open. closeBatch() is noexcept and runs via a ScopeGuard, so it executes even when enrich()
     * throws, guaranteeing nothing attached survives into the fill phase.
     * If beginBatch() throws, closeBatch() will NOT be called, implementations must be
     * all-or-nothing (leave no acquired resources if they throw).
     */
    virtual void beginBatch() = 0;
    virtual void closeBatch() noexcept = 0;

    /**
     * Enrich one buffered event; called only inside an open scope, and only for data events
     * (kAdvanced). Non-advanced events pass through untouched without entering enrich().
     *
     * Returns the enriched document to emit, or boost::none to drop the event (emit nothing for
     * it).
     *
     * Must not call back into this stage (getNext() or any other method).
     */
    virtual boost::optional<Document> enrich(Document event) = 0;

    /**
     * When a subclass wants to stop emitting before its upstream is exhausted (e.g. $search
     * idLookup honouring a 'limit'), this returns true once no further output should be produced.
     * Note the source may still have data -- the subclass is choosing to stop, not observing EOF.
     * The base then stops pulling upstream, drops any still-buffered input, and surfaces EOF after
     * the already-enriched output drains. Checked per-event during enrichment as well, so a window
     * never emits past the cap. The default never caps (a stage bounded only by its upstream).
     */
    virtual bool shouldStopEmittingDocuments() const {
        return false;
    }

    /**
     * Invoked when the stage observes that its input is exhausted -- either upstream returned EOF
     * or shouldStopEmittingDocuments() tripped. Fired by emit() as the terminating EOF is surfaced
     * (the single exhaustion signal for both causes). May fire on more than one getNext() once
     * exhausted, so implementations must be idempotent. Default is a no-op.
     */
    virtual void onExhausted() {}

private:
    /**
     * The phases the stage moves through, in order. doGetNext() resumes at '_phase' and runs the
     * phases to produce one result. It loops back to kBuffer when a fill window yields nothing to
     * emit (every event dropped) and no non-advanced result or buffered exception is pending, so a
     * fully-dropped window refills rather than returning early. kBuffer: nothing buffered; gather
     * upstream events into '_inputBuffer'. kEnrich: input buffered, output drained; enrich within a
     * batch window into '_outputBuffer'. kEmit: return one enriched document, else the buffered
     * non-advanced result (or rethrow the buffered exception).
     */
    enum class Phase { kBuffer, kEnrich, kEmit };

    GetNextResult doGetNext() final;
    void doDispose() final;

    /**
     * Pulls upstream events into '_inputBuffer' until a fill cap trips or upstream yields a
     * non-advanced result (control event, pause, or EOF), which is cached in
     * '_bufferedNonAdvancedResult'. Entered only in kBuffer (all buffers empty) and hands off to
     * kEnrich. Holds no resources. Foreign upstream stage code runs here.
     *
     * A 'ChangeStreamInvalidated' or 'CloseChangeStream' from upstream signals normal change stream
     * closure, not an error, via a queue-then-throw protocol: upstream returns a data event (e.g.
     * "invalidate") and only throws on the following call. Such exception is buffered and the
     * batch will no further be filled.
     */
    void fillBatch();

    /**
     * Enriches one bounded window of '_inputBuffer' into '_outputBuffer'. Any leftover
     * '_inputBuffer' (the output-byte cap tripped mid-window) is a suspended sub-batch resumed by a
     * later call once '_outputBuffer' drains. A no-op when no input is buffered (fill cached only a
     * non-advanced result). Entered in kEnrich. Hands off to kEmit.
     */
    void enrichBatch();

    /**
     * Returns the next enriched document from '_outputBuffer'. When the output buffer is empty a
     * buffered non-advanced result (control event, pause, or the terminating EOF) or a buffered
     * exception must be pending -- doGetNext() never calls emit() on a fully-dropped window -- so
     * this rethrows the buffered exception if one is stashed, else returns that result. Fires
     * onExhausted() as it surfaces a terminating EOF.
     */
    GetNextResult emit();

    /**
     * Drops all still-buffered input events (accounting for the freed memory). Used when
     * shouldStopEmittingDocuments() ends the stage with input still buffered.
     */
    void discardInputBuffer();

    /**
     * Account for a document entering (trackPush) or leaving (trackPop) a buffer.
     */
    void trackPush(const Document& doc);
    void trackPop(const Document& doc);

    /**
     * Pre-enrichment events. GetNextResult preserves the data/control kind and arrival order.
     *
     * TODO SERVER-129349: both buffers are bounded (by maxInputEvents / maxOutputBytes), so a
     * preallocated fixed-size ring buffer would avoid the per-push/pop allocation churn of
     * std::deque on this hot path.
     */
    std::deque<GetNextResult> _inputBuffer;
    /**
     * Enriched events awaiting drain.
     */
    std::deque<GetNextResult> _outputBuffer;

    const Limits _limits;

    /**
     * Bytes buffered across both buffers, feeding operation/cursor memory accounting.
     */
    SimpleMemoryUsageTracker _memTracker;

    /**
     * A cached non-advanced upstream result (control event, pause, or EOF) to surface in order once
     * the buffers drain.
     */
    boost::optional<GetNextResult> _bufferedNonAdvancedResult;

    /**
     * A "normal cursor lifecycle" exception caught by fillBatch() after at least one event was
     * already buffered. Deferred so those already-buffered events are enriched and emitted first.
     * Rethrown by emit() once both buffers are fully drained.
     */
    boost::optional<Status> _bufferedException;

    /**
     * The phase doGetNext() resumes at. Starts at kBuffer (empty).
     */
    Phase _phase = Phase::kBuffer;
};

}  // namespace mongo::exec::agg
