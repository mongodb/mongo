/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_queue.h"
namespace mongo {

REGISTER_INTERNAL_DOCUMENT_SOURCE(queue,
                                  LiteParsedDocumentSourceDefault::parse,
                                  DocumentSourceQueue::createFromBson,
                                  true);

boost::intrusive_ptr<DocumentSource> DocumentSourceQueue::createFromBson(
    BSONElement arrayElem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return DocumentSourceQueue::create(expCtx, parseFromArray(arrayElem));
}

std::deque<DocumentSource::GetNextResult> DocumentSourceQueue::parseFromArray(
    BSONElement arrayElem) {
    uassert(5858201,
            "literal documents specification must be an array",
            arrayElem.type() == BSONType::Array);
    std::deque<GetNextResult> queue;
    // arrayElem is an Array and can be iterated through by using .Obj() method
    for (auto elem : arrayElem.Obj()) {
        uassert(5858202,
                "literal documents specification must be an array of objects",
                elem.type() == BSONType::Object);
        queue.emplace_back(Document{elem.Obj()}.getOwned());
    }
    return queue;
}

boost::intrusive_ptr<DocumentSourceQueue> DocumentSourceQueue::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::optional<StringData> aliasStageName) {
    return DocumentSourceQueue::create(expCtx, {}, aliasStageName);
}

boost::intrusive_ptr<DocumentSourceQueue> DocumentSourceQueue::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::deque<GetNextResult> documents,
    boost::optional<StringData> aliasStageName) {
    return make_intrusive<DocumentSourceQueue>(std::move(documents), expCtx, aliasStageName);
}

DocumentSourceQueue::DocumentSourceQueue(std::deque<GetNextResult> results,
                                         const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         boost::optional<StringData> aliasStageName)
    : DocumentSource(kStageName /* pass the real stage name here for execution stats */, expCtx),
      _queue(std::move(results)),
      _aliasStageName(std::move(aliasStageName)) {}

const char* DocumentSourceQueue::getSourceName() const {
    return _aliasStageName.value_or(kStageName).rawData();
}

DocumentSource::GetNextResult DocumentSourceQueue::doGetNext() {
    if (_queue.empty()) {
        return GetNextResult::makeEOF();
    }

    auto next = std::move(_queue.front());
    _queue.pop_front();
    return next;
}

Value DocumentSourceQueue::serialize(boost::optional<ExplainOptions::Verbosity> explain) const {
    return serializeWithName(explain, kStageName);
}

Value DocumentSourceQueue::serializeWithName(boost::optional<ExplainOptions::Verbosity> explain,
                                             StringData stageName) const {
    ValueArrayStream vals;
    for (auto elem : _queue) {
        vals << elem.getDocument().getOwned();
    }
    return Value(DOC(stageName << vals.done()));
}

}  // namespace mongo
