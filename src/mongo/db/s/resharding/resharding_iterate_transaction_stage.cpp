// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/resharding/resharding_iterate_transaction_stage.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/s/resharding/document_source_resharding_iterate_transaction.h"
#include "mongo/db/transaction/transaction_history_iterator.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <string_view>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {
// Generates the _id field for the resharding event. For an event within a transaction, this will be
// {clusterTime: <transaction commit time>, ts: <applyOps optime>}. For all other events, this will
// be {clusterTime: <optime>, ts: <optime>}.
Document appendReshardingId(Document inputDoc, boost::optional<Timestamp> txnCommitTime) {
    auto eventTime = inputDoc.getField(repl::OplogEntry::kTimestampFieldName);
    tassert(
        5730308, "'ts' field is not a BSON Timestamp", eventTime.getType() == BSONType::timestamp);
    MutableDocument doc{inputDoc};
    doc.setField("_id",
                 Value{Document{{"clusterTime", txnCommitTime.value_or(eventTime.getTimestamp())},
                                {"ts", eventTime.getTimestamp()}}});
    return doc.freeze();
}
}  // namespace

boost::intrusive_ptr<exec::agg::Stage> documentSourceReshardingIterateTransactionToStageFn(
    const boost::intrusive_ptr<DocumentSource>& source) {
    auto documentSource =
        boost::dynamic_pointer_cast<const DocumentSourceReshardingIterateTransaction>(source);

    tassert(10812501, "expected 'DocumentSourceReshardingIterateTransaction' type", documentSource);

    return make_intrusive<exec::agg::ReshardingIterateTransactionStage>(
        documentSource->kStageName,
        documentSource->getExpCtx(),
        documentSource->getIncludeCommitTransactionTimestamp());
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(reshardingIterateTransactionStage,
                           DocumentSourceReshardingIterateTransaction::id,
                           documentSourceReshardingIterateTransactionToStageFn);

ReshardingIterateTransactionStage::ReshardingIterateTransactionStage(
    std::string_view stageName,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    bool includeCommitTransactionTimestamp)
    : Stage(stageName, expCtx),
      _includeCommitTransactionTimestamp(includeCommitTransactionTimestamp),
      _stageName(stageName) {}

GetNextResult ReshardingIterateTransactionStage::doGetNext() {
    uassert(5730301,
            str::stream() << _stageName << " cannot be executed from router",
            !pExpCtx->getInRouter());

    while (true) {
        // If we're unwinding an 'applyOps' from a transaction, check if there are any documents
        // we have stored that can be returned.
        if (_txnIterator) {
            if (auto next = _txnIterator->getNextApplyOpsTxnEntry(pExpCtx->getOperationContext())) {
                return std::move(*next);
            }

            _txnIterator = boost::none;
        }

        // Get the next input document.
        auto input = pSource->getNext();
        if (!input.isAdvanced()) {
            return input;
        }

        auto doc = input.releaseDocument();

        // If the oplog entry is not part of a transaction, allow it to pass through after appending
        // a resharding _id to the document.
        if (!_isTransactionOplogEntry(doc)) {
            if (_includeCommitTransactionTimestamp) {
                return doc;
            }
            return appendReshardingId(std::move(doc), boost::none);
        }

        // The only two commands we will see here are an applyOps or a commit, which both mean
        // we need to open a "transaction context" representing a group of updates that all
        // occurred at once as part of a transaction. If we already have a transaction context
        // open, that would mean we are looking at an applyOps or commit nested within an
        // applyOps, which is not allowed in the oplog.
        tassert(5730302, "Attempted to open embedded txn iterator", !_txnIterator);

        // Once we initialize the transaction iterator, we can loop back to the top in order to
        // call 'getNextTransactionOp' on it. Note that is possible for the transaction iterator
        // to be empty of any relevant operations, meaning that this loop may need to execute
        // multiple times before it encounters a relevant change to return.
        _txnIterator.emplace(pExpCtx->getOperationContext(),
                             pExpCtx->getMongoProcessInterface(),
                             doc,
                             _includeCommitTransactionTimestamp);
    }
}

bool ReshardingIterateTransactionStage::_isTransactionOplogEntry(const Document& doc) {
    auto op = doc[repl::OplogEntry::kOpTypeFieldName];
    auto ctx = IDLParserContext("ReshardingEntry.op");
    auto opType = idl::deserialize<repl::OpTypeEnum>(op.getStringData(), ctx);
    auto commandVal = doc["o"];
    repl::MultiOplogEntryType multiOpType = repl::MultiOplogEntryType::kLegacyMultiOpType;
    if (doc["multiOpType"].getType() == BSONType::numberInt)
        multiOpType = idl::deserialize<repl::MultiOplogEntryType>(doc["multiOpType"].getInt(), ctx);

    if (opType != repl::OpTypeEnum::kCommand || doc["txnNumber"].missing() ||
        multiOpType == repl::MultiOplogEntryType::kApplyOpsAppliedSeparately ||
        (commandVal["applyOps"].missing() && commandVal["commitTransaction"].missing())) {
        return false;
    }

    return true;
}

ReshardingIterateTransactionStage::TransactionOpIterator::TransactionOpIterator(
    OperationContext* opCtx,
    std::shared_ptr<MongoProcessInterface> mongoProcessInterface,
    const Document& input,
    bool includeCommitTransactionTimestamp)
    : _mongoProcessInterface(mongoProcessInterface),
      _includeCommitTransactionTimestamp(includeCommitTransactionTimestamp) {
    Value lsidValue = input["lsid"];
    tassert(5730306, "oplog entry with non-object lsid", lsidValue.getType() == BSONType::object);
    _lsid = lsidValue.getDocument();

    Value txnNumberValue = input["txnNumber"];
    tassert(5730307,
            "oplog entry with non-long txnNumber",
            txnNumberValue.getType() == BSONType::numberLong);
    _txnNumber = txnNumberValue.getLong();

    // We want to parse the OpTime out of this document using the BSON OpTime parser. Instead of
    // converting the entire Document back to BSON, we convert only the fields we need.
    repl::OpTime txnOpTime = repl::OpTime::parse(BSON(repl::OpTime::kTimestampFieldName
                                                      << input[repl::OpTime::kTimestampFieldName]
                                                      << repl::OpTime::kTermFieldName
                                                      << input[repl::OpTime::kTermFieldName]));
    _clusterTime = txnOpTime.getTimestamp();

    auto commandObj = input["o"].getDocument();
    Value applyOps = commandObj["applyOps"];

    if (!applyOps.missing()) {
        // We found an applyOps that implicitly commits a transaction. We include it in the
        // '_txnOplogEntries' stack of applyOps entries that the iterator should process as
        // part of this transaction. There may be additional applyOps entries linked through the
        // 'prevOpTime' field, which will also get added to '_txnOplogEntries' later in this
        // function. Note that this style of transaction does not have a 'commitTransaction'
        // command.
        _txnOplogEntries.push(txnOpTime);
    } else {
        // This must be a "commitTransaction" command, which commits a prepared transaction.
        // This style of transaction does not have an applyOps entry that implicitly commits it,
        // as in the previous case. We're going to iterate through the other oplog entries in
        // the transaction, but this entry does not have any updates in it, so we do not include
        // it in the '_txnOplogEntries' stack.
        tassert(5730303,
                str::stream() << "Unexpected op at " << input["ts"].getTimestamp().toString(),
                !commandObj["commitTransaction"].missing());
    }

    if (BSONType::object ==
        input[repl::OplogEntry::kPrevWriteOpTimeInTransactionFieldName].getType()) {
        // As with the 'txnOpTime' parsing above, we convert a portion of 'input' back to BSON
        // in order to parse an OpTime, this time from the "prevOpTime" field.
        repl::OpTime prevOpTime = repl::OpTime::parse(
            input[repl::OplogEntry::kPrevWriteOpTimeInTransactionFieldName].getDocument().toBson());
        _collectAllOpTimesFromTransaction(opCtx, prevOpTime);
    }

    // By this stage, we should always have at least one transaction entry optime in the stack.
    tassert(5730304, "No transaction oplog entries found", _txnOplogEntries.size() > 0);
}

boost::optional<Document>
ReshardingIterateTransactionStage::TransactionOpIterator::getNextApplyOpsTxnEntry(
    OperationContext* opCtx) {
    while (true) {
        if (_txnOplogEntries.empty()) {
            // There are no more operations in this transaction.
            return boost::none;
        }

        // Pop the optime of the next applyOps entry off the stack and retrieve it.
        auto applyOpsEntry = _lookUpOplogEntryByOpTime(opCtx, _txnOplogEntries.top());
        _txnOplogEntries.pop();

        if (_includeCommitTransactionTimestamp) {
            // Attach the transaction commit timestamp so it can be used by a downstream stage to
            // generate the _id for the document.
            MutableDocument doc{Document{applyOpsEntry.getEntry().toBSON()}};
            doc.addField(CommitTransactionOplogObject::kCommitTimestampFieldName,
                         Value{_clusterTime});
            return doc.freeze();
        }
        // Generate the _id for the document as {clusterTime: txnCommitTime, ts: applyOpsTs}.
        return appendReshardingId(Document{applyOpsEntry.getEntry().toBSON()}, _clusterTime);
    }
}

repl::OplogEntry
ReshardingIterateTransactionStage::TransactionOpIterator::_lookUpOplogEntryByOpTime(
    OperationContext* opCtx, repl::OpTime lookupTime) const {
    tassert(5730305, "Cannot look up transaction entry with null op time", !lookupTime.isNull());

    std::unique_ptr<TransactionHistoryIteratorBase> iterator(
        _mongoProcessInterface->createTransactionHistoryIterator(lookupTime));

    return iterator->next(opCtx);
}

void ReshardingIterateTransactionStage::TransactionOpIterator::_collectAllOpTimesFromTransaction(
    OperationContext* opCtx, repl::OpTime firstOpTime) {
    std::unique_ptr<TransactionHistoryIteratorBase> iterator(
        _mongoProcessInterface->createTransactionHistoryIterator(firstOpTime));

    while (iterator->hasNext()) {
        _txnOplogEntries.push(iterator->nextOpTime(opCtx));
    }
}
}  // namespace exec::agg
}  // namespace mongo
