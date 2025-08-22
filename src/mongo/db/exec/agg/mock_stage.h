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

#include "mongo/base/string_data.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/intrusive_counter.h"

#include <cstddef>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {

class MockStage : public Stage {
public:
    MockStage(StringData stageName,
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
