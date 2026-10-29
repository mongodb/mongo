// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_documents.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/uuid.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using boost::intrusive_ptr;

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(documents,
                                     DocumentSourceDocuments::LiteParsed::parse,
                                     AllowedWithApiStrict::kAlways);

REGISTER_DOCUMENT_SOURCE_CONTAINER_WITH_STAGE_PARAMS_DEFAULT(documents,
                                                             DocumentSourceDocuments,
                                                             DocumentsStageParams);

std::list<intrusive_ptr<DocumentSource>> DocumentSourceDocuments::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {
    // kGenFieldName is a temporary field to hold docs to wire $project, $unwind, and $replaceRoot
    // together. This may show up in explain, but it will not possibly overlap with user data, since
    // the user data is nested a level below, within an array or object.
    auto projectContent = BSON(kGenFieldName << elem);
    auto queue = DocumentSourceQueue::create(expCtx, DocumentSourceDocuments::kStageName);
    queue->emplace_back(Document{});
    /* Create the following pipeline from $documents: [...]
     *  => [ {$queue: [{}] },
     *       {$project: {[kGenFieldName]: [...]}},
     *       {$unwind: "$" + kGenFieldName},
     *       {$replaceWith: "$" + kGenFieldName} ]
     */
    return {queue,
            DocumentSourceProject::create(projectContent, expCtx, elem.fieldNameStringData()),
            DocumentSourceUnwind::create(expCtx, kGenFieldName, false, {}, true),
            DocumentSourceReplaceRoot::create(
                expCtx,
                ExpressionFieldPath::createPathFromString(
                    expCtx.get(), kGenFieldName, expCtx->variablesParseState),
                "elements within the array passed to $documents",
                SbeCompatibility::noRequirements)};
}


boost::optional<std::vector<BSONObj>> DocumentSourceDocuments::extractDesugaredStagesFromPipeline(
    const std::vector<BSONObj>& pipeline) {
    if (pipeline.size() >= 4 && pipeline[0].hasField(DocumentSourceQueue::kStageName) &&
        pipeline[1].hasField(DocumentSourceProject::kStageName) &&
        pipeline[1][DocumentSourceProject::kStageName].Obj().hasField(
            DocumentSourceDocuments::kGenFieldName)) {
        return boost::optional<std::vector<BSONObj>>(
            {pipeline[0], pipeline[1], pipeline[2], pipeline[3]});
    }
    return boost::none;
}

}  // namespace mongo
