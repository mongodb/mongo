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

#include "mongo/db/pipeline/document_source_change_stream_add_pre_image.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/change_stream_serverless_helpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/version_context.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <memory>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

REGISTER_INTERNAL_DOCUMENT_SOURCE(_internalChangeStreamAddPreImage,
                                  LiteParsedDocumentSourceChangeStreamInternal::parse,
                                  DocumentSourceChangeStreamAddPreImage::createFromBson,
                                  true);
ALLOCATE_DOCUMENT_SOURCE_ID(_internalChangeStreamAddPreImage,
                            DocumentSourceChangeStreamAddPreImage::id)

constexpr StringData DocumentSourceChangeStreamAddPreImage::kStageName;
constexpr StringData DocumentSourceChangeStreamAddPreImage::kFullDocumentBeforeChangeFieldName;

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

Value DocumentSourceChangeStreamAddPreImage::doSerialize(const SerializationOptions& opts) const {
    return opts.isSerializingForExplain()
        ? Value(Document{
              {DocumentSourceChangeStream::kStageName,
               Document{{"stage"_sd, "internalAddPreImage"_sd},
                        {"fullDocumentBeforeChange"_sd,
                         FullDocumentBeforeChangeMode_serializer(_fullDocumentBeforeChangeMode)}}}})
        : Value(Document{
              {kStageName,
               DocumentSourceChangeStreamAddPreImageSpec(_fullDocumentBeforeChangeMode).toBSON()}});
}

}  // namespace mongo
