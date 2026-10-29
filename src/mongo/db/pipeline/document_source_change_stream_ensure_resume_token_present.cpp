// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_change_stream_ensure_resume_token_present.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/change_stream_helpers.h"
#include "mongo/db/pipeline/change_stream_start_after_invalidate_info.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/util/assert_util.h"

#include <string_view>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

ALLOCATE_DOCUMENT_SOURCE_ID(_internalChangeStreamEnsureResumeTokenPresent,
                            DocumentSourceChangeStreamEnsureResumeTokenPresent::id)

DocumentSourceChangeStreamEnsureResumeTokenPresent::
    DocumentSourceChangeStreamEnsureResumeTokenPresent(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, ResumeTokenData token)
    : DocumentSourceChangeStreamCheckResumability(expCtx, std::move(token)) {}

boost::intrusive_ptr<DocumentSourceChangeStreamEnsureResumeTokenPresent>
DocumentSourceChangeStreamEnsureResumeTokenPresent::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const DocumentSourceChangeStreamSpec& spec) {
    auto resumeToken = change_stream::resolveResumeTokenFromSpec(expCtx, spec);
    tassert(5666902,
            "Expected non-high-water-mark resume token",
            !ResumeToken::isHighWaterMarkToken(resumeToken));
    return new DocumentSourceChangeStreamEnsureResumeTokenPresent(expCtx, std::move(resumeToken));
}

std::string_view DocumentSourceChangeStreamEnsureResumeTokenPresent::getSourceName() const {
    return kStageName;
}

StageConstraints DocumentSourceChangeStreamEnsureResumeTokenPresent::constraints(
    PipelineSplitState) const {
    StageConstraints constraints{StreamType::kStreaming,
                                 PositionRequirement::kNone,
                                 // If this is parsed on mongos it should stay on mongos. If we're
                                 // not in a sharded cluster then it's okay to run on mongod.
                                 HostTypeRequirement::kReceivingHostOnly,
                                 DiskUseRequirement::kNoDiskUse,
                                 FacetRequirement::kNotAllowed,
                                 TransactionRequirement::kNotAllowed,
                                 LookupRequirement::kNotAllowed,
                                 UnionRequirement::kNotAllowed,
                                 ChangeStreamRequirement::kChangeStreamStage};

    // The '$match', '$redact', and 'DocumentSourceSingleDocumentTransformation' stages can swap
    // with this stage, allowing filtering and reshaping to occur earlier in the pipeline. For
    // sharded cluster pipelines, swaps can allow $match, $redact and
    // 'DocumentSourceSingleDocumentTransformation' stages to execute on the shards, providing
    // inter-node parallelism and potentially reducing the amount of data sent form each shard to
    // the mongoS.
    constraints.canSwapWithMatch = true;
    constraints.canSwapWithSingleDocTransformOrRedact = true;
    constraints.consumesLogicalCollectionData = false;
    constraints.outputDependsOnSingleInput = true;

    return constraints;
}

Value DocumentSourceChangeStreamEnsureResumeTokenPresent::doSerialize(
    const query_shape::SerializationOptions& opts) const {
    BSONObjBuilder builder;
    if (opts.isSerializingForExplain()) {
        BSONObjBuilder sub(builder.subobjStart(DocumentSourceChangeStream::kStageName));
        sub.append("stage"sv, kStageName);
        sub << "resumeToken"sv << Value(ResumeToken(_tokenFromClient).toDocument(opts));
        sub.done();
    } else {
        BSONObjBuilder sub(builder.subobjStart(kStageName));
        sub << "resumeToken"sv << Value(ResumeToken(_tokenFromClient).toDocument(opts));
        sub.done();
    }
    return Value(builder.obj());
}

}  // namespace mongo
