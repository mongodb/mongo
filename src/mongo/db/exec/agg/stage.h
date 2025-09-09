/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace exec {
namespace agg {

/**
 * This is what is returned from the main 'Stage' API: getNext(). It is essentially a
 * (ReturnStatus, Document) pair, with the first entry being used to communicate information
 * about the execution of the 'Stage', such as whether or not it has been exhausted.
 */
class GetNextResult {
public:
    enum class ReturnStatus {
        // There is a result to be processed.
        kAdvanced,

        // There is a control document to be processed. Control documents are documents with
        // additional metadata that will be passed on by most pipeline stages unmodified and
        // without further inspection.
        // Currently only produced inside change streams.
        kAdvancedControlDocument,

        // There will be no further results.
        kEOF,
        // There is not a result to be processed yet, but there may be more results in the future.
        // If a 'Stage' retrieves this status from its child, it must propagate it
        // without doing any further work.
        kPauseExecution,
    };

    static GetNextResult makeEOF() {
        return GetNextResult(ReturnStatus::kEOF);
    }

    static GetNextResult makePauseExecution() {
        return GetNextResult(ReturnStatus::kPauseExecution);
    }

    /**
     * Builder method to create an 'advanced' GetNextResult containing a change stream control
     * event.
     */
    static GetNextResult makeAdvancedControlDocument(Document&& result) {
        dassert(result.metadata().isChangeStreamControlEvent());
        return GetNextResult(ReturnStatus::kAdvancedControlDocument, std::move(result));
    }

    /**
     * Shortcut constructor for the common case of creating an 'advanced' GetNextResult from the
     * given 'result'. Accepts only an rvalue reference as an argument, since a Stage
     * will want to move 'result' into this GetNextResult, and should have to opt in to making a
     * copy.
     */
    /* implicit */ GetNextResult(Document&& result)
        : GetNextResult(ReturnStatus::kAdvanced, std::move(result)) {
        dassert(!_result.metadata().isChangeStreamControlEvent());
    }

    /**
     * Gets the result document. It is an error to call this if both 'isAdvanced()' and
     * 'isAdvancedControlDocument()' return false.
     */
    const Document& getDocument() const {
        dassert(isAdvanced() || isAdvancedControlDocument());
        return _result;
    }

    /**
     * Releases the result document, transferring ownership to the caller. It is an error to
     * call this if both 'isAdvanced()' and 'isAdvancedControlDocument()' return false.
     */
    Document releaseDocument() {
        dassert(isAdvanced() || isAdvancedControlDocument());
        return std::move(_result);
    }

    ReturnStatus getStatus() const {
        return _status;
    }

    bool isAdvanced() const {
        return _status == ReturnStatus::kAdvanced;
    }

    bool isEOF() const {
        return _status == ReturnStatus::kEOF;
    }

    bool isPaused() const {
        return _status == ReturnStatus::kPauseExecution;
    }

    bool isAdvancedControlDocument() const {
        return _status == ReturnStatus::kAdvancedControlDocument;
    }

private:
    // Private constructors, called from public constructor or builder methods above.
    GetNextResult(ReturnStatus status) : _status(status) {}

    GetNextResult(ReturnStatus status, Document&& result)
        : _status(status), _result(std::move(result)) {}

    ReturnStatus _status;
    Document _result;
};

// TODO SPM-4106: Remove inheritance once the refactoring is done.
class Stage : public virtual RefCountable {
public:
    Stage(StringData stageName, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);
    ~Stage() override {}

    /**
     * The stage spills its data and asks from all its children to spill their data as well.
     */
    void forceSpill() {
        pExpCtx->checkForInterrupt();
        doForceSpill();
        if (pSource) {
            pSource->forceSpill();
        }
    }

    /**
     * The main execution API of a Stage. Returns an intermediate query result
     * generated by this Stage.
     *
     * For performance reasons, a streaming stage must not keep references to documents across calls
     * to getNext(). Such stages must retrieve a result from their child and then release it (or
     * return it) before asking for another result. Failing to do so can result in extra work, since
     * the Document/Value library must copy data on write when that data has a refcount above one.
     */
    GetNextResult getNext() {
        pExpCtx->checkForInterrupt();

        if (MONGO_likely(!pExpCtx->shouldCollectDocumentSourceExecStats())) {
            return doGetNext();
        }

        auto serviceCtx = pExpCtx->getOperationContext()->getServiceContext();
        dassert(serviceCtx);

        auto timer = getOptTimer(serviceCtx);

        ++_commonStats.works;

        GetNextResult next = doGetNext();
        _commonStats.advanced += static_cast<unsigned>(next.isAdvanced());
        return next;
    }

    /**
     * Informs the stage that it is no longer needed and can release its resources. After dispose()
     * is called the stage must still be able to handle calls to getNext(), but can return kEOF.
     *
     * This is a non-virtual public interface to ensure dispose() is threaded through the entire
     * pipeline. Subclasses should override doDispose() to implement their disposal.
     *
     * The pipeline and its constituent stages should be attached to a valid 'OperationContext'
     * pointer when calling this method.
     */
    void dispose() {
        doDispose();
        if (pSource) {
            pSource->dispose();
        }
    }

    /**
     * Get the CommonStats for this Stage.
     */
    const CommonStats& getCommonStats() const {
        return _commonStats;
    }

    /**
     * Get the stats specific to the Stage. It is legal for the Stage
     * to return nullptr to indicate that no specific stats are available.
     */
    virtual const SpecificStats* getSpecificStats() const {
        return nullptr;
    }

    /**
     * Returns the expression context from the stage's context.
     */
    const boost::intrusive_ptr<ExpressionContext>& getContext() const {
        return pExpCtx;
    }

    /**
     * Set the underlying source this stage should use to get Documents from. Must not throw
     * exceptions.
     */
    virtual void setSource(Stage* source) {
        pSource = source;
    }

    virtual void detachFromOperationContext() {}

    virtual void reattachToOperationContext(OperationContext* opCtx) {}

    /**
     * Validate that all operation contexts associated with this document source, including any
     * subpipelines, match the argument.
     */
    virtual bool validateOperationContext(const OperationContext* opCtx) const {
        return getContext()->getOperationContext() == opCtx;
    }

    virtual bool usedDisk() const {
        return false;
    }

    virtual Document getExplainOutput(
        const SerializationOptions& opts = SerializationOptions{}) const;

protected:
    /**
     * The main execution API of a Stage. Returns an intermediate query result
     * generated by this Stage. See comment at getNext().
     */
    virtual GetNextResult doGetNext() = 0;

    /**
     * Release any resources held by this stage. After doDispose() is called the stage must still be
     * able to handle calls to getNext(), but can return kEOF.
     */
    virtual void doDispose() {}


    /**
     * Spills the stage's data to disk. Stages that can spill their own data need to override this
     * method.
     */
    virtual void doForceSpill() {}

    /**
     * Most Stages have an underlying source they get their data
     * from. This is a convenience for them.
     *
     * The default implementation of setSourceStage() sets this; if you don't
     * need a source, override that to verify(). The default is to verify()
     * if this has already been set.
     */
    Stage* pSource;

    boost::intrusive_ptr<ExpressionContext> pExpCtx;

    CommonStats _commonStats;

private:
    /**
     * Returns an optional timer which is used to collect the execution time.
     * May return boost::none if it is not necessary to collect timing info.
     */
    boost::optional<ScopedTimer> getOptTimer(ServiceContext* serviceCtx) {
        if (serviceCtx &&
            _commonStats.executionTime.precision != QueryExecTimerPrecision::kNoTiming) {
            if (MONGO_likely(_commonStats.executionTime.precision ==
                             QueryExecTimerPrecision::kMillis)) {
                return boost::optional<ScopedTimer>(
                    boost::in_place_init,
                    &_commonStats.executionTime.executionTimeEstimate,
                    serviceCtx->getFastClockSource());
            } else {
                return boost::optional<ScopedTimer>(
                    boost::in_place_init,
                    &_commonStats.executionTime.executionTimeEstimate,
                    serviceCtx->getTickSource());
            }
        }
        return boost::none;
    }
};

// TODO SERVER-105494: Use 'std::unique_ptr' instead of 'boost::intrusive_ptr'.
using StagePtr = boost::intrusive_ptr<Stage>;

}  // namespace agg
}  // namespace exec
}  // namespace mongo
