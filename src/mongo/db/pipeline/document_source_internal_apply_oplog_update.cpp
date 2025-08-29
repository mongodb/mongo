/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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


#include "mongo/db/pipeline/document_source_internal_apply_oplog_update.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source_internal_apply_oplog_update_gen.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

REGISTER_DOCUMENT_SOURCE(_internalApplyOplogUpdate,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceInternalApplyOplogUpdate::createFromBson,
                         AllowedWithApiStrict::kNeverInVersion1);
ALLOCATE_DOCUMENT_SOURCE_ID(_internalApplyOplogUpdate, DocumentSourceInternalApplyOplogUpdate::id)

boost::intrusive_ptr<DocumentSource> DocumentSourceInternalApplyOplogUpdate::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(6315901,
            str::stream() << "Argument to " << kStageName
                          << " stage must be an object, but found type: " << typeName(elem.type()),
            elem.type() == BSONType::object);

    auto spec =
        InternalApplyOplogUpdateSpec::parse(elem.embeddedObject(), IDLParserContext(kStageName));

    return new DocumentSourceInternalApplyOplogUpdate(pExpCtx, spec.getOplogUpdate());
}

DocumentSourceInternalApplyOplogUpdate::DocumentSourceInternalApplyOplogUpdate(
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx, const BSONObj& oplogUpdate)
    : DocumentSource(kStageName, pExpCtx), _oplogUpdate(oplogUpdate) {}

Value DocumentSourceInternalApplyOplogUpdate::serialize(const SerializationOptions& opts) const {
    return Value(Document{
        {kStageName, Document{{kOplogUpdateFieldName, opts.serializeLiteral(_oplogUpdate)}}}});
}

}  // namespace mongo
