// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/json.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {

class MockStage : public Stage {
public:
    MockStage(std::string_view stageName,
              const boost::intrusive_ptr<ExpressionContext>& expCtx,
              std::deque<GetNextResult> results);

    static boost::intrusive_ptr<MockStage> createForTest(
        Document doc, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Convenience constructor that works with a vector of BSONObj or vector of Documents.
     */
    template <typename Doc>
    static boost::intrusive_ptr<MockStage> createForTest(
        const std::vector<Doc>& docs, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        std::deque<GetNextResult> results;
        for (auto&& doc : docs) {
            results.emplace_back(Document(doc));
        }
        return make_intrusive<MockStage>(
            DocumentSourceMock::kStageName, expCtx, std::move(results));
    }

    /**
     * Convenience constructor that works with a BSONObj or Document.
     */
    template <typename Doc>
    static boost::intrusive_ptr<MockStage> createForTest(
        const Doc& doc, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return createForTest(Document(doc), expCtx);
    }

    static boost::intrusive_ptr<MockStage> createForTest(
        std::deque<GetNextResult> results, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return make_intrusive<MockStage>(
            DocumentSourceMock::kStageName, expCtx, std::move(results));
    }

    static boost::intrusive_ptr<MockStage> createForTest(
        const char* json, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        std::deque<GetNextResult> results{Document(fromjson(json))};
        return make_intrusive<MockStage>(
            DocumentSourceMock::kStageName, expCtx, std::move(results));
    }

    static boost::intrusive_ptr<MockStage> createForTest(
        const std::initializer_list<const char*>& jsons,
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        std::deque<GetNextResult> results;
        for (auto&& json : jsons) {
            results.emplace_back(Document(fromjson(json)));
        }
        return make_intrusive<MockStage>(
            DocumentSourceMock::kStageName, expCtx, std::move(results));
    }

    /**
     * Test helper to set the source of a stage. Since Stage::setSource() is protected, tests should
     * use this method to set the source of any stage under test. Accepts any pointer-like type
     * (raw pointer, boost::intrusive_ptr, etc.) for the stage argument.
     */
    template <typename T>
    static void setSource_forTest(const T& stage, Stage* source) {
        stage->setSource(source);
    }

    size_t size() const;

    /**
     * Adds the given document to the internal queue of this stage.
     *
     * 'count' specifies the number of times the given document should be replicated in the output
     * of this stage.
     */
    void emplace_back(Document doc, int32_t count = 1) {
        if (doc.metadata().isChangeStreamControlEvent()) {
            _queue.push_back(
                QueueItem{GetNextResult::makeAdvancedControlDocument(std::move(doc)), count});
        } else {
            _queue.push_back(QueueItem{GetNextResult(std::move(doc)), count});
        }
    }

    /**
     * Adds the given GetNextResult to the internal queue of this stage.
     *
     * 'count' specifies the number of times the given GetNextResult should be replicated in the
     * output of this stage.
     */
    void push_back(GetNextResult&& result, int32_t count = 1) {
        _queue.push_back(QueueItem{std::move(result), count});
    }

    bool isEOF() const final {
        return _queue.empty();
    }

    void reattachToOperationContext(OperationContext* opCtx) override {
        isDetachedFromOpCtx = false;
    }

    void detachFromOperationContext() override {
        isDetachedFromOpCtx = true;
    }

    void doDispose() override {
        isDisposed = true;
    }

    bool isDisposed{false};
    bool isDetachedFromOpCtx{false};

protected:
    GetNextResult doGetNext() override;

private:
    struct QueueItem {
        GetNextResult next;
        int32_t count{1};
    };

    // Return documents from front of queue.
    std::deque<QueueItem> _queue;
};

}  // namespace mongo::exec::agg
