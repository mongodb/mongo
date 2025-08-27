/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_change_stream_check_invalidate.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/pipeline/change_stream_helpers.h"
#include "mongo/db/pipeline/change_stream_start_after_invalidate_info.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

REGISTER_INTERNAL_DOCUMENT_SOURCE(_internalChangeStreamCheckInvalidate,
                                  LiteParsedDocumentSourceChangeStreamInternal::parse,
                                  DocumentSourceChangeStreamCheckInvalidate::createFromBson,
                                  true);
ALLOCATE_DOCUMENT_SOURCE_ID(_internalChangeStreamCheckInvalidate,
                            DocumentSourceChangeStreamCheckInvalidate::id)

boost::intrusive_ptr<DocumentSourceChangeStreamCheckInvalidate>
DocumentSourceChangeStreamCheckInvalidate::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const DocumentSourceChangeStreamSpec& spec) {
    // If resuming from an "invalidate" using "startAfter", pass along the resume token data to
    // DSCSCheckInvalidate to signify that another invalidate should not be generated.
    auto resumeToken = change_stream::resolveResumeTokenFromSpec(expCtx, spec);
    return new DocumentSourceChangeStreamCheckInvalidate(
        expCtx, boost::make_optional(resumeToken.fromInvalidate, std::move(resumeToken)));
}

boost::intrusive_ptr<DocumentSourceChangeStreamCheckInvalidate>
DocumentSourceChangeStreamCheckInvalidate::createFromBson(
    BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(5467602,
            str::stream() << "the '" << kStageName << "' object spec must be an object",
            spec.type() == BSONType::object);

    auto parsed = DocumentSourceChangeStreamCheckInvalidateSpec::parse(
        spec.embeddedObject(), IDLParserContext("DocumentSourceChangeStreamCheckInvalidateSpec"));
    return new DocumentSourceChangeStreamCheckInvalidate(
        expCtx,
        parsed.getStartAfterInvalidate() ? parsed.getStartAfterInvalidate()->getData()
                                         : boost::optional<ResumeTokenData>());
}

Value DocumentSourceChangeStreamCheckInvalidate::doSerialize(
    const SerializationOptions& opts) const {
    BSONObjBuilder builder;
    if (opts.isSerializingForExplain()) {
        BSONObjBuilder sub(builder.subobjStart(DocumentSourceChangeStream::kStageName));
        sub.append("stage"_sd, kStageName);
        sub.done();
    }
    DocumentSourceChangeStreamCheckInvalidateSpec spec;
    if (_startAfterInvalidate) {
        spec.setStartAfterInvalidate(ResumeToken(*_startAfterInvalidate));
    }
    builder.append(DocumentSourceChangeStreamCheckInvalidate::kStageName, spec.toBSON());
    return Value(builder.obj());
}

}  // namespace mongo
