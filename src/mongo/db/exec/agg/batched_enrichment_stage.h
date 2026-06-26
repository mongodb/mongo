/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
         * maxInputEvents (enrich is 1:1).
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
     * Must not call back into this stage (getNext() or any other method).
     */
    virtual Document enrich(Document event) = 0;

private:
    /**
     * The phases the stage moves through, in order. doGetNext() resumes at '_phase' and falls
     * through the later phases, so each runs at most once per call (no loop).
     *   kBuffer: nothing buffered; gather upstream events into '_inputBuffer'.
     *   kEnrich: input buffered, output drained; enrich within a batch window into '_outputBuffer'.
     *   kEmit:   return one enriched document, else the buffered non-advanced result.
     */
    enum class Phase { kBuffer, kEnrich, kEmit };

    GetNextResult doGetNext() final;
    void doDispose() final;

    /**
     * Pulls upstream events into '_inputBuffer' until a fill cap trips or upstream yields a
     * non-advanced result (control event, pause, or EOF), which is cached in
     * '_bufferedNonAdvancedResult'. Entered only in kBuffer (all buffers empty) and hands off to
     * kEnrich. Holds no resources. Foreign upstream stage code runs here.
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
     * Returns one enriched document from '_outputBuffer'. When empty, the buffered non-advanced
     * result (then clears it).
     */
    GetNextResult emit();

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
     * The phase doGetNext() resumes at. Starts at kBuffer (empty).
     */
    Phase _phase = Phase::kBuffer;
};

}  // namespace mongo::exec::agg
