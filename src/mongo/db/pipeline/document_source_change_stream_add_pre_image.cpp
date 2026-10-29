// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_change_stream_add_pre_image.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
using namespace std::literals::string_view_literals;

REGISTER_INTERNAL_LITE_PARSED_DOCUMENT_SOURCE(_internalChangeStreamAddPreImage,
                                              ChangeStreamAddPreImageLiteParsed::parse);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(_internalChangeStreamAddPreImage,
                                                   DocumentSourceChangeStreamAddPreImage,
                                                   ChangeStreamAddPreImageStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(_internalChangeStreamAddPreImage,
                            DocumentSourceChangeStreamAddPreImage::id)

constexpr std::string_view DocumentSourceChangeStreamAddPreImage::kStageName;
constexpr std::string_view
    DocumentSourceChangeStreamAddPreImage::kFullDocumentBeforeChangeFieldName;

boost::intrusive_ptr<DocumentSourceChangeStreamAddPreImage>
DocumentSourceChangeStreamAddPreImage::create(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                              const DocumentSourceChangeStreamSpec& spec) {
    auto mode = spec.getFullDocumentBeforeChange();

    return make_intrusive<DocumentSourceChangeStreamAddPreImage>(expCtx, mode);
}

boost::intrusive_ptr<DocumentSourceChangeStreamAddPreImage>
DocumentSourceChangeStreamAddPreImage::createFromBson(
    const BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(5467610,
            str::stream() << "the '" << kStageName << "' stage spec must be an object",
            elem.type() == BSONType::object);
    auto parsedSpec = DocumentSourceChangeStreamAddPreImageSpec::parse(
        elem.Obj(), IDLParserContext("DocumentSourceChangeStreamAddPreImageSpec"));
    return make_intrusive<DocumentSourceChangeStreamAddPreImage>(
        expCtx, parsedSpec.getFullDocumentBeforeChange());
}

Value DocumentSourceChangeStreamAddPreImage::doSerialize(
    const query_shape::SerializationOptions& opts) const {
    return opts.isSerializingForExplain()
        ? Value(Document{{DocumentSourceChangeStream::kStageName,
                          Document{{"stage"sv, "internalAddPreImage"sv},
                                   {"fullDocumentBeforeChange"sv,
                                    idl::serialize(_fullDocumentBeforeChangeMode)}}}})
        : Value(Document{
              {kStageName,
               DocumentSourceChangeStreamAddPreImageSpec(_fullDocumentBeforeChangeMode).toBSON()}});
}

}  // namespace mongo
