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
// while the pipeline documentKey contains additional fields, then the collection has become
// sharded since the resume token was generated. In that case, we relax the requirements such that
// only the timestamp, version, applyOpsIndex, UUID and documentKey._id need match. This remains
// correct, since the only circumstances under which the resume token omits the shard key is if it
// was generated either (1) before the collection was sharded, (2) after the collection was sharded
// but before the primary shard became aware of that fact, implying that it was before the first
// chunk moved off the shard, or (3) by a malicious client who has constructed their own resume
// token. In the first two cases, we can be guaranteed that the _id is unique and the stream can
// therefore be resumed seamlessly; in the third case, the worst that can happen is that some
// entries are missed or duplicated. Note that the simple collation is used to compare the resume
// tokens, and that we purposefully avoid the user's requested collation if present.
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
    if (tokenDataFromResumedStream.tokenType > tokenDataFromClient.tokenType) {
        return ResumeStatus::kSurpassedToken;
    }

    if (tokenDataFromResumedStream.applyOpsIndex < tokenDataFromClient.applyOpsIndex) {
        return ResumeStatus::kCheckNextDoc;
    } else if (tokenDataFromResumedStream.applyOpsIndex > tokenDataFromClient.applyOpsIndex) {
        // This could happen if the client provided an applyOpsIndex of 0, yet the 0th document in
        // the applyOps was irrelevant (meaning it was an operation on a collection or DB not being
        // watched). This indicates a corrupt resume token.
        uasserted(50792, "Invalid resumeToken: applyOpsIndex was skipped");
    }

    // It is acceptable for the stream UUID to differ from the client's, if this is a whole-database
    // or cluster-wide stream and we are comparing operations from different shards at the same
    // clusterTime. If the stream UUID sorts after the client's, however, then the stream is not
    // resumable; we are past the point in the stream where the token should have appeared.
    if (tokenDataFromResumedStream.uuid != tokenDataFromClient.uuid) {
        // If we're not in mongos then this must be a replica set deployment, in which case we don't
        // ever expect to see identical timestamps and we reject the resume attempt immediately.
        return !expCtx->inMongos || tokenDataFromResumedStream.uuid > tokenDataFromClient.uuid
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

    if (_resumeStatus == ResumeStatus::kFoundToken) {
        // We've already verified the resume token is present.
        return pSource->getNext();
    }

    Document documentFromResumedStream;

    // Keep iterating the stream until we see either the resume token we're looking for,
    // or a change with a higher timestamp than our resume token.
    while (_resumeStatus == ResumeStatus::kCheckNextDoc) {
        auto nextInput = pSource->getNext();

        if (!nextInput.isAdvanced())
            return nextInput;

        // The incoming documents are sorted on clusterTime, uuid, documentKey. We examine a range
        // of documents that have the same prefix (i.e. clusterTime and uuid). If the user provided
        // token would sort before this received document we cannot resume the change stream.
        _resumeStatus = compareAgainstClientResumeToken(
            pExpCtx, (documentFromResumedStream = nextInput.getDocument()), _tokenFromClient);
    }

    uassert(40585,
            str::stream()
                << "resume of change stream was not possible, as the resume token was not found. "
                << documentFromResumedStream["_id"].getDocument().toString(),
            _resumeStatus != ResumeStatus::kSurpassedToken);

    // If we reach this point, then we've seen the resume token.
    invariant(_resumeStatus == ResumeStatus::kFoundToken);

    // Don't return the document which has the token; the user has already seen it.
    return pSource->getNext();
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
    return create(expCtx, ResumeToken::makeHighWaterMarkToken(ts, expCtx->uuid).getData());
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
        uassert(40576,
                "Resume of change stream was not possible, as the resume point may no longer "
                "be in the oplog. ",
                firstOplogEntry["ts"].getTimestamp() < _tokenFromClient.clusterTime);
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
