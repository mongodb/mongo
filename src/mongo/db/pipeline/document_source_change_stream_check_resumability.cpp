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

#include "mongo/db/pipeline/document_source_change_stream_check_resumability.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/pipeline/change_stream_helpers.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <utility>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

using boost::intrusive_ptr;

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

REGISTER_INTERNAL_DOCUMENT_SOURCE(_internalChangeStreamCheckResumability,
                                  LiteParsedDocumentSourceChangeStreamInternal::parse,
                                  DocumentSourceChangeStreamCheckResumability::createFromBson,
                                  true);
ALLOCATE_DOCUMENT_SOURCE_ID(_internalChangeStreamCheckResumability,
                            DocumentSourceChangeStreamCheckResumability::id)

DocumentSourceChangeStreamCheckResumability::DocumentSourceChangeStreamCheckResumability(
    const intrusive_ptr<ExpressionContext>& expCtx, ResumeTokenData token)
    : DocumentSourceInternalChangeStreamStage(getSourceName(), expCtx),
      _tokenFromClient(std::move(token)) {}

intrusive_ptr<DocumentSourceChangeStreamCheckResumability>
DocumentSourceChangeStreamCheckResumability::create(const intrusive_ptr<ExpressionContext>& expCtx,
                                                    const DocumentSourceChangeStreamSpec& spec) {
    auto resumeToken = change_stream::resolveResumeTokenFromSpec(expCtx, spec);
    return new DocumentSourceChangeStreamCheckResumability(expCtx, std::move(resumeToken));
}

intrusive_ptr<DocumentSourceChangeStreamCheckResumability>
DocumentSourceChangeStreamCheckResumability::createFromBson(
    BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(5467603,
            str::stream() << "the '" << kStageName << "' object spec must be an object",
            spec.type() == BSONType::object);

    auto parsed = DocumentSourceChangeStreamCheckResumabilitySpec::parse(
        spec.embeddedObject(), IDLParserContext("DocumentSourceChangeStreamCheckResumabilitySpec"));
    return new DocumentSourceChangeStreamCheckResumability(expCtx,
                                                           parsed.getResumeToken().getData());
}

const char* DocumentSourceChangeStreamCheckResumability::getSourceName() const {
    return kStageName.data();
}

Value DocumentSourceChangeStreamCheckResumability::doSerialize(
    const SerializationOptions& opts) const {
    BSONObjBuilder builder;
    if (opts.isSerializingForExplain()) {
        BSONObjBuilder sub(builder.subobjStart(DocumentSourceChangeStream::kStageName));
        sub.append("stage"_sd, kStageName);
        sub << "resumeToken"_sd << Value(ResumeToken(_tokenFromClient).toDocument(opts));
        sub.done();
    } else {
        builder.append(
            kStageName,
            DocumentSourceChangeStreamCheckResumabilitySpec(ResumeToken(_tokenFromClient))
                .toBSON());
    }
    return Value(builder.obj());
}

}  // namespace mongo
