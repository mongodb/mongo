// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/s/resharding/document_source_resharding_add_resume_id.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

REGISTER_INTERNAL_LITE_PARSED_DOCUMENT_SOURCE(_addReshardingResumeId,
                                              ReshardingAddResumeIdLiteParsed::parse);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(_addReshardingResumeId,
                                                   DocumentSourceReshardingAddResumeId,
                                                   ReshardingAddResumeIdStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(_addReshardingResumeId, DocumentSourceReshardingAddResumeId::id)

boost::intrusive_ptr<DocumentSourceReshardingAddResumeId>
DocumentSourceReshardingAddResumeId::create(const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return new DocumentSourceReshardingAddResumeId(expCtx);
}

boost::intrusive_ptr<DocumentSourceReshardingAddResumeId>
DocumentSourceReshardingAddResumeId::createFromBson(
    const BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(6387803,
            str::stream() << "the '" << kStageName << "' spec must be an empty object",
            elem.type() == BSONType::object && elem.Obj().isEmpty());
    return new DocumentSourceReshardingAddResumeId(expCtx);
}

DocumentSourceReshardingAddResumeId::DocumentSourceReshardingAddResumeId(
    const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSource(kStageName, expCtx) {}

StageConstraints DocumentSourceReshardingAddResumeId::constraints(
    PipelineSplitState pipeState) const {
    StageConstraints constraints(StreamType::kStreaming,
                                 PositionRequirement::kNone,
                                 HostTypeRequirement::kTargetedShards,
                                 DiskUseRequirement::kNoDiskUse,
                                 FacetRequirement::kNotAllowed,
                                 TransactionRequirement::kNotAllowed,
                                 LookupRequirement::kNotAllowed,
                                 UnionRequirement::kNotAllowed,
                                 ChangeStreamRequirement::kDenylist);
    constraints.preservesCardinality = true;
    constraints.consumesLogicalCollectionData = false;
    return constraints;
}

Value DocumentSourceReshardingAddResumeId::serialize(
    const query_shape::SerializationOptions& opts) const {
    return Value(Document{{kStageName, Value(Document{})}});
}

DepsTracker::State DocumentSourceReshardingAddResumeId::getDependencies(DepsTracker* deps) const {
    deps->fields.insert(std::string{repl::OplogEntry::kTimestampFieldName});
    deps->fields.insert(std::string{CommitTransactionOplogObject::kCommitTimestampFieldName});
    return DepsTracker::State::SEE_NEXT;
}

DocumentSource::GetModPathsReturn DocumentSourceReshardingAddResumeId::getModifiedPaths() const {
    return {GetModPathsReturn::Type::kFiniteSet,
            {std::string{repl::OplogEntry::k_idFieldName},
             std::string{CommitTransactionOplogObject::kCommitTimestampFieldName}},
            {}};
}
}  // namespace mongo
