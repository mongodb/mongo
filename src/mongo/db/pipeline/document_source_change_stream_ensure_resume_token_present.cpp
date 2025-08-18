/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_change_stream_ensure_resume_token_present.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/change_stream_helpers.h"
#include "mongo/db/pipeline/change_stream_start_after_invalidate_info.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/util/assert_util.h"

#include <utility>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

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

const char* DocumentSourceChangeStreamEnsureResumeTokenPresent::getSourceName() const {
    return kStageName.data();
}

StageConstraints DocumentSourceChangeStreamEnsureResumeTokenPresent::constraints(
    PipelineSplitState) const {
    StageConstraints constraints{StreamType::kStreaming,
                                 PositionRequirement::kNone,
                                 // If this is parsed on mongos it should stay on mongos. If we're
                                 // not in a sharded cluster then it's okay to run on mongod.
                                 HostTypeRequirement::kLocalOnly,
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

    return constraints;
}

Value DocumentSourceChangeStreamEnsureResumeTokenPresent::doSerialize(
    const SerializationOptions& opts) const {
    BSONObjBuilder builder;
    if (opts.isSerializingForExplain()) {
        BSONObjBuilder sub(builder.subobjStart(DocumentSourceChangeStream::kStageName));
        sub.append("stage"_sd, kStageName);
        sub << "resumeToken"_sd << Value(ResumeToken(_tokenFromClient).toDocument(opts));
        sub.done();
    } else {
        BSONObjBuilder sub(builder.subobjStart(kStageName));
        sub << "resumeToken"_sd << Value(ResumeToken(_tokenFromClient).toDocument(opts));
        sub.done();
    }
    return Value(builder.obj());
}

}  // namespace mongo
