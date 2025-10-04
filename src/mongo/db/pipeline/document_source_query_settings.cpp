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

#include "document_source_query_settings.h"

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

REGISTER_DOCUMENT_SOURCE(querySettings,
                         DocumentSourceQuerySettings::LiteParsed::parse,
                         DocumentSourceQuerySettings::createFromBson,
                         AllowedWithApiStrict::kNeverInVersion1);
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

Value DocumentSourceQuerySettings::serialize(const SerializationOptions& opts) const {
    return Value(DOC(getSourceName() << DOC("showDebugQueryShape" << Value(_showDebugQueryShape))));
}
}  // namespace mongo
