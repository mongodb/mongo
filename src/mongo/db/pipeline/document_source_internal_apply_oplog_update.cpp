// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/pipeline/document_source_internal_apply_oplog_update.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source_internal_apply_oplog_update_gen.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(_internalApplyOplogUpdate,
                                     InternalApplyOplogUpdateLiteParsed::parse,
                                     AllowedWithApiStrict::kNeverInVersion1);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(_internalApplyOplogUpdate,
                                                   DocumentSourceInternalApplyOplogUpdate,
                                                   InternalApplyOplogUpdateStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(_internalApplyOplogUpdate, DocumentSourceInternalApplyOplogUpdate::id);

boost::intrusive_ptr<DocumentSource> DocumentSourceInternalApplyOplogUpdate::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(6315901,
            str::stream() << "Argument to " << kStageName
                          << " stage must be an object, but found type: " << typeName(elem.type()),
            elem.type() == BSONType::object);

    auto spec =
        InternalApplyOplogUpdateSpec::parse(elem.embeddedObject(), IDLParserContext(kStageName));

    return new DocumentSourceInternalApplyOplogUpdate(pExpCtx, spec.getOplogUpdate());
}

DocumentSourceInternalApplyOplogUpdate::DocumentSourceInternalApplyOplogUpdate(
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx, const BSONObj& oplogUpdate)
    : DocumentSource(kStageName, pExpCtx), _oplogUpdate(oplogUpdate) {}

Value DocumentSourceInternalApplyOplogUpdate::serialize(
    const query_shape::SerializationOptions& opts) const {
    return Value(Document{{kStageName,
                           Document{{kOplogUpdateFieldName,
                                     opts.serializeLiteral(_oplogUpdate, Value(Document{}))}}}});
}

}  // namespace mongo
