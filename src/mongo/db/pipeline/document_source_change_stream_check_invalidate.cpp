// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

REGISTER_INTERNAL_LITE_PARSED_DOCUMENT_SOURCE(_internalChangeStreamCheckInvalidate,
                                              ChangeStreamCheckInvalidateLiteParsed::parse);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(_internalChangeStreamCheckInvalidate,
                                                   DocumentSourceChangeStreamCheckInvalidate,
                                                   ChangeStreamCheckInvalidateStageParams);

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
    const query_shape::SerializationOptions& opts) const {
    BSONObjBuilder builder;
    if (opts.isSerializingForExplain()) {
        BSONObjBuilder sub(builder.subobjStart(DocumentSourceChangeStream::kStageName));
        sub.append("stage"sv, kStageName);
        sub.done();
    }
    DocumentSourceChangeStreamCheckInvalidateSpec spec;
    if (_startAfterInvalidate) {
        spec.setStartAfterInvalidate(ResumeToken(*_startAfterInvalidate));
    }
    builder.append(DocumentSourceChangeStreamCheckInvalidate::kStageName, spec.toBSON());
    return Value(builder.obj());
}

bool DocumentSourceChangeStreamCheckInvalidate::canInvalidateEventOccur(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return !expCtx->isClusterAggregation();
}

}  // namespace mongo
