// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/resharding/resharding_add_resume_id_stage.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/s/resharding/document_source_resharding_add_resume_id.h"
#include "mongo/db/s/resharding/donor_oplog_id_gen.h"

#include <string_view>

namespace mongo {
namespace {
/**
 * Generates the _id field for the given oplog entry document. If the document corresponds to an
 * applyOps oplog entry for a committed transaction, it should have the commit timestamp for the
 * transaction attached to it, and the generated _id will be {clusterTime: <transaction commit
 * timestamp>, ts: <applyOps optime.ts>}. For all other documents, the generated _id will be
 * {clusterTime: <optime.ts>, ts: <optime.ts>}.
 */
Document appendId(Document inputDoc) {
    auto eventTime = inputDoc.getField(repl::OplogEntry::kTimestampFieldName);
    tassert(
        6387801, "'ts' field is not a BSON Timestamp", eventTime.getType() == BSONType::timestamp);
    auto commitTxnTs = inputDoc.getField(CommitTransactionOplogObject::kCommitTimestampFieldName);
    tassert(6387802,
            str::stream() << "'" << CommitTransactionOplogObject::kCommitTimestampFieldName
                          << "' field is not a BSON Timestamp",
            commitTxnTs.missing() || commitTxnTs.getType() == BSONType::timestamp);

    MutableDocument doc{inputDoc};
    doc.remove(CommitTransactionOplogObject::kCommitTimestampFieldName);
    doc.setField(repl::OplogEntry::k_idFieldName,
                 Value{Document{{ReshardingDonorOplogId::kClusterTimeFieldName,
                                 commitTxnTs.missing() ? eventTime.getTimestamp()
                                                       : commitTxnTs.getTimestamp()},
                                {ReshardingDonorOplogId::kTsFieldName, eventTime.getTimestamp()}}});
    return doc.freeze();
}
}  // namespace

boost::intrusive_ptr<exec::agg::Stage> documentSourceReshardingAddResumeIdToStageFn(
    const boost::intrusive_ptr<DocumentSource>& source) {
    auto documentSource =
        boost::dynamic_pointer_cast<const DocumentSourceReshardingAddResumeId>(source);

    tassert(10812502, "expected 'DocumentSourceReshardingAddResumeId' type", documentSource);

    return make_intrusive<exec::agg::ReshardingAddResumeIdStage>(documentSource->kStageName,
                                                                 documentSource->getExpCtx());
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(reshardingAddResumeIdStage,
                           DocumentSourceReshardingAddResumeId::id,
                           documentSourceReshardingAddResumeIdToStageFn);

ReshardingAddResumeIdStage::ReshardingAddResumeIdStage(
    std::string_view stageName, const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : Stage(stageName, expCtx) {}

GetNextResult ReshardingAddResumeIdStage::doGetNext() {
    uassert(6387804,
            str::stream() << DocumentSourceReshardingAddResumeId::kStageName
                          << " cannot be executed from router",
            !pExpCtx->getInRouter());

    // Get the next input document.
    auto input = pSource->getNext();
    if (!input.isAdvanced()) {
        return input;
    }

    auto doc = input.releaseDocument();
    return appendId(doc);
}
}  // namespace exec::agg
}  // namespace mongo
