// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/search/document_source_list_search_indexes.h"

#include "mongo/db/pipeline/search/document_source_list_search_indexes_gen.h"
#include "mongo/db/query/search/search_index_common.h"
#include "mongo/db/query/search/search_index_process_interface.h"
#include "mongo/db/version_context.h"


namespace mongo {

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(
    listSearchIndexes,
    DocumentSourceListSearchIndexes::LiteParsedListSearchIndexes::parse,
    AllowedWithApiStrict::kNeverInVersion1);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(listSearchIndexes,
                                                   DocumentSourceListSearchIndexes,
                                                   ListSearchIndexesStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(listSearchIndexes, DocumentSourceListSearchIndexes::id)

boost::intrusive_ptr<DocumentSource> DocumentSourceListSearchIndexes::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    // We must validate if atlas is configured. However, we might just be parsing or validating the
    // query without executing it. In this scenario, there is no reason to check if we are running
    // with atlas configured, since we will never make a call to the search index management host.
    // For example, if we are in query analysis, performing pipeline-style updates, or creating
    // query shapes. Additionally, it would be an error to validate this inside query analysis,
    // since query analysis doesn't have access to the search index management host.
    //
    // This validation should occur before parsing so in the case of a parse and configuration
    // error, the configuration error is thrown.
    if (pExpCtx->getMongoProcessInterface()->isExpectedToExecuteQueries()) {
        throwIfNotRunningWithRemoteSearchIndexManagement();
    }

    uassert(ErrorCodes::FailedToParse,
            str::stream() << "The $listSearchIndexes stage specification must be an object. Found: "
                          << typeName(elem.type()),
            elem.type() == BSONType::object);
    auto spec = DocumentSourceListSearchIndexesSpec::parse(elem.embeddedObject(),
                                                           IDLParserContext(kStageName));

    return new DocumentSourceListSearchIndexes(pExpCtx, elem.Obj());
}

Value DocumentSourceListSearchIndexes::serialize(
    const query_shape::SerializationOptions& opts) const {
    BSONObjBuilder bob;
    auto spec = DocumentSourceListSearchIndexesSpec::parse(_cmdObj, IDLParserContext(kStageName));
    spec.serialize(&bob, opts);
    return Value(Document{{kStageName, bob.done()}});
}

// We use 'kReceivingHostOnly' because the aggregation request can be handled by a shard or mongos
// depending on where the user sends the request.
StageConstraints DocumentSourceListSearchIndexes::constraints(PipelineSplitState pipeState) const {
    StageConstraints constraints(StreamType::kStreaming,
                                 PositionRequirement::kFirst,
                                 HostTypeRequirement::kReceivingHostOnly,
                                 DiskUseRequirement::kNoDiskUse,
                                 FacetRequirement::kNotAllowed,
                                 TransactionRequirement::kNotAllowed,
                                 LookupRequirement::kAllowed,
                                 UnionRequirement::kAllowed,
                                 ChangeStreamRequirement::kDenylist);
    constraints.setConstraintsForNoInputSources();
    return constraints;
}
}  // namespace mongo
