// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_change_stream_end_of_transaction.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

ALLOCATE_DOCUMENT_SOURCE_ID(_internalChangeStreamEndOfTransaction,
                            DocumentSourceChangeStreamEndOfTransaction::id)

boost::intrusive_ptr<DocumentSourceChangeStreamEndOfTransaction>
DocumentSourceChangeStreamEndOfTransaction::createFromBson(
    const BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(13766900,
            str::stream() << "the '" << kStageName << "' spec must be an empty object",
            elem.type() == BSONType::object && elem.Obj().isEmpty());
    return create(expCtx);
}

boost::intrusive_ptr<DocumentSourceChangeStreamEndOfTransaction>
DocumentSourceChangeStreamEndOfTransaction::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return new DocumentSourceChangeStreamEndOfTransaction(expCtx);
}

DocumentSourceChangeStreamEndOfTransaction::DocumentSourceChangeStreamEndOfTransaction(
    const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSourceInternalChangeStreamStage(kStageName, expCtx) {}

StageConstraints DocumentSourceChangeStreamEndOfTransaction::constraints(
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

    // Like the topology stage, allow '$match', '$redact' and single-document transformations to be
    // swapped past this stage so that they are pushed down to the shards. The stage re-applies a
    // copy of those user stages to the events it synthesizes on the router.
    constraints.canSwapWithMatch = true;
    constraints.canSwapWithSingleDocTransformOrRedact = true;

    // The stage neither consumes logical collection data nor depends on more than its single input.
    constraints.consumesLogicalCollectionData = false;
    constraints.outputDependsOnSingleInput = true;

    return constraints;
}

void DocumentSourceChangeStreamEndOfTransaction::setUserPipeline(
    DocumentSourceContainer userStages) {
    if (!userStages.empty()) {
        _userPipeline = Pipeline::create(std::move(userStages), getExpCtx());
    }
}

Value DocumentSourceChangeStreamEndOfTransaction::doSerialize(
    const query_shape::SerializationOptions& opts) const {
    if (opts.isShapifying()) {
        // Stages made internally by 'DocumentSourceChangeStream' should not be included in the
        // query shape. When shapifying we will serialize only the user specified $changeStream
        // stage.
        return Value();
    }

    if (opts.isSerializingForExplain()) {
        BSONObjBuilder builder;
        BSONObjBuilder sub(builder.subobjStart(DocumentSourceChangeStream::kStageName));
        sub.append("stage"sv, "internalEndOfTransaction");
        if (_userPipeline) {
            sub << "pipeline"sv << Value(_userPipeline->serializeToBson(opts));
        }
        sub.done();
        return Value(builder.obj());
    }

    return Value(Document{{kStageName, Document()}});
}

}  // namespace mongo
