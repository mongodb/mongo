// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_internal_hybrid_search.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

ALLOCATE_DOCUMENT_SOURCE_ID(_internalHybridSearch, DocumentSourceInternalHybridSearch::id);

// Reached only via the StageParams registry; intentionally not registered as a user-facing
// parser (no REGISTER_*_DOCUMENT_SOURCE).
boost::intrusive_ptr<DocumentSource> DocumentSourceInternalHybridSearch::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "$_internalHybridSearch must take a nested object but found: " << elem,
            elem.type() == BSONType::object);
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "$_internalHybridSearch must take an empty object but found: "
                          << elem.embeddedObject(),
            elem.embeddedObject().isEmpty());
    return new DocumentSourceInternalHybridSearch(expCtx);
}

Value DocumentSourceInternalHybridSearch::serialize(
    const query_shape::SerializationOptions& opts) const {
    return Value(Document{{getSourceName(), Value{Document{}}}});
}

}  // namespace mongo
