// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


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

REGISTER_INTERNAL_LITE_PARSED_DOCUMENT_SOURCE(_internalReshardingIterateTransaction,
                                              ReshardingIterateTransactionLiteParsed::parse);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(_internalReshardingIterateTransaction,
                                                   DocumentSourceReshardingIterateTransaction,
                                                   ReshardingIterateTransactionStageParams);

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
                                 HostTypeRequirement::kTargetedShards,
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
    const query_shape::SerializationOptions& opts) const {
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
