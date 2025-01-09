/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source_internal_shard_filter.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/search/document_source_internal_search_mongot_remote_gen.h"

namespace mongo {

using boost::intrusive_ptr;

REGISTER_DOCUMENT_SOURCE(_internalSearchIdLookup,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceInternalSearchIdLookUp::createFromBson,
                         AllowedWithApiStrict::kInternal);

DocumentSourceInternalSearchIdLookUp::DocumentSourceInternalSearchIdLookUp(
    const intrusive_ptr<ExpressionContext>& expCtx,
    long long limit,
    ExecShardFilterPolicy shardFilterPolicy)
    : DocumentSource(kStageName, expCtx), _limit(limit), _shardFilterPolicy(shardFilterPolicy) {

    // We need to reset the docsSeenByIdLookup/docsReturnedByIdLookup in the state shared by the
    // DocumentSourceInternalSearchMongotRemote and DocumentSourceInternalSearchIdLookup stages when
    // we create a new DocumentSourceInternalSearchIdLookup stage. This is because if $search is
    // part of a $lookup sub-pipeline, the sub-pipeline gets parsed anew for every document the
    // stage processes, but each parse uses the same expression context.
    pExpCtx->sharedSearchState.resetDocsReturnedByIdLookup();
}

intrusive_ptr<DocumentSource> DocumentSourceInternalSearchIdLookUp::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(
        31016,
        str::stream() << "$_internalSearchIdLookup value must be an empty object or just have "
                         "one field called 'limit'. Found: "
                      << typeName(elem.type()),
        elem.type() == BSONType::Object &&
            (elem.embeddedObject().isEmpty() ||
             ((elem.embeddedObject().nFields() == 1) &&
              elem.embeddedObject().hasField(InternalSearchMongotRemoteSpec::kLimitFieldName))));
    auto specObj = elem.embeddedObject();
    if (specObj.hasField(InternalSearchMongotRemoteSpec::kLimitFieldName)) {
        auto limitElem = specObj.getField(InternalSearchMongotRemoteSpec::kLimitFieldName);
        uassert(6770001, "Limit must be a long", limitElem.type() == BSONType::NumberLong);
        return make_intrusive<DocumentSourceInternalSearchIdLookUp>(expCtx, limitElem.Long());
    }
    return make_intrusive<DocumentSourceInternalSearchIdLookUp>(expCtx);
}

Value DocumentSourceInternalSearchIdLookUp::serialize(const SerializationOptions& opts) const {
    auto internalDoc = _limit == 0 ? Document()
                                   : DOC(InternalSearchMongotRemoteSpec::kLimitFieldName
                                         << opts.serializeLiteral(Value((long long)_limit)));
    return Value(DOC(getSourceName() << internalDoc));
}

DocumentSource::GetNextResult DocumentSourceInternalSearchIdLookUp::doGetNext() {
    boost::optional<Document> result;
    Document inputDoc;
    if (_limit != 0 && pExpCtx->sharedSearchState.getDocsReturnedByIdLookup() >= _limit) {
        return DocumentSource::GetNextResult::makeEOF();
    }
    while (!result) {
        auto nextInput = pSource->getNext();
        if (!nextInput.isAdvanced()) {
            return nextInput;
        }

        inputDoc = nextInput.releaseDocument();
        auto documentId = inputDoc["_id"];

        if (!documentId.missing()) {
            auto documentKey = Document({{"_id", documentId}});

            uassert(31052,
                    "Collection must have a UUID to use $_internalSearchIdLookup.",
                    pExpCtx->uuid.has_value());

            // Find the document by performing a local read.
            MakePipelineOptions pipelineOpts;
            pipelineOpts.attachCursorSource = false;
            auto pipeline =
                Pipeline::makePipeline({BSON("$match" << documentKey)}, pExpCtx, pipelineOpts);

            pipeline = pExpCtx->mongoProcessInterface->attachCursorSourceToPipelineForLocalRead(
                pipeline.release(), boost::none, _shardFilterPolicy);

            result = pipeline->getNext();
            if (auto next = pipeline->getNext()) {
                uasserted(ErrorCodes::TooManyMatchingDocuments,
                          str::stream() << "found more than one document with document key "
                                        << documentKey.toString() << ": [" << result->toString()
                                        << ", " << next->toString() << "]");
            }
        }
    }

    // Result must be populated here - EOF returns above.
    invariant(result);
    MutableDocument output(*result);

    // Transfer searchScore metadata from inputDoc to the result.
    output.copyMetaDataFrom(inputDoc);
    pExpCtx->sharedSearchState.incrementDocsReturnedByIdLookup();
    return output.freeze();
}

const char* DocumentSourceInternalSearchIdLookUp::getSourceName() const {
    return kStageName.rawData();
}

Pipeline::SourceContainer::iterator DocumentSourceInternalSearchIdLookUp::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    for (auto optItr = std::next(itr); optItr != container->end(); ++optItr) {
        auto limitStage = dynamic_cast<DocumentSourceLimit*>(optItr->get());
        if (limitStage) {
            _limit = limitStage->getLimit();
            break;
        }
        if (!optItr->get()->constraints().canSwapWithSkippingOrLimitingStage) {
            break;
        }
    }
    return std::next(itr);
}

}  // namespace mongo
