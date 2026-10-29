// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_internal_shardserver_info.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <initializer_list>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(_internalShardServerInfo,
                                     DocumentSourceInternalShardServerInfo::LiteParsed::parse,
                                     AllowedWithApiStrict::kNeverInVersion1);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(_internalShardServerInfo,
                                                   DocumentSourceInternalShardServerInfo,
                                                   InternalShardServerInfoStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(_internalShardServerInfo, DocumentSourceInternalShardServerInfo::id);

boost::intrusive_ptr<DocumentSource> DocumentSourceInternalShardServerInfo::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "$_internalShardServerInfo must take an empty object but found: "
                          << elem,
            elem.type() == BSONType::object && elem.Obj().isEmpty());

    return new DocumentSourceInternalShardServerInfo(expCtx);
}

Value DocumentSourceInternalShardServerInfo::serialize(
    const query_shape::SerializationOptions& opts) const {
    return Value(Document{{getSourceName(), Value{Document{{}}}}});
}

}  // namespace mongo
