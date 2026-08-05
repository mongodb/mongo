// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_internal_query_settings_debug_shape.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using boost::intrusive_ptr;

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(
    _internalQuerySettingsDebugShape,
    DocumentSourceInternalQuerySettingsDebugShape::LiteParsed::parse,
    AllowedWithApiStrict::kNeverInVersion1);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(_internalQuerySettingsDebugShape,
                                                   DocumentSourceInternalQuerySettingsDebugShape,
                                                   InternalQuerySettingsDebugShapeStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(_internalQuerySettingsDebugShape,
                            DocumentSourceInternalQuerySettingsDebugShape::id);

DocumentSourceInternalQuerySettingsDebugShape::DocumentSourceInternalQuerySettingsDebugShape(
    const intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSource(kStageName, expCtx) {}

intrusive_ptr<DocumentSource> DocumentSourceInternalQuerySettingsDebugShape::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(12915501,
            str::stream() << kStageName << " must take a nested empty object but found: " << elem,
            elem.type() == BSONType::object && elem.embeddedObject().isEmpty());
    return make_intrusive<DocumentSourceInternalQuerySettingsDebugShape>(expCtx);
}

Value DocumentSourceInternalQuerySettingsDebugShape::serialize(
    const query_shape::SerializationOptions& opts) const {
    return Value(DOC(getSourceName() << Document()));
}

}  // namespace mongo
