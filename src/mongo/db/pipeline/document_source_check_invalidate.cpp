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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_check_invalidate.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/util/assert_util.h"

namespace mongo {

using DSCS = DocumentSourceChangeStream;

REGISTER_INTERNAL_DOCUMENT_SOURCE(
    _internalChangeStreamCheckInvalidate,
    LiteParsedDocumentSourceChangeStreamInternal::parse,
    DocumentSourceCheckInvalidate::createFromBson,
    feature_flags::gFeatureFlagChangeStreamsOptimization.isEnabledAndIgnoreFCV());

namespace {

// Returns true if the given 'operationType' should invalidate the change stream based on the
// namespace in 'pExpCtx'.
bool isInvalidatingCommand(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                           StringData operationType) {
    if (pExpCtx->isSingleNamespaceAggregation()) {
        return operationType == DSCS::kDropCollectionOpType ||
            operationType == DSCS::kRenameCollectionOpType ||
            operationType == DSCS::kDropDatabaseOpType;
    } else if (!pExpCtx->isClusterAggregation()) {
        return operationType == DSCS::kDropDatabaseOpType;
    } else {
        return false;
    }
};

}  // namespace

boost::intrusive_ptr<DocumentSourceCheckInvalidate> DocumentSourceCheckInvalidate::createFromBson(
    BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(5467602,
            str::stream() << "the '" << kStageName << "' object spec must be an object",
            spec.type() == Object);

    auto parsed = DocumentSourceChangeStreamCheckInvalidateSpec::parse(
        IDLParserErrorContext("DocumentSourceChangeStreamCheckInvalidateSpec"),
        spec.embeddedObject());
    return new DocumentSourceCheckInvalidate(
        expCtx,
        parsed.getStartAfterInvalidate()
            ? boost::optional<ResumeTokenData>(parsed.getStartAfterInvalidate()->getData())
            : boost::none);
}

DocumentSource::GetNextResult DocumentSourceCheckInvalidate::doGetNext() {
    // To declare a change stream as invalidated, this stage first emits an invalidate event and
    // then throws a 'ChangeStreamInvalidated' exception on the next call to this method.

    if (_queuedInvalidate) {
        const auto res = DocumentSource::GetNextResult(std::move(_queuedInvalidate.get()));
        _queuedInvalidate.reset();
        return res;
    }

    if (_queuedException &&
        feature_flags::gFeatureFlagChangeStreamsOptimization.isEnabledAndIgnoreFCV()) {
        uasserted(static_cast<ChangeStreamInvalidationInfo>(*_queuedException),
                  "Change stream invalidated");
    }

    auto nextInput = pSource->getNext();
    if (!nextInput.isAdvanced())
        return nextInput;

    auto doc = nextInput.getDocument();
    const auto& kOperationTypeField = DSCS::kOperationTypeField;
    DSCS::checkValueType(doc[kOperationTypeField], kOperationTypeField, BSONType::String);
    auto operationType = doc[kOperationTypeField].getString();

    // If this command should invalidate the stream, generate an invalidate entry and queue it up
    // to be returned after the notification of this command. The new entry will have a nearly
    // identical resume token to the notification for the command, except with an extra flag
    // indicating that the token is from an invalidate. This flag is necessary to disambiguate
    // the two tokens, and thus preserve a total ordering on the stream.
    if (isInvalidatingCommand(pExpCtx, operationType)) {
        auto resumeTokenData = ResumeToken::parse(doc[DSCS::kIdField].getDocument()).getData();
        resumeTokenData.fromInvalidate = ResumeTokenData::FromInvalidate::kFromInvalidate;

        // If a client receives an invalidate and wants to start a new stream after the invalidate,
        // they can use the 'startAfter' option. In this case, '_startAfterInvalidate' will be set
        // to the resume token with which the client restarted the stream. We must be sure to avoid
        // re-invalidating the new stream, and so we will swallow the first invalidate we see on
        // each shard. The one exception is the invalidate which matches the 'startAfter' resume
        // token. We must re-generate this invalidate, since DSEnsureResumeTokenPresent needs to see
        // (and will take care of swallowing) the event which exactly matches the client's token.
        if (_startAfterInvalidate && resumeTokenData != _startAfterInvalidate) {
            _startAfterInvalidate.reset();
            return nextInput;
        }

        auto resumeTokenDoc = ResumeToken(resumeTokenData).toDocument();

        MutableDocument result(Document{{DSCS::kIdField, resumeTokenDoc},
                                        {DSCS::kOperationTypeField, DSCS::kInvalidateOpType},
                                        {DSCS::kClusterTimeField, doc[DSCS::kClusterTimeField]}});
        result.copyMetaDataFrom(doc);

        // We set the resume token as the document's sort key in both the sharded and non-sharded
        // cases, since we will later rely upon it to generate a correct postBatchResumeToken. We
        // must therefore update the sort key to match the new resume token that we generated above.
        const bool isSingleElementKey = true;
        result.metadata().setSortKey(Value{resumeTokenDoc}, isSingleElementKey);

        _queuedInvalidate = result.freeze();

        // By this point, either the '_startAfterInvalidate' is absent or it is present and the
        // current event matches the resume token. In the latter case, we do not want to close the
        // change stream and should not throw an exception. Therefore, we only queue up an exception
        // if '_startAfterInvalidate' is absent.
        if (!_startAfterInvalidate) {
            _queuedException = ChangeStreamInvalidationInfo(
                _queuedInvalidate->metadata().getSortKey().getDocument().toBson());
        }
    }

    // Regardless of whether the first document we see is an invalidating command, we only skip the
    // first invalidate for streams with the 'startAfter' option, so we should not skip any
    // invalidates that come after the first one.
    _startAfterInvalidate.reset();

    return nextInput;
}

Value DocumentSourceCheckInvalidate::serializeLatest(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    if (explain) {
        return Value(Document{{DocumentSourceChangeStream::kStageName,
                               Document{{"stage"_sd, "internalCheckInvalidate"_sd}}}});
    }

    DocumentSourceChangeStreamCheckInvalidateSpec spec;
    if (_startAfterInvalidate) {
        spec.setStartAfterInvalidate(ResumeToken(*_startAfterInvalidate));
    }
    return Value(Document{{DocumentSourceCheckInvalidate::kStageName, spec.toBSON()}});
}

}  // namespace mongo
