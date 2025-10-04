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

#include "mongo/db/pipeline/document_source_change_stream_add_post_image.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/str.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

constexpr StringData DocumentSourceChangeStreamAddPostImage::kStageName;
constexpr StringData DocumentSourceChangeStreamAddPostImage::kFullDocumentFieldName;

REGISTER_INTERNAL_DOCUMENT_SOURCE(_internalChangeStreamAddPostImage,
                                  LiteParsedDocumentSourceChangeStreamInternal::parse,
                                  DocumentSourceChangeStreamAddPostImage::createFromBson,
                                  true);
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

Value DocumentSourceChangeStreamAddPostImage::doSerialize(const SerializationOptions& opts) const {
    return opts.isSerializingForExplain()
        ? Value(Document{
              {DocumentSourceChangeStream::kStageName,
               Document{{"stage"_sd, kStageName},
                        {kFullDocumentFieldName, FullDocumentMode_serializer(_fullDocumentMode)}}}})
        : Value(Document{{kStageName,
                          DocumentSourceChangeStreamAddPostImageSpec(_fullDocumentMode).toBSON()}});
}
}  // namespace mongo
