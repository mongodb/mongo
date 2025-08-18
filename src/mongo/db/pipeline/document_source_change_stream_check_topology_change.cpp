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

#include "mongo/db/pipeline/document_source_change_stream_check_topology_change.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

REGISTER_INTERNAL_DOCUMENT_SOURCE(_internalChangeStreamCheckTopologyChange,
                                  LiteParsedDocumentSourceChangeStreamInternal::parse,
                                  DocumentSourceChangeStreamCheckTopologyChange::createFromBson,
                                  true);
ALLOCATE_DOCUMENT_SOURCE_ID(_internalChangeStreamCheckTopologyChange,
                            DocumentSourceChangeStreamCheckTopologyChange::id)

StageConstraints DocumentSourceChangeStreamCheckTopologyChange::constraints(
    PipelineSplitState pipeState) const {
    StageConstraints constraints(StreamType::kStreaming,
                                 PositionRequirement::kNone,
                                 HostTypeRequirement::kAnyShard,
                                 DiskUseRequirement::kNoDiskUse,
                                 FacetRequirement::kNotAllowed,
                                 TransactionRequirement::kNotAllowed,
                                 LookupRequirement::kNotAllowed,
                                 UnionRequirement::kNotAllowed,
                                 ChangeStreamRequirement::kChangeStreamStage);
    constraints.consumesLogicalCollectionData = false;
    return constraints;
}

boost::intrusive_ptr<DocumentSourceChangeStreamCheckTopologyChange>
DocumentSourceChangeStreamCheckTopologyChange::createFromBson(
    const BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(5669601,
            str::stream() << "the '" << kStageName << "' spec must be an object",
            elem.type() == BSONType::object && elem.Obj().isEmpty());
    return new DocumentSourceChangeStreamCheckTopologyChange(expCtx);
}

Value DocumentSourceChangeStreamCheckTopologyChange::doSerialize(
    const SerializationOptions& opts) const {
    if (opts.isSerializingForExplain()) {
        return Value(DOC(DocumentSourceChangeStream::kStageName
                         << DOC("stage" << "internalCheckTopologyChange"_sd)));
    }

    return Value(Document{{kStageName, Document()}});
}

}  // namespace mongo
