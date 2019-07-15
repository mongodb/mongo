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

#include "mongo/platform/basic.h"

#include "mongo/db/curop.h"
#include "mongo/db/pipeline/document_source_check_resume_token.h"

using boost::intrusive_ptr;
namespace mongo {
namespace {

using ResumeStatus = DocumentSourceEnsureResumeTokenPresent::ResumeStatus;

// Returns ResumeStatus::kFoundToken if the document retrieved from the resumed pipeline satisfies
// the client's resume token, ResumeStatus::kCheckNextDoc if it is older than the client's token,
// and ResumeToken::kCannotResume if it is more recent than the client's resume token (indicating
// that we will never see the token). If the resume token's documentKey contains only the _id field
// while the pipeline documentKey contains additional fields, then the collection has become sharded
// since the resume token was generated. In that case, we relax the requirements such that only the
// timestamp, version, txnOpIndex, UUID and documentKey._id need match. This remains correct, since
// the only circumstances under which the resume token omits the shard key is if it was generated
// either (1) before the collection was sharded, (2) after the collection was sharded but before the
// primary shard became aware of that fact, implying that it was before the first chunk moved off
// the shard, or (3) by a malicious client who has constructed their own resume token. In the first
// two cases, we can be guaranteed that the _id is unique and the stream can therefore be resumed
// seamlessly; in the third case, the worst that can happen is that some entries are missed or
// duplicated. Note that the simple collation is used to compare the resume tokens, and that we
// purposefully avoid the user's requested collation if present.
ResumeStatus compareAgainstClientResumeToken(const intrusive_ptr<ExpressionContext>& expCtx,
                                             const Document& documentFromResumedStream,
                                             const ResumeTokenData& tokenDataFromClient) {
    // Parse the stream doc into comprehensible ResumeTokenData.
    auto tokenDataFromResumedStream =
        ResumeToken::parse(documentFromResumedStream["_id"].getDocument()).getData();

    // We start the resume with a $gte query on the timestamp, so we never expect it to be lower
    // than our resume token's timestamp.
    invariant(tokenDataFromResumedStream.clusterTime >= tokenDataFromClient.clusterTime);

    // If the clusterTime differs from the client's token, this stream cannot be resumed.
    if (tokenDataFromResumedStream.clusterTime != tokenDataFromClient.clusterTime) {
        return ResumeStatus::kSurpassedToken;
    }

    // If the tokenType exceeds the client token's type, then we have passed the resume token point.
    // This can happen if the client resumes from a synthetic 'high water mark' token from another
    // shard which happens to have the same clusterTime as an actual change on this shard.
    if (tokenDataFromResumedStream.tokenType != tokenDataFromClient.tokenType) {
        return tokenDataFromResumedStream.tokenType > tokenDataFromClient.tokenType
            ? ResumeStatus::kSurpassedToken
            : ResumeStatus::kCheckNextDoc;
    }

    // If the document's 'txnIndex' sorts before that of the client token, we must keep looking.
    if (tokenDataFromResumedStream.txnOpIndex < tokenDataFromClient.txnOpIndex) {
        return ResumeStatus::kCheckNextDoc;
    } else if (tokenDataFromResumedStream.txnOpIndex > tokenDataFromClient.txnOpIndex) {
        // This could happen if the client provided a txnOpIndex of 0, yet the 0th document in the
        // applyOps was irrelevant (meaning it was an operation on a collection or DB not being
        // watched). If we are looking for the resume token on a shard then this simply means that
        // the resume token may be on a different shard; otherwise, it indicates a corrupt token.
        uassert(50792, "Invalid resumeToken: txnOpIndex was skipped", expCtx->needsMerge);
        // We are running on a merging shard. Signal that we have read beyond the resume token.
        return ResumeStatus::kSurpassedToken;
    }

    // If 'fromInvalidate' exceeds the client's token value, then we have passed the resume point.
    if (tokenDataFromResumedStream.fromInvalidate != tokenDataFromClient.fromInvalidate) {
        return tokenDataFromResumedStream.fromInvalidate ? ResumeStatus::kSurpassedToken
                                                         : ResumeStatus::kCheckNextDoc;
    }

    // It is acceptable for the stream UUID to differ from the client's, if this is a whole-database
    // or cluster-wide stream and we are comparing operations from different shards at the same
    // clusterTime. If the stream UUID sorts after the client's, however, then the stream is not
    // resumable; we are past the point in the stream where the token should have appeared.
    if (tokenDataFromResumedStream.uuid != tokenDataFromClient.uuid) {
        // If we are running on a replica set deployment, we don't ever expect to see identical time
        // stamps and txnOpIndex but differing UUIDs, and we reject the resume attempt at once.
        if (!expCtx->inMongos && !expCtx->needsMerge) {
            return ResumeStatus::kSurpassedToken;
        }
        // Otherwise, return a ResumeStatus based on the sort-order of the client and stream UUIDs.
        return tokenDataFromResumedStream.uuid > tokenDataFromClient.uuid
            ? ResumeStatus::kSurpassedToken
            : ResumeStatus::kCheckNextDoc;
    }

    // If all the fields match exactly, then we have found the token.
    if (ValueComparator::kInstance.evaluate(tokenDataFromResumedStream.documentKey ==
                                            tokenDataFromClient.documentKey)) {
        return ResumeStatus::kFoundToken;
    }

    // At this point, we know that the tokens differ only by documentKey. The status we return will
    // depend on whether the stream token is logically before or after the client token. If the
    // latter, then we will never see the resume token and the stream cannot be resumed. However,
    // before we can return this value, we need to check the possibility that the resumed stream is
    // on a sharded collection and the client token is from before the collection was sharded.
    const auto defaultResumeStatus =
        ValueComparator::kInstance.evaluate(tokenDataFromResumedStream.documentKey >
                                            tokenDataFromClient.documentKey)
        ? ResumeStatus::kSurpassedToken
        : ResumeStatus::kCheckNextDoc;

    // If we're not running in a sharded context, we don't need to proceed any further.
    if (!expCtx->needsMerge && !expCtx->inMongos) {
        return defaultResumeStatus;
    }

    // If we reach here, we still need to check the possibility that the collection has become
    // sharded in the time since the client's resume token was generated. If so, then the client
    // token will only have an _id field, while the token from the new pipeline may have additional
    // shard key fields.

    // We expect the documentKey to be an object in both the client and stream tokens. If either is
    // not, then we cannot compare the embedded _id values in each, and so the stream token does not
    // satisfy the client token.
    if (tokenDataFromClient.documentKey.getType() != BSONType::Object ||
        tokenDataFromResumedStream.documentKey.getType() != BSONType::Object) {
        return defaultResumeStatus;
    }

    auto documentKeyFromResumedStream = tokenDataFromResumedStream.documentKey.getDocument();
    auto documentKeyFromClient = tokenDataFromClient.documentKey.getDocument();

    // In order for the relaxed comparison to be applicable, the client token must have a single _id
    // field, and the resumed stream token must have additional fields beyond _id.
    if (!(documentKeyFromClient.size() == 1 && documentKeyFromResumedStream.size() > 1)) {
        return defaultResumeStatus;
    }

    // If the resume token's documentKey only contains the _id field while the pipeline's
    // documentKey contains additional fields, we require only that the _ids match.
    return (!documentKeyFromClient["_id"].missing() &&
                    ValueComparator::kInstance.evaluate(documentKeyFromResumedStream["_id"] ==
                                                        documentKeyFromClient["_id"])
                ? ResumeStatus::kFoundToken
                : defaultResumeStatus);
}
}  // namespace

const char* DocumentSourceEnsureResumeTokenPresent::getSourceName() const {
    return "$_ensureResumeTokenPresent";
}

Value DocumentSourceEnsureResumeTokenPresent::serialize(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    // This stage is created by the DocumentSourceChangeStream stage, so serializing it here
    // would result in it being created twice.
    return Value();
}

intrusive_ptr<DocumentSourceEnsureResumeTokenPresent>
DocumentSourceEnsureResumeTokenPresent::create(const intrusive_ptr<ExpressionContext>& expCtx,
                                               ResumeTokenData token) {
    return new DocumentSourceEnsureResumeTokenPresent(expCtx, std::move(token));
}

DocumentSourceEnsureResumeTokenPresent::DocumentSourceEnsureResumeTokenPresent(
    const intrusive_ptr<ExpressionContext>& expCtx, ResumeTokenData token)
    : DocumentSource(expCtx), _tokenFromClient(std::move(token)) {}

DocumentSource::GetNextResult DocumentSourceEnsureResumeTokenPresent::getNext() {
    pExpCtx->checkForInterrupt();

    if (_resumeStatus == ResumeStatus::kSurpassedToken) {
        // We've already verified the resume token is present.
        return pSource->getNext();
    }

    // The incoming documents are sorted by resume token. We examine a range of documents that have
    // the same clusterTime as the client's resume token, until we either find (and swallow) a match
    // for the token or pass the point in the stream where it should have been.
    while (_resumeStatus != ResumeStatus::kSurpassedToken) {
        auto nextInput = pSource->getNext();

        // If there are no more results, return EOF. We will continue checking for the client's
        // resume token the next time the getNext method is called.
        if (!nextInput.isAdvanced()) {
            return nextInput;
        }
        // Check the current event. If we found and swallowed the resume token, then the result will
        // be the first event in the stream which should be returned to the user. Otherwise, we keep
        // iterating the stream until we find an event matching the client's resume token.
        if (auto nextOutput = _checkNextDocAndSwallowResumeToken(nextInput)) {
            return *nextOutput;
        }
    }
    MONGO_UNREACHABLE;
}

boost::optional<DocumentSource::GetNextResult>
DocumentSourceEnsureResumeTokenPresent::_checkNextDocAndSwallowResumeToken(
    const DocumentSource::GetNextResult& nextInput) {
    // We should only ever call this method when we have a new event to examine.
    invariant(nextInput.isAdvanced());
    auto resumeStatus =
        compareAgainstClientResumeToken(pExpCtx, nextInput.getDocument(), _tokenFromClient);
    switch (resumeStatus) {
        case ResumeStatus::kCheckNextDoc:
            return boost::none;
        case ResumeStatus::kFoundToken:
            // We found the resume token. If we are starting after an 'invalidate' token and the
            // invalidating command (e.g. collection drop) occurred at the same clusterTime on
            // more than one shard, then we will see multiple identical 'invalidate' events
            // here. We should continue to swallow all of them to ensure that the new stream
            // begins after the collection drop, and that it is not immediately re-invalidated.
            if (pExpCtx->inMongos && _tokenFromClient.fromInvalidate) {
                _resumeStatus = ResumeStatus::kFoundToken;
                return boost::none;
            }
            // If the token is not an invalidate or if we are not running in a cluster, we mark
            // the stream as having surpassed the resume token, skip the current event since the
            // client has already seen it, and return the next event in the stream.
            _resumeStatus = ResumeStatus::kSurpassedToken;
            return pSource->getNext();
        case ResumeStatus::kSurpassedToken:
            // If we have surpassed the point in the stream where the resume token should have
            // been and we did not see the token itself, then this stream cannot be resumed.
            uassert(40585,
                    str::stream() << "cannot resume stream; the resume token was not found. "
                                  << nextInput.getDocument()["_id"].getDocument().toString(),
                    _resumeStatus == ResumeStatus::kFoundToken);
            _resumeStatus = ResumeStatus::kSurpassedToken;
            return nextInput;
    }
    MONGO_UNREACHABLE;
}

const char* DocumentSourceShardCheckResumability::getSourceName() const {
    return "$_checkShardResumability";
}

Value DocumentSourceShardCheckResumability::serialize(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    // This stage is created by the DocumentSourceChangeStream stage, so serializing it here
    // would result in it being created twice.
    return Value();
}

intrusive_ptr<DocumentSourceShardCheckResumability> DocumentSourceShardCheckResumability::create(
    const intrusive_ptr<ExpressionContext>& expCtx, Timestamp ts) {
    // We are resuming from a point in time, not an event. Seed the stage with a high water mark.
    return create(expCtx, ResumeToken::makeHighWaterMarkToken(ts).getData());
}

intrusive_ptr<DocumentSourceShardCheckResumability> DocumentSourceShardCheckResumability::create(
    const intrusive_ptr<ExpressionContext>& expCtx, ResumeTokenData token) {
    return new DocumentSourceShardCheckResumability(expCtx, std::move(token));
}

DocumentSourceShardCheckResumability::DocumentSourceShardCheckResumability(
    const intrusive_ptr<ExpressionContext>& expCtx, ResumeTokenData token)
    : DocumentSource(expCtx), _tokenFromClient(std::move(token)) {}

DocumentSource::GetNextResult DocumentSourceShardCheckResumability::getNext() {
    pExpCtx->checkForInterrupt();

    if (_surpassedResumeToken)
        return pSource->getNext();

    while (!_surpassedResumeToken) {
        auto nextInput = pSource->getNext();

        // If we hit EOF, check the oplog to make sure that we are able to resume. This prevents us
        // from continually returning EOF in cases where the resume point has fallen off the oplog.
        if (!nextInput.isAdvanced()) {
            _assertOplogHasEnoughHistory(nextInput);
            return nextInput;
        }
        // Determine whether the current event sorts before, equal to or after the resume token.
        auto resumeStatus =
            compareAgainstClientResumeToken(pExpCtx, nextInput.getDocument(), _tokenFromClient);
        switch (resumeStatus) {
            case ResumeStatus::kCheckNextDoc:
                // If the result was kCheckNextDoc, we are resumable but must swallow this event.
                _verifiedOplogHasEnoughHistory = true;
                continue;
            case ResumeStatus::kSurpassedToken:
                // In this case the resume token wasn't found; it must be on another shard. We must
                // examine the oplog to ensure that its history reaches back to before the resume
                // token, otherwise we may have missed events that fell off the oplog. If we can
                // resume, fall through into the following case and set _surpassedResumeToken.
                _assertOplogHasEnoughHistory(nextInput);
            case ResumeStatus::kFoundToken:
                // We found the actual token! Set _surpassedResumeToken and return the result.
                _surpassedResumeToken = true;
                return nextInput;
        }
    }
    MONGO_UNREACHABLE;
}

void DocumentSourceShardCheckResumability::_assertOplogHasEnoughHistory(
    const GetNextResult& nextInput) {
    // If we have already verified that this stream is resumable, return immediately.
    if (_verifiedOplogHasEnoughHistory) {
        return;
    }
    // Look up the first document in the oplog and compare it with the resume token's clusterTime.
    auto firstEntryExpCtx = pExpCtx->copyWith(NamespaceString::kRsOplogNamespace);
    auto matchSpec = BSON("$match" << BSONObj());
    auto pipeline = pExpCtx->mongoProcessInterface->makePipeline({matchSpec}, firstEntryExpCtx);
    if (auto first = pipeline->getNext()) {
        auto firstOplogEntry = Value(*first);
        // If the first entry in the oplog is the replset initialization, then it doesn't matter
        // if its timestamp is later than the resume token. No events earlier than the token can
        // have fallen off this oplog, and it is therefore safe to resume. Otherwise, verify that
        // the timestamp of the first oplog entry is earlier than that of the resume token.
        const bool isNewRS =
            Value::compare(firstOplogEntry["o"]["msg"], Value("initiating set"_sd), nullptr) == 0 &&
            Value::compare(firstOplogEntry["op"], Value("n"_sd), nullptr) == 0;
        uassert(40576,
                "Resume of change stream was not possible, as the resume point may no longer be in "
                "the oplog. ",
                isNewRS || firstOplogEntry["ts"].getTimestamp() < _tokenFromClient.clusterTime);
    } else {
        // Very unusual case: the oplog is empty.  We can always resume. However, it should never be
        // possible to have obtained a document that matched the filter if the oplog is empty.
        uassert(51087,
                "Oplog was empty but found an event in the change stream pipeline. It should not "
                "be possible for this to happen",
                nextInput.isEOF());
    }
    _verifiedOplogHasEnoughHistory = true;
}
}  // namespace mongo
