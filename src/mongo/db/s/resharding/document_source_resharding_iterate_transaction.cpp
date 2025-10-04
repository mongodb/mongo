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


#include "mongo/db/s/resharding/document_source_resharding_iterate_transaction.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

REGISTER_INTERNAL_DOCUMENT_SOURCE(_internalReshardingIterateTransaction,
                                  LiteParsedDocumentSourceInternal::parse,
                                  DocumentSourceReshardingIterateTransaction::createFromBson,
                                  true);
ALLOCATE_DOCUMENT_SOURCE_ID(_internalReshardingIterateTransaction,
                            DocumentSourceReshardingIterateTransaction::id)

boost::intrusive_ptr<DocumentSourceReshardingIterateTransaction>
DocumentSourceReshardingIterateTransaction::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, bool includeCommitTransactionTimestamp) {
    return new DocumentSourceReshardingIterateTransaction(expCtx,
                                                          includeCommitTransactionTimestamp);
}

boost::intrusive_ptr<DocumentSourceReshardingIterateTransaction>
DocumentSourceReshardingIterateTransaction::createFromBson(
    const BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(5730300,
            str::stream() << "the '" << kStageName << "' spec must be an object",
            elem.type() == BSONType::object);

    bool _includeCommitTransactionTimestamp = false;
    for (auto&& subElem : elem.Obj()) {
        if (subElem.fieldNameStringData() == kIncludeCommitTransactionTimestampFieldName) {
            uassert(6387808,
                    str::stream() << "expected a boolean for the "
                                  << kIncludeCommitTransactionTimestampFieldName << " option to "
                                  << kStageName << " stage, got " << typeName(subElem.type()),
                    subElem.type() == BSONType::boolean);
            _includeCommitTransactionTimestamp = subElem.Bool();
        } else {
            uasserted(6387809,
                      str::stream() << "unrecognized option to " << kStageName
                                    << " stage: " << subElem.fieldNameStringData());
        }
    }

    return new DocumentSourceReshardingIterateTransaction(expCtx,
                                                          _includeCommitTransactionTimestamp);
}

DocumentSourceReshardingIterateTransaction::DocumentSourceReshardingIterateTransaction(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, bool includeCommitTransactionTimestamp)
    : DocumentSource(kStageName, expCtx),
      _includeCommitTransactionTimestamp(includeCommitTransactionTimestamp) {}

StageConstraints DocumentSourceReshardingIterateTransaction::constraints(
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

Value DocumentSourceReshardingIterateTransaction::serialize(
    const SerializationOptions& opts) const {
    return Value(
        Document{{kStageName,
                  Value(Document{{kIncludeCommitTransactionTimestampFieldName,
                                  _includeCommitTransactionTimestamp ? Value(true) : Value()}})}});
}

DepsTracker::State DocumentSourceReshardingIterateTransaction::getDependencies(
    DepsTracker* deps) const {
    deps->fields.insert(std::string{repl::OplogEntry::kOpTypeFieldName});
    deps->fields.insert(std::string{repl::OplogEntry::kTimestampFieldName});
    deps->fields.insert(std::string{repl::OplogEntry::kObjectFieldName});
    deps->fields.insert(std::string{repl::OplogEntry::kPrevWriteOpTimeInTransactionFieldName});
    deps->fields.insert(std::string{repl::OplogEntry::kSessionIdFieldName});
    deps->fields.insert(std::string{repl::OplogEntry::kTermFieldName});
    deps->fields.insert(std::string{repl::OplogEntry::kTxnNumberFieldName});

    return DepsTracker::State::SEE_NEXT;
}

DocumentSource::GetModPathsReturn DocumentSourceReshardingIterateTransaction::getModifiedPaths()
    const {
    return {DocumentSource::GetModPathsReturn::Type::kAllPaths, OrderedPathSet{}, {}};
}
}  // namespace mongo
