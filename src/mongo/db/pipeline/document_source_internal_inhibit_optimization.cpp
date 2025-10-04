/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

REGISTER_DOCUMENT_SOURCE(_internalInhibitOptimization,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceInternalInhibitOptimization::createFromBson,
                         AllowedWithApiStrict::kNeverInVersion1);
ALLOCATE_DOCUMENT_SOURCE_ID(_internalInhibitOptimization,
                            DocumentSourceInternalInhibitOptimization::id)

constexpr StringData DocumentSourceInternalInhibitOptimization::kStageName;

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

Value DocumentSourceInternalInhibitOptimization::serialize(const SerializationOptions& opts) const {
    return Value(Document{{getSourceName(), Value{Document{}}}});
}

}  // namespace mongo
