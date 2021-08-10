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

#include "mongo/db/pipeline/document_source_documents.h"
#include "mongo/db/exec/projection_executor.h"
#include "mongo/db/exec/projection_executor_builder.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using boost::intrusive_ptr;

REGISTER_INTERNAL_DOCUMENT_SOURCE(documents,
                                  LiteParsedDocumentSourceDefault::parse,
                                  DocumentSourceDocuments::createFromBson,
                                  true);

std::list<intrusive_ptr<DocumentSource>> DocumentSourceDocuments::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {
    // genField is a temporary field to hold docs to wire $project,
    // $unwind, and $replaceRoot together.
    auto genField = UUID::gen().toString();
    auto projectContent = BSON(genField << elem);
    auto queue = DocumentSourceQueue::create(expCtx);
    queue->emplace_back(Document{});
    /* Create the following pipeline from $documents: [...]
     *  => [ queue([{}]),
     *       project: {tempDocumentsField: [...]},
     *       unwind: "$tempDocumentsField",
     *       replaceWith: "$tempDocumentsField" ]
     */
    return {
        queue,
        DocumentSourceProject::create(projectContent, expCtx, elem.fieldNameStringData()),
        DocumentSourceUnwind::create(expCtx, genField, false, {}, true),
        DocumentSourceReplaceRoot::createFromBson(
            BSON("$replaceRoot" << BSON("newRoot" << std::string("$") + genField)).firstElement(),
            expCtx)};
}

}  // namespace mongo
