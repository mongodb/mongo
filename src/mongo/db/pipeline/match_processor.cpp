// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/match_processor.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/document_path_support.h"

#include <string_view>

namespace mongo {
namespace {
// Upper bound on the document size (in bytes) for which the 'match against the whole trivially
// convertible document' fast path (see MatchProcessor::process) is used. Bounds the cost of the
// matcher's linear field scan over fields the predicate does not need.
constexpr int kWholeDocumentMatchMaxSizeBytes = 16 * 1024;
}  // namespace

MatchProcessor::MatchProcessor(std::unique_ptr<MatchExpression> expr,
                               DepsTracker dependencies,
                               BSONObj&& predicate)
    : _expression(std::move(expr)),
      _dependencies(std::move(dependencies)),
      _dependenciesHaveUniqueFirstFields(_dependencies.fields.size() < 2 ||
                                         dependenciesHaveUniqueFirstFields(_dependencies.fields)),
      _predicate(std::move(predicate)) {
    tassert(10422701, "expecting 'predicate' to be owned", _predicate.isOwned());
}

bool MatchProcessor::process(const Document& input, const EvaluationContext& ctx) const {
    // MatchExpression only takes BSON, so serialize the fields it needs. The result may be a view
    // into '_buffer', so it must not outlive this call.
    BSONMatchableDocument toMatch(
        [&]() -> BSONObj {
            // If the input is already bson and we either need the whole document or the document is
            // rather small, use the input directly.
            if (auto whole = input.toBsonIfTriviallyConvertible(); whole &&
                (_dependencies.needWholeDocument ||
                 whole->objsize() <= kWholeDocumentMatchMaxSizeBytes)) {
                return std::move(*whole);
            }
            if (!_buffer.has_value()) {
                _buffer.emplace();
            }
            _buffer->resetToEmpty();
            if (_dependencies.needWholeDocument) {
                input.toBson(&*_buffer);
            } else if (_dependenciesHaveUniqueFirstFields) {
                // Skips the first-field uniqueness check.
                document_path_support::documentToBsonWithPaths<
                    /* PathsHaveUniqueFirstFields */ true>(input, _dependencies.fields, &*_buffer);
            } else {
                document_path_support::documentToBsonWithPaths<
                    /* PathsHaveUniqueFirstFields */ false>(input, _dependencies.fields, &*_buffer);
            }
            // asTempObj() does no size validation, so a mid-pipeline document may exceed the
            // standard 16MB limit.
            return _buffer->asTempObj();
        }(),
        &input);
    trackBufferMemory(ctx);
    return exec::matcher::matches(_expression.get(), &toMatch, /*details*/ nullptr, ctx);
}

bool MatchProcessor::dependenciesHaveUniqueFirstFields(const OrderedPathSet& paths) {
    boost::optional<std::string_view> prevFirstField = boost::none;
    for (auto&& path : paths) {
        auto firstField = FieldPath::extractFirstFieldFromDottedPath(path);
        if (prevFirstField == firstField) {
            return false;
        }
        prevFirstField = firstField;
    }
    return true;
}

int64_t MatchProcessor::bufferCapacity() const {
    return _buffer.has_value() ? _buffer->capacity() : 0;
}

void MatchProcessor::trackBufferMemory(const EvaluationContext& ctx) const {
    if (!ctx.tracker) {
        return;
    }
    if (int64_t capacity = bufferCapacity(); capacity > _trackedBufferBytes) {
        ctx.tracker->add(capacity - _trackedBufferBytes);
        _trackedBufferBytes = capacity;
    }
}

void MatchProcessor::releaseBuffer(SimpleMemoryUsageTracker* tracker) const {
    if (tracker && _trackedBufferBytes > 0) {
        tracker->add(-_trackedBufferBytes);
    }
    _buffer = boost::none;
    _trackedBufferBytes = 0;
}

}  // namespace mongo
