// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_internal_inhibit_optimization.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(_internalInhibitOptimization,
                                     InternalInhibitOptimizationLiteParsed::parse,
                                     AllowedWithApiStrict::kNeverInVersion1);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(_internalInhibitOptimization,
                                                   DocumentSourceInternalInhibitOptimization,
                                                   InternalInhibitOptimizationStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(_internalInhibitOptimization,
                            DocumentSourceInternalInhibitOptimization::id);

constexpr std::string_view DocumentSourceInternalInhibitOptimization::kStageName;

boost::intrusive_ptr<DocumentSource> DocumentSourceInternalInhibitOptimization::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "$_internalInhibitOptimization must take a nested object but found: "
                          << elem,
            elem.type() == BSONType::object);

    auto specObj = elem.embeddedObject();
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "$_internalInhibitOptimization must take an empty object but found: "
                          << specObj,
            specObj.isEmpty());

    return new DocumentSourceInternalInhibitOptimization(expCtx);
}

Value DocumentSourceInternalInhibitOptimization::serialize(
    const query_shape::SerializationOptions& opts) const {
    return Value(Document{{getSourceName(), Value{Document{}}}});
}

}  // namespace mongo
