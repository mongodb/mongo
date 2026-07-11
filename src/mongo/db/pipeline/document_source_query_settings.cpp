// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_query_settings.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_query_settings_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using namespace query_settings;

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(querySettings,
                                     DocumentSourceQuerySettings::LiteParsed::parse,
                                     AllowedWithApiStrict::kNeverInVersion1);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(querySettings,
                                                   DocumentSourceQuerySettings,
                                                   QuerySettingsStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(querySettings, DocumentSourceQuerySettings::id)

DocumentSourceQuerySettings::DocumentSourceQuerySettings(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, bool showDebugQueryShape)
    : DocumentSource(kStageName, expCtx), _showDebugQueryShape(showDebugQueryShape) {}

boost::intrusive_ptr<DocumentSource> DocumentSourceQuerySettings::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(7746801,
            "$querySettings stage expects a document as argument",
            elem.type() == BSONType::object);

    // Resolve whether to include the debug query shape or not.
    bool showDebugQueryShape = DocumentSourceQuerySettingsSpec::parse(
                                   elem.embeddedObject(), IDLParserContext("$querySettings"))
                                   .getShowDebugQueryShape();
    return make_intrusive<DocumentSourceQuerySettings>(expCtx, showDebugQueryShape);
}

Value DocumentSourceQuerySettings::serialize(const query_shape::SerializationOptions& opts) const {
    return Value(DOC(getSourceName() << DOC("showDebugQueryShape" << Value(_showDebugQueryShape))));
}
}  // namespace mongo
