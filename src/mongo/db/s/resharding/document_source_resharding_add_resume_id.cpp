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

#include "mongo/platform/basic.h"

#include "mongo/db/commands/txn_cmds_gen.h"
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
    tassert(6387801,
            "'ts' field is not a BSON Timestamp",
            eventTime.getType() == BSONType::bsonTimestamp);
    auto commitTxnTs = inputDoc.getField(CommitTransactionOplogObject::kCommitTimestampFieldName);
    tassert(6387802,
            str::stream() << "'" << CommitTransactionOplogObject::kCommitTimestampFieldName
                          << "' field is not a BSON Timestamp",
            commitTxnTs.missing() || commitTxnTs.getType() == BSONType::bsonTimestamp);

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

REGISTER_INTERNAL_DOCUMENT_SOURCE(_addReshardingResumeId,
                                  LiteParsedDocumentSourceDefault::parse,
                                  DocumentSourceReshardingAddResumeId::createFromBson,
                                  true);

boost::intrusive_ptr<DocumentSourceReshardingAddResumeId>
DocumentSourceReshardingAddResumeId::create(const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return new DocumentSourceReshardingAddResumeId(expCtx);
}

boost::intrusive_ptr<DocumentSourceReshardingAddResumeId>
DocumentSourceReshardingAddResumeId::createFromBson(
    const BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(6387803,
            str::stream() << "the '" << kStageName << "' spec must be an empty object",
            elem.type() == BSONType::Object && elem.Obj().isEmpty());
    return new DocumentSourceReshardingAddResumeId(expCtx);
}

DocumentSourceReshardingAddResumeId::DocumentSourceReshardingAddResumeId(
    const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSource(kStageName, expCtx) {}

StageConstraints DocumentSourceReshardingAddResumeId::constraints(
    Pipeline::SplitState pipeState) const {
    return StageConstraints(StreamType::kStreaming,
                            PositionRequirement::kNone,
                            HostTypeRequirement::kAnyShard,
                            DiskUseRequirement::kNoDiskUse,
                            FacetRequirement::kNotAllowed,
                            TransactionRequirement::kNotAllowed,
                            LookupRequirement::kNotAllowed,
                            UnionRequirement::kNotAllowed,
                            ChangeStreamRequirement::kDenylist);
}

Value DocumentSourceReshardingAddResumeId::serialize(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    return Value(Document{{kStageName, Value(Document{})}});
}

DepsTracker::State DocumentSourceReshardingAddResumeId::getDependencies(DepsTracker* deps) const {
    deps->fields.insert(repl::OplogEntry::kTimestampFieldName.toString());
    deps->fields.insert(CommitTransactionOplogObject::kCommitTimestampFieldName.toString());
    return DepsTracker::State::SEE_NEXT;
}

DocumentSource::GetModPathsReturn DocumentSourceReshardingAddResumeId::getModifiedPaths() const {
    return {GetModPathsReturn::Type::kFiniteSet,
            {repl::OplogEntry::k_idFieldName.toString(),
             CommitTransactionOplogObject::kCommitTimestampFieldName.toString()},
            {}};
}

DocumentSource::GetNextResult DocumentSourceReshardingAddResumeId::doGetNext() {
    uassert(6387804,
            str::stream() << kStageName << " cannot be executed from mongos",
            !pExpCtx->inMongos);

    // Get the next input document.
    auto input = pSource->getNext();
    if (!input.isAdvanced()) {
        return input;
    }

    auto doc = input.releaseDocument();
    return appendId(doc);
}
}  // namespace mongo
