/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_mock.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/expression_context.h"

namespace mongo {

ALLOCATE_DOCUMENT_SOURCE_ID(mock, DocumentSourceMock::id)

boost::intrusive_ptr<DocumentSourceMock> DocumentSourceMock::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return make_intrusive<DocumentSourceMock>(std::deque<GetNextResult>{}, expCtx);
}

DocumentSourceMock::DocumentSourceMock(std::deque<GetNextResult> results,
                                       const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSource(kStageName, expCtx),
      mockConstraints(StreamType::kStreaming,
                      PositionRequirement::kNone,
                      HostTypeRequirement::kNone,
                      DiskUseRequirement::kNoDiskUse,
                      FacetRequirement::kAllowed,
                      TransactionRequirement::kAllowed,
                      LookupRequirement::kAllowed,
                      UnionRequirement::kAllowed) {
    for (auto& res : results) {
        _queue.push_back(QueueItem{std::move(res), /*count*/ 1});
    }
    mockConstraints.requiresInputDocSource = false;
}

const char* DocumentSourceMock::getSourceName() const {
    return kStageName.rawData();
}

size_t DocumentSourceMock::size() const {
    return _queue.size();
}

boost::intrusive_ptr<DocumentSourceMock> DocumentSourceMock::createForTest(
    Document doc, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return new DocumentSourceMock({std::move(doc)}, expCtx);
}

boost::intrusive_ptr<DocumentSourceMock> DocumentSourceMock::createForTest(
    std::deque<GetNextResult> results, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return new DocumentSourceMock(std::move(results), expCtx);
}

boost::intrusive_ptr<DocumentSourceMock> DocumentSourceMock::createForTest(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return new DocumentSourceMock(std::deque<GetNextResult>(), expCtx);
}

boost::intrusive_ptr<DocumentSourceMock> DocumentSourceMock::createForTest(
    const GetNextResult& result, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    std::deque<GetNextResult> results = {result};
    return new DocumentSourceMock(std::move(results), expCtx);
}

boost::intrusive_ptr<DocumentSourceMock> DocumentSourceMock::createForTest(
    const char* json, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return createForTest(Document(fromjson(json)), expCtx);
}

boost::intrusive_ptr<DocumentSourceMock> DocumentSourceMock::createForTest(
    const std::initializer_list<const char*>& jsons,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    std::deque<GetNextResult> results;
    for (auto&& json : jsons) {
        results.emplace_back(Document(fromjson(json)));
    }
    return new DocumentSourceMock(std::move(results), expCtx);
}

DocumentSource::GetNextResult DocumentSourceMock::doGetNext() {
    invariant(!isDisposed);
    invariant(!isDetachedFromOpCtx);

    if (_queue.empty()) {
        return GetNextResult::makeEOF();
    }

    boost::optional<DocumentSource::GetNextResult> next;
    auto& nextQueueItem = _queue.front();
    --nextQueueItem.count;
    if (nextQueueItem.count == 0) {
        next = std::move(nextQueueItem.next);
        _queue.pop_front();
    } else {
        next = nextQueueItem.next;
    }
    return std::move(*next);
}

}  // namespace mongo
