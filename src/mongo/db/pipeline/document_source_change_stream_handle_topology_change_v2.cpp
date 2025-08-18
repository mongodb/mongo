/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_change_stream_handle_topology_change_v2.h"

#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

ALLOCATE_DOCUMENT_SOURCE_ID(_internalChangeStreamHandleTopologyChangeV2,
                            DocumentSourceChangeStreamHandleTopologyChangeV2::id)

boost::intrusive_ptr<DocumentSourceChangeStreamHandleTopologyChangeV2>
DocumentSourceChangeStreamHandleTopologyChangeV2::createFromBson(
    const BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(10612600,
            str::stream() << "the '" << kStageName << "' spec must be an empty object",
            elem.type() == BSONType::object && elem.Obj().isEmpty());
    return new DocumentSourceChangeStreamHandleTopologyChangeV2(expCtx);
}

boost::intrusive_ptr<DocumentSourceChangeStreamHandleTopologyChangeV2>
DocumentSourceChangeStreamHandleTopologyChangeV2::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return new DocumentSourceChangeStreamHandleTopologyChangeV2(expCtx);
}

DocumentSourceChangeStreamHandleTopologyChangeV2::DocumentSourceChangeStreamHandleTopologyChangeV2(
    const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSourceInternalChangeStreamStage(kStageName, expCtx),
      exec::agg::Stage(kStageName, expCtx) {}

StageConstraints DocumentSourceChangeStreamHandleTopologyChangeV2::constraints(
    PipelineSplitState pipeState) const {
    StageConstraints constraints{StreamType::kStreaming,
                                 PositionRequirement::kNone,
                                 HostTypeRequirement::kRouter,
                                 DiskUseRequirement::kNoDiskUse,
                                 FacetRequirement::kNotAllowed,
                                 TransactionRequirement::kNotAllowed,
                                 LookupRequirement::kNotAllowed,
                                 UnionRequirement::kNotAllowed,
                                 ChangeStreamRequirement::kChangeStreamStage};

    // Can be swapped with the '$match', '$redact', and 'DocumentSourceSingleDocumentTransformation'
    // stages and ensures that they get pushed down to the shards, as this stage bisects the change
    // streams pipeline.
    constraints.canSwapWithMatch = true;
    constraints.canSwapWithSingleDocTransformOrRedact = true;
    constraints.consumesLogicalCollectionData = false;

    return constraints;
}

DocumentSource::GetNextResult DocumentSourceChangeStreamHandleTopologyChangeV2::doGetNext() {
    return pSource->getNext();
}

Value DocumentSourceChangeStreamHandleTopologyChangeV2::doSerialize(
    const SerializationOptions& opts) const {
    if (opts.isSerializingForExplain()) {
        return Value(DOC(DocumentSourceChangeStream::kStageName
                         << DOC("stage" << "internalHandleTopologyChangeV2"_sd)));
    }

    return Value(Document{{kStageName, Document()}});
}

}  // namespace mongo
