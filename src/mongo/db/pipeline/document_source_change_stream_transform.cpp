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

#include "mongo/db/pipeline/document_source_change_stream_transform.h"

#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/repl/bson_extract_optime.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

using boost::intrusive_ptr;
using boost::optional;

REGISTER_INTERNAL_DOCUMENT_SOURCE(_internalChangeStreamTransform,
                                  LiteParsedDocumentSourceChangeStreamInternal::parse,
                                  DocumentSourceChangeStreamTransform::createFromBson,
                                  true);

intrusive_ptr<DocumentSourceChangeStreamTransform> DocumentSourceChangeStreamTransform::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const DocumentSourceChangeStreamSpec& spec) {
    return new DocumentSourceChangeStreamTransform(expCtx, spec);
}

intrusive_ptr<DocumentSourceChangeStreamTransform>
DocumentSourceChangeStreamTransform::createFromBson(
    BSONElement rawSpec, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(5467601,
            "the '$_internalChangeStreamTransform' object spec must be an object",
            rawSpec.type() == BSONType::Object);
    auto spec =
        DocumentSourceChangeStreamSpec::parse(IDLParserContext("$changeStream"), rawSpec.Obj());
    return new DocumentSourceChangeStreamTransform(expCtx, std::move(spec));
}

DocumentSourceChangeStreamTransform::DocumentSourceChangeStreamTransform(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, DocumentSourceChangeStreamSpec spec)
    : DocumentSource(DocumentSourceChangeStreamTransform::kStageName, expCtx),
      _changeStreamSpec(std::move(spec)),
      _transformer(expCtx, _changeStreamSpec),
      _isIndependentOfAnyCollection(expCtx->ns.isCollectionlessAggregateNS()) {

    // Extract the resume token or high-water-mark from the spec.
    auto tokenData =
        DocumentSourceChangeStream::resolveResumeTokenFromSpec(expCtx, _changeStreamSpec);

    // Set the initialPostBatchResumeToken on the expression context.
    expCtx->initialPostBatchResumeToken = ResumeToken(tokenData).toBSON();
}

StageConstraints DocumentSourceChangeStreamTransform::constraints(
    Pipeline::SplitState pipeState) const {
    StageConstraints constraints(StreamType::kStreaming,
                                 PositionRequirement::kNone,
                                 HostTypeRequirement::kNone,
                                 DiskUseRequirement::kNoDiskUse,
                                 FacetRequirement::kNotAllowed,
                                 TransactionRequirement::kNotAllowed,
                                 LookupRequirement::kNotAllowed,
                                 UnionRequirement::kNotAllowed,
                                 ChangeStreamRequirement::kChangeStreamStage);

    // This transformation could be part of a 'collectionless' change stream on an entire
    // database or cluster, mark as independent of any collection if so.
    constraints.isIndependentOfAnyCollection = _isIndependentOfAnyCollection;
    return constraints;
}

Value DocumentSourceChangeStreamTransform::serialize(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    if (explain) {
        return Value(Document{{DocumentSourceChangeStream::kStageName,
                               Document{{"stage"_sd, "internalTransform"_sd},
                                        {"options"_sd, _changeStreamSpec.toBSON()}}}});
    }

    return Value(
        Document{{DocumentSourceChangeStreamTransform::kStageName, _changeStreamSpec.toBSON()}});
}

DepsTracker::State DocumentSourceChangeStreamTransform::getDependencies(DepsTracker* deps) const {
    deps->fields.merge(_transformer.getFieldNameDependencies());
    return DepsTracker::State::EXHAUSTIVE_ALL;
}

DocumentSource::GetModPathsReturn DocumentSourceChangeStreamTransform::getModifiedPaths() const {
    // All paths are modified.
    return {DocumentSource::GetModPathsReturn::Type::kAllPaths, OrderedPathSet{}, {}};
}

DocumentSource::GetNextResult DocumentSourceChangeStreamTransform::doGetNext() {
    uassert(50988,
            "Illegal attempt to execute an internal change stream stage on mongos. A $changeStream "
            "stage must be the first stage in a pipeline",
            !pExpCtx->inMongos);

    auto input = pSource->getNext();
    if (!input.isAdvanced()) {
        return input;
    }

    return _transformer.applyTransformation(input.releaseDocument());
}

}  // namespace mongo
