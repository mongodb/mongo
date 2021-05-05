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

namespace mongo {

DocumentSourceEnsureResumeTokenPresent::DocumentSourceEnsureResumeTokenPresent(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, ResumeTokenData token)
    : DocumentSourceCheckResumability(expCtx, std::move(token)) {}

boost::intrusive_ptr<DocumentSourceEnsureResumeTokenPresent>
DocumentSourceEnsureResumeTokenPresent::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, ResumeTokenData token) {
    return new DocumentSourceEnsureResumeTokenPresent(expCtx, std::move(token));
}

const char* DocumentSourceEnsureResumeTokenPresent::getSourceName() const {
    return kStageName.rawData();
}

DocumentSource::GetNextResult DocumentSourceEnsureResumeTokenPresent::doGetNext() {
    // If we have already verified the resume token is present, return the next doc immediately.
    if (_resumeStatus == ResumeStatus::kSurpassedToken) {
        return pSource->getNext();
    }

    auto nextInput = GetNextResult::makeEOF();

    // If we are starting after an 'invalidate' and the invalidating command (e.g. collection drop)
    // occurred at the same clusterTime on more than one shard, then we may see multiple identical
    // resume tokens here. We swallow all of them until the resume status becomes kSurpassedToken.
    while (_resumeStatus != ResumeStatus::kSurpassedToken) {
        // Delegate to DocumentSourceCheckResumability to consume all events up to the token. This
        // will also set '_resumeStatus' to indicate whether we have seen or surpassed the token.
        nextInput = DocumentSourceCheckResumability::doGetNext();

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

Value DocumentSourceEnsureResumeTokenPresent::serialize(
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
