// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_mock.h"

#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/expression_context.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

ALLOCATE_DOCUMENT_SOURCE_ID(mock, DocumentSourceMock::id)

boost::intrusive_ptr<DocumentSourceMock> DocumentSourceMock::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return make_intrusive<DocumentSourceMock>(std::deque<GetNextResult>{}, expCtx);
}

DocumentSourceMock::DocumentSourceMock(std::deque<GetNextResult> results,
                                       const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       SortPattern sortPattern)
    : DocumentSource(kStageName, expCtx, std::move(sortPattern)),
      mockConstraints(StreamType::kStreaming,
                      PositionRequirement::kNone,
                      HostTypeRequirement::kNone,
                      DiskUseRequirement::kNoDiskUse,
                      FacetRequirement::kAllowed,
                      TransactionRequirement::kAllowed,
                      LookupRequirement::kAllowed,
                      UnionRequirement::kAllowed),
      _results(std::move(results)) {
    mockConstraints.setConstraintsForNoInputSources();
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
                      UnionRequirement::kAllowed),
      _results(std::move(results)) {
    mockConstraints.setConstraintsForNoInputSources();
}

std::string_view DocumentSourceMock::getSourceName() const {
    return kStageName;
}

boost::intrusive_ptr<DocumentSourceMock> DocumentSourceMock::createForTest(
    Document doc, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    std::deque<GetNextResult> results;
    if (doc.metadata().isChangeStreamControlEvent()) {
        results.push_back(GetNextResult::makeAdvancedControlDocument(std::move(doc)));
    } else {
        results.push_back(std::move(doc));
    }
    return new DocumentSourceMock(std::move(results), expCtx);
}

boost::intrusive_ptr<DocumentSourceMock> DocumentSourceMock::createForTest(
    std::deque<GetNextResult> results, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return new DocumentSourceMock(std::move(results), expCtx);
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

}  // namespace mongo
