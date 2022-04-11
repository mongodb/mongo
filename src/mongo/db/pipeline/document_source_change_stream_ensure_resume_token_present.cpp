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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_change_stream_ensure_resume_token_present.h"

#include "mongo/db/pipeline/change_stream_start_after_invalidate_info.h"
#include "mongo/db/query/query_feature_flags_gen.h"

namespace mongo {

DocumentSourceChangeStreamEnsureResumeTokenPresent::
    DocumentSourceChangeStreamEnsureResumeTokenPresent(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, ResumeTokenData token)
    : DocumentSourceChangeStreamCheckResumability(expCtx, std::move(token)) {}

boost::intrusive_ptr<DocumentSourceChangeStreamEnsureResumeTokenPresent>
DocumentSourceChangeStreamEnsureResumeTokenPresent::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const DocumentSourceChangeStreamSpec& spec) {
    auto resumeToken = DocumentSourceChangeStream::resolveResumeTokenFromSpec(expCtx, spec);
    tassert(5666902,
            "Expected non-high-water-mark resume token",
            !ResumeToken::isHighWaterMarkToken(resumeToken));
    return new DocumentSourceChangeStreamEnsureResumeTokenPresent(expCtx, std::move(resumeToken));
}

const char* DocumentSourceChangeStreamEnsureResumeTokenPresent::getSourceName() const {
    return kStageName.rawData();
}


StageConstraints DocumentSourceChangeStreamEnsureResumeTokenPresent::constraints(
    Pipeline::SplitState) const {
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

    // The '$match' and 'DocumentSourceSingleDocumentTransformation' stages can swap with this
    // stage, allowing filtering and reshaping to occur earlier in the pipeline. For sharded cluster
    // pipelines, swaps can allow $match and 'DocumentSourceSingleDocumentTransformation' stages to
    // execute on the shards, providing inter-node parallelism and potentially reducing the amount
    // of data sent form each shard to the mongoS.
    constraints.canSwapWithMatch = true;
    constraints.canSwapWithSingleDocTransform = true;

    return constraints;
}

DocumentSource::GetNextResult DocumentSourceChangeStreamEnsureResumeTokenPresent::doGetNext() {
    // If we have already verified the resume token is present, return the next doc immediately.
    if (_resumeStatus == ResumeStatus::kSurpassedToken) {
        return pSource->getNext();
    }

    auto nextInput = GetNextResult::makeEOF();

    // If we are starting after an 'invalidate' and the invalidating command (e.g. collection drop)
    // occurred at the same clusterTime on more than one shard, then we may see multiple identical
    // resume tokens here. We swallow all of them until the resume status becomes kSurpassedToken.
    while (_resumeStatus != ResumeStatus::kSurpassedToken) {
        // Delegate to DocumentSourceChangeStreamCheckResumability to consume all events up to the
        // token. This will also set '_resumeStatus' to indicate whether we have seen or surpassed
        // the token.
        nextInput = _tryGetNext();

        // If there are no more results, return EOF. We will continue checking for the resume token
        // the next time the getNext method is called. If we hit EOF, then we cannot have surpassed
        // the resume token on this iteration.
        if (!nextInput.isAdvanced()) {
            invariant(_resumeStatus != ResumeStatus::kSurpassedToken);
            return nextInput;
        }

        // When we reach here, we have either found the resume token or surpassed it.
        invariant(_resumeStatus != ResumeStatus::kCheckNextDoc);

        // If the resume status is kFoundToken, record the fact that we have seen the token. When we
        // have surpassed the resume token, we will assert that we saw the token before doing so. We
        // cannot simply assert once and then assume we have surpassed the token, because in certain
        // cases we may see 1..N identical tokens and must swallow them all before proceeding.
        _hasSeenResumeToken = (_hasSeenResumeToken || _resumeStatus == ResumeStatus::kFoundToken);
    }

    // Assert that before surpassing the resume token, we observed the token itself in the stream.
    uassert(ErrorCodes::ChangeStreamFatalError,
            str::stream() << "cannot resume stream; the resume token was not found. "
                          << nextInput.getDocument()["_id"].getDocument().toString(),
            _hasSeenResumeToken);

    // At this point, we have seen the token and swallowed it. Return the next event to the client.
    invariant(_hasSeenResumeToken && _resumeStatus == ResumeStatus::kSurpassedToken);
    return nextInput;
}

DocumentSource::GetNextResult DocumentSourceChangeStreamEnsureResumeTokenPresent::_tryGetNext() {
    try {
        return DocumentSourceChangeStreamCheckResumability::doGetNext();
    } catch (const ExceptionFor<ErrorCodes::ChangeStreamStartAfterInvalidate>& ex) {
        const auto extraInfo = ex.extraInfo<ChangeStreamStartAfterInvalidateInfo>();
        tassert(5779200, "Missing ChangeStreamStartAfterInvalidationInfo on exception", extraInfo);

        const DocumentSource::GetNextResult nextInput =
            Document::fromBsonWithMetaData(extraInfo->getStartAfterInvalidateEvent());
        _resumeStatus =
            DocumentSourceChangeStreamCheckResumability::compareAgainstClientResumeToken(
                pExpCtx, nextInput.getDocument(), _tokenFromClient);

        // This exception should always contain the client-provided resume token.
        tassert(5779201,
                "Client resume token did not match with the resume token on the invalidate event",
                _resumeStatus == ResumeStatus::kFoundToken);

        return nextInput;
    }
}

Value DocumentSourceChangeStreamEnsureResumeTokenPresent::serialize(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    // We only serialize this stage in the context of explain.
    if (explain) {
        return Value(DOC(DocumentSourceChangeStream::kStageName
                         << DOC("stage"
                                << "internalEnsureResumeTokenPresent"_sd
                                << "resumeToken" << ResumeToken(_tokenFromClient).toDocument())));
    }
    MONGO_UNREACHABLE_TASSERT(5467611);
}

}  // namespace mongo
