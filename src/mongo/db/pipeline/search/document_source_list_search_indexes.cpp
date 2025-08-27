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

#include "mongo/db/pipeline/search/document_source_list_search_indexes.h"

#include "mongo/db/pipeline/search/document_source_list_search_indexes_gen.h"
#include "mongo/db/query/search/search_index_common.h"
#include "mongo/db/query/search/search_index_process_interface.h"
#include "mongo/db/version_context.h"

#include <boost/optional/optional.hpp>

namespace mongo {

REGISTER_DOCUMENT_SOURCE(listSearchIndexes,
                         DocumentSourceListSearchIndexes::LiteParsedListSearchIndexes::parse,
                         DocumentSourceListSearchIndexes::createFromBson,
                         AllowedWithApiStrict::kNeverInVersion1)
ALLOCATE_DOCUMENT_SOURCE_ID(listSearchIndexes, DocumentSourceListSearchIndexes::id)

void DocumentSourceListSearchIndexes::validateListSearchIndexesSpec(
    const DocumentSourceListSearchIndexesSpec* spec) {
    uassert(ErrorCodes::InvalidOptions,
            "Cannot set both 'name' and 'id' for $listSearchIndexes.",
            !(spec->getId() && spec->getName()));
};

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

Value DocumentSourceListSearchIndexes::serialize(const SerializationOptions& opts) const {
    BSONObjBuilder bob;
    auto spec = DocumentSourceListSearchIndexesSpec::parse(_cmdObj, IDLParserContext(kStageName));
    spec.serialize(&bob, opts);
    return Value(Document{{kStageName, bob.done()}});
}

// We use 'kLocalOnly' because the aggregation request can be handled by a shard or mongos depending
// on where the user sends the request.
StageConstraints DocumentSourceListSearchIndexes::constraints(PipelineSplitState pipeState) const {
    StageConstraints constraints(StreamType::kStreaming,
                                 PositionRequirement::kFirst,
                                 HostTypeRequirement::kLocalOnly,
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
