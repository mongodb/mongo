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

REGISTER_INTERNAL_DOCUMENT_SOURCE(_addReshardingResumeId,
                                  LiteParsedDocumentSourceInternal::parse,
                                  DocumentSourceReshardingAddResumeId::createFromBson,
                                  true);
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
                                 HostTypeRequirement::kAnyShard,
                                 DiskUseRequirement::kNoDiskUse,
                                 FacetRequirement::kNotAllowed,
                                 TransactionRequirement::kNotAllowed,
                                 LookupRequirement::kNotAllowed,
                                 UnionRequirement::kNotAllowed,
                                 ChangeStreamRequirement::kDenylist);
    constraints.consumesLogicalCollectionData = false;
    return constraints;
}

Value DocumentSourceReshardingAddResumeId::serialize(const SerializationOptions& opts) const {
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
