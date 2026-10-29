// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_change_stream_add_post_image.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/str.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

constexpr std::string_view DocumentSourceChangeStreamAddPostImage::kStageName;
constexpr std::string_view DocumentSourceChangeStreamAddPostImage::kFullDocumentFieldName;


REGISTER_INTERNAL_LITE_PARSED_DOCUMENT_SOURCE(_internalChangeStreamAddPostImage,
                                              ChangeStreamAddPostImageLiteParsed::parse);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(_internalChangeStreamAddPostImage,
                                                   DocumentSourceChangeStreamAddPostImage,
                                                   ChangeStreamAddPostImageStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(_internalChangeStreamAddPostImage,
                            DocumentSourceChangeStreamAddPostImage::id)

boost::intrusive_ptr<DocumentSourceChangeStreamAddPostImage>
DocumentSourceChangeStreamAddPostImage::createFromBson(
    const BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(5467608,
            str::stream() << "the '" << kStageName << "' stage spec must be an object",
            elem.type() == BSONType::object);
    auto parsedSpec = DocumentSourceChangeStreamAddPostImageSpec::parse(
        elem.Obj(), IDLParserContext("DocumentSourceChangeStreamAddPostImageSpec"));
    return new DocumentSourceChangeStreamAddPostImage(expCtx, parsedSpec.getFullDocument());
}

Value DocumentSourceChangeStreamAddPostImage::doSerialize(
    const query_shape::SerializationOptions& opts) const {
    return opts.isSerializingForExplain()
        ? Value(Document{{DocumentSourceChangeStream::kStageName,
                          Document{{"stage"sv, kStageName},
                                   {kFullDocumentFieldName, idl::serialize(_fullDocumentMode)}}}})
        : Value(Document{{kStageName,
                          DocumentSourceChangeStreamAddPostImageSpec(_fullDocumentMode).toBSON()}});
}
}  // namespace mongo
