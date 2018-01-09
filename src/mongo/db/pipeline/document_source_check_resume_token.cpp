/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_check_resume_token.h"

using boost::intrusive_ptr;
namespace mongo {
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
                                               ResumeToken token) {
    return new DocumentSourceEnsureResumeTokenPresent(expCtx, std::move(token));
}

DocumentSourceEnsureResumeTokenPresent::DocumentSourceEnsureResumeTokenPresent(
    const intrusive_ptr<ExpressionContext>& expCtx, ResumeToken token)
    : DocumentSource(expCtx), _token(std::move(token)), _haveSeenResumeToken(false) {}

DocumentSource::GetNextResult DocumentSourceEnsureResumeTokenPresent::getNext() {
    pExpCtx->checkForInterrupt();

    if (_haveSeenResumeToken) {
        // We've already verified the resume token is present.
        return pSource->getNext();
    }

    bool cannotResume = false;
    const auto tokenDataWeAreSearchingFor = _token.getData();
    ResumeToken tokenFromSource;

    // Keep iterating the stream until we see either the resume token we're looking for,
    // or a change with a higher timestamp than our resume token.
    while (!cannotResume && !_haveSeenResumeToken) {
        auto nextInput = pSource->getNext();

        if (!nextInput.isAdvanced())
            return nextInput;

        auto doc = nextInput.getDocument();

        tokenFromSource = ResumeToken::parse(doc["_id"].getDocument());
        auto tokenDataFromSource = tokenFromSource.getData();

        // We start the resume with a $gte query on the timestamp, so we never expect it to be
        // lower than our resume token's timestamp.
        invariant(tokenDataFromSource.clusterTime >= tokenDataWeAreSearchingFor.clusterTime);

        // The incoming documents are sorted on clusterTime, uuid, documentKey. We examine a range
        // of documents that have the same prefix (i.e. clusterTime and uuid). If the user provided
        // token would sort before this received document we cannot resume the change stream.
        // Use the simple collation to compare the resume tokens.
        // Note this is purposefully avoiding the user's requested collation.
        if (tokenDataWeAreSearchingFor.clusterTime == tokenDataFromSource.clusterTime) {
            if (tokenDataWeAreSearchingFor.uuid != tokenDataFromSource.uuid) {
                cannotResume = true;
            } else if (ValueComparator::kInstance.evaluate(tokenDataWeAreSearchingFor.documentKey ==
                                                           tokenDataFromSource.documentKey)) {
                _haveSeenResumeToken = true;
            } else if (ValueComparator::kInstance.evaluate(tokenDataWeAreSearchingFor.documentKey <
                                                           tokenDataFromSource.documentKey)) {
                // This means we will never see the resume token because it would have come before
                // this one.
                cannotResume = true;
            }
        } else {
            cannotResume = true;
        }
    }

    if (cannotResume) {
        uasserted(40585,
                  str::stream() << "resume of change stream was not possible, as the resume "
                                   "token was not found. "
                                << tokenFromSource.toDocument().toString());
    }

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
    const intrusive_ptr<ExpressionContext>& expCtx, ResumeToken token) {
    return new DocumentSourceShardCheckResumability(expCtx, std::move(token));
}

DocumentSourceShardCheckResumability::DocumentSourceShardCheckResumability(
    const intrusive_ptr<ExpressionContext>& expCtx, ResumeToken token)
    : DocumentSourceNeedsMongoProcessInterface(expCtx),
      _token(std::move(token)),
      _verifiedResumability(false) {}

DocumentSource::GetNextResult DocumentSourceShardCheckResumability::getNext() {
    pExpCtx->checkForInterrupt();

    auto nextInput = pSource->getNext();
    if (_verifiedResumability)
        return nextInput;

    _verifiedResumability = true;
    if (nextInput.isAdvanced()) {
        auto doc = nextInput.getDocument();

        auto receivedToken = ResumeToken::parse(doc["_id"].getDocument());
        if (receivedToken == _token) {
            // Pass along the document, as the DocumentSourceEnsureResumeTokenPresent stage on the
            // merger will
            // need to see it.
            return nextInput;
        }
    }
    // If we make it here, we need to look up the first document in the oplog and compare it
    // with the resume token.
    auto firstEntryExpCtx = pExpCtx->copyWith(NamespaceString::kRsOplogNamespace);
    auto matchSpec = BSON("$match" << BSONObj());
    auto pipeline =
        uassertStatusOK(_mongoProcessInterface->makePipeline({matchSpec}, firstEntryExpCtx));
    if (auto first = pipeline->getNext()) {
        auto firstOplogEntry = Value(*first);
        uassert(40576,
                "resume of change notification was not possible, as the resume point may no longer "
                "be in the oplog. ",
                firstOplogEntry["ts"].getTimestamp() < _token.getData().clusterTime);
        return nextInput;
    }
    // Very unusual case: the oplog is empty.  We can always resume.  It should never be possible
    // that the oplog is empty and we got a document matching the filter, however.
    invariant(nextInput.isEOF());
    return nextInput;
}
}  // namespace mongo
