// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/pipeline/document_source_internal_shred_documents.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(_internalShredDocuments,
                                     InternalShredDocumentsLiteParsed::parse,
                                     AllowedWithApiStrict::kNeverInVersion1);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(_internalShredDocuments,
                                                   DocumentSourceInternalShredDocuments,
                                                   InternalShredDocumentsStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(_internalShredDocuments, DocumentSourceInternalShredDocuments::id);

DocumentSourceInternalShredDocuments::DocumentSourceInternalShredDocuments(
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx)
    : DocumentSource(kStageName, pExpCtx) {}

Value DocumentSourceInternalShredDocuments::serialize(
    const query_shape::SerializationOptions& opts) const {
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
