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

#include "mongo/db/pipeline/document_source_queue.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

REGISTER_INTERNAL_DOCUMENT_SOURCE(queue,
                                  LiteParsedDocumentSourceDefault::parse,
                                  DocumentSourceQueue::createFromBson,
                                  true);
ALLOCATE_DOCUMENT_SOURCE_ID(queue, DocumentSourceQueue::id)

boost::intrusive_ptr<DocumentSource> DocumentSourceQueue::createFromBson(
    BSONElement arrayElem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(5858201,
            "literal documents specification must be an array",
            arrayElem.type() == BSONType::array);
    auto queue = DocumentSourceQueue::create(expCtx);
    // arrayElem is an Array and can be iterated through by using .Obj() method
    for (const auto& elem : arrayElem.Obj()) {
        uassert(5858202,
                "literal documents specification must be an array of objects",
                elem.type() == BSONType::object);
        queue->emplace_back(Document{elem.Obj()}.getOwned());
    }
    return queue;
}

boost::intrusive_ptr<DocumentSourceQueue> DocumentSourceQueue::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::optional<StringData> stageNameOverride) {
    return new DocumentSourceQueue(std::deque<GetNextResult>{}, expCtx, stageNameOverride);
}

DocumentSourceQueue::DocumentSourceQueue(DocumentSourceQueue::DeferredQueue results,
                                         const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         boost::optional<StringData> stageNameOverride,
                                         boost::optional<Value> serializeOverride,
                                         boost::optional<StageConstraints> constraintsOverride)
    : DocumentSource(kStageName /* pass the real stage name here for execution stats */, expCtx),
      _queue(std::move(results)),
      _stageNameOverride(std::move(stageNameOverride)),
      _serializeOverride(std::move(serializeOverride)),
      _constraintsOverride(std::move(constraintsOverride)) {}

const char* DocumentSourceQueue::getSourceName() const {
    return _stageNameOverride.value_or(kStageName).data();
}

Value DocumentSourceQueue::serialize(const SerializationOptions& opts) const {
    // Early exit with the pre-defined serialization override if it exists.
    if (_serializeOverride.has_value()) {
        return *_serializeOverride;
    }

    // Initialize the deferred queue if needed, and serialize its documents as one literal in the
    // context of redaction.
    ValueArrayStream vals;
    for (const auto& elem : _queue.get()) {
        vals << elem.getDocument().getOwned();
    }
    return Value(DOC(kStageName << opts.serializeLiteral(vals.done())));
}

}  // namespace mongo
