// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_queue.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <string_view>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

REGISTER_INTERNAL_LITE_PARSED_DOCUMENT_SOURCE(queue, QueueLiteParsed::parse);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(queue, DocumentSourceQueue, QueueStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(queue, DocumentSourceQueue::id);

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
    boost::optional<std::string_view> stageNameOverride) {
    return new DocumentSourceQueue(std::deque<GetNextResult>{}, expCtx, stageNameOverride);
}

DocumentSourceQueue::DocumentSourceQueue(DocumentSourceQueue::DeferredQueue results,
                                         const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         boost::optional<std::string_view> stageNameOverride,
                                         boost::optional<Value> serializeOverride,
                                         boost::optional<StageConstraints> constraintsOverride)
    : DocumentSource(kStageName /* pass the real stage name here for execution stats */, expCtx),
      _queue(std::move(results)),
      _stageNameOverride(std::move(stageNameOverride)),
      _serializeOverride(std::move(serializeOverride)),
      _constraintsOverride(std::move(constraintsOverride)) {}

std::string_view DocumentSourceQueue::getSourceName() const {
    return _stageNameOverride.value_or(kStageName);
}

Value DocumentSourceQueue::serialize(const query_shape::SerializationOptions& opts) const {
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
