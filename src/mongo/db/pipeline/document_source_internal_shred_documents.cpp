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


#include "mongo/db/pipeline/document_source_internal_shred_documents.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

REGISTER_DOCUMENT_SOURCE(_internalShredDocuments,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceInternalShredDocuments::createFromBson,
                         AllowedWithApiStrict::kNeverInVersion1);
ALLOCATE_DOCUMENT_SOURCE_ID(_internalShredDocuments, DocumentSourceInternalShredDocuments::id)

DocumentSourceInternalShredDocuments::DocumentSourceInternalShredDocuments(
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx)
    : DocumentSource(kStageName, pExpCtx) {}

Value DocumentSourceInternalShredDocuments::serialize(const SerializationOptions& opts) const {
    return Value(DOC(getSourceName() << Document()));
}

boost::intrusive_ptr<DocumentSource> DocumentSourceInternalShredDocuments::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(7997500,
            "$_internalShredDocuments specification must be an object",
            elem.type() == BSONType::object);
    uassert(7997501, "$_internalShredDocuments specification must be empty", elem.Obj().isEmpty());
    return DocumentSourceInternalShredDocuments::create(expCtx);
}

boost::intrusive_ptr<DocumentSource> DocumentSourceInternalShredDocuments::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return make_intrusive<DocumentSourceInternalShredDocuments>(expCtx);
}

}  // namespace mongo
