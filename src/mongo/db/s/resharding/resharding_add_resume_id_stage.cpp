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

#include "mongo/db/s/resharding/resharding_add_resume_id_stage.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/s/resharding/document_source_resharding_add_resume_id.h"
#include "mongo/db/s/resharding/donor_oplog_id_gen.h"

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
    StringData stageName, const boost::intrusive_ptr<ExpressionContext>& expCtx)
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
