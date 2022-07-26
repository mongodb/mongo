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


#include "mongo/platform/basic.h"

#include "mongo/db/s/resharding/document_source_resharding_iterate_transaction.h"

#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/transaction_history_iterator.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {
// Generates the _id field for the resharding event. For an event within a transaction, this will be
// {clusterTime: <transaction commit time>, ts: <applyOps optime>}. For all other events, this will
// be {clusterTime: <optime>, ts: <optime>}.
Document appendReshardingId(Document inputDoc, boost::optional<Timestamp> txnCommitTime) {
    auto eventTime = inputDoc.getField(repl::OplogEntry::kTimestampFieldName);
    tassert(5730308,
            "'ts' field is not a BSON Timestamp",
            eventTime.getType() == BSONType::bsonTimestamp);
    MutableDocument doc{inputDoc};
    doc.setField("_id",
                 Value{Document{{"clusterTime", txnCommitTime.value_or(eventTime.getTimestamp())},
                                {"ts", eventTime.getTimestamp()}}});
    return doc.freeze();
}
}  // namespace

REGISTER_INTERNAL_DOCUMENT_SOURCE(_internalReshardingIterateTransaction,
                                  LiteParsedDocumentSourceDefault::parse,
                                  DocumentSourceReshardingIterateTransaction::createFromBson,
                                  true);

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
            elem.type() == BSONType::Object);

    bool _includeCommitTransactionTimestamp = false;
    for (auto&& subElem : elem.Obj()) {
        if (subElem.fieldNameStringData() == kIncludeCommitTransactionTimestampFieldName) {
            uassert(6387808,
                    str::stream() << "expected a boolean for the "
                                  << kIncludeCommitTransactionTimestampFieldName << " option to "
                                  << kStageName << " stage, got " << typeName(subElem.type()),
                    subElem.type() == Bool);
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

Value DocumentSourceReshardingIterateTransaction::serialize(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    return Value(
        Document{{kStageName,
                  Value(Document{{kIncludeCommitTransactionTimestampFieldName,
                                  _includeCommitTransactionTimestamp ? Value(true) : Value()}})}});
}

DepsTracker::State DocumentSourceReshardingIterateTransaction::getDependencies(
    DepsTracker* deps) const {
    deps->fields.insert(repl::OplogEntry::kOpTypeFieldName.toString());
    deps->fields.insert(repl::OplogEntry::kTimestampFieldName.toString());
    deps->fields.insert(repl::OplogEntry::kObjectFieldName.toString());
    deps->fields.insert(repl::OplogEntry::kPrevWriteOpTimeInTransactionFieldName.toString());
    deps->fields.insert(repl::OplogEntry::kSessionIdFieldName.toString());
    deps->fields.insert(repl::OplogEntry::kTermFieldName.toString());
    deps->fields.insert(repl::OplogEntry::kTxnNumberFieldName.toString());

    return DepsTracker::State::SEE_NEXT;
}

DocumentSource::GetModPathsReturn DocumentSourceReshardingIterateTransaction::getModifiedPaths()
    const {
    return {DocumentSource::GetModPathsReturn::Type::kAllPaths, OrderedPathSet{}, {}};
}

DocumentSource::GetNextResult DocumentSourceReshardingIterateTransaction::doGetNext() {
    uassert(5730301,
            str::stream() << kStageName << " cannot be executed from mongos",
            !pExpCtx->inMongos);

    while (true) {
        // If we're unwinding an 'applyOps' from a transaction, check if there are any documents
        // we have stored that can be returned.
        if (_txnIterator) {
            if (auto next = _txnIterator->getNextApplyOpsTxnEntry(pExpCtx->opCtx)) {
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
        _txnIterator.emplace(pExpCtx->opCtx,
                             pExpCtx->mongoProcessInterface,
                             doc,
                             _includeCommitTransactionTimestamp);
    }
}

bool DocumentSourceReshardingIterateTransaction::_isTransactionOplogEntry(const Document& doc) {
    auto op = doc[repl::OplogEntry::kOpTypeFieldName];
    auto opType = repl::OpType_parse(IDLParserContext("ReshardingEntry.op"), op.getStringData());
    auto commandVal = doc["o"];

    if (opType != repl::OpTypeEnum::kCommand || doc["txnNumber"].missing() ||
        (commandVal["applyOps"].missing() && commandVal["commitTransaction"].missing())) {
        return false;
    }

    return true;
}

DocumentSourceReshardingIterateTransaction::TransactionOpIterator::TransactionOpIterator(
    OperationContext* opCtx,
    std::shared_ptr<MongoProcessInterface> mongoProcessInterface,
    const Document& input,
    bool includeCommitTransactionTimestamp)
    : _mongoProcessInterface(mongoProcessInterface),
      _includeCommitTransactionTimestamp(includeCommitTransactionTimestamp) {
    Value lsidValue = input["lsid"];
    tassert(5730306, "oplog entry with non-object lsid", lsidValue.getType() == BSONType::Object);
    _lsid = lsidValue.getDocument();

    Value txnNumberValue = input["txnNumber"];
    tassert(5730307,
            "oplog entry with non-long txnNumber",
            txnNumberValue.getType() == BSONType::NumberLong);
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

    if (BSONType::Object ==
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
DocumentSourceReshardingIterateTransaction::TransactionOpIterator::getNextApplyOpsTxnEntry(
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
DocumentSourceReshardingIterateTransaction::TransactionOpIterator::_lookUpOplogEntryByOpTime(
    OperationContext* opCtx, repl::OpTime lookupTime) const {
    tassert(5730305, "Cannot look up transaction entry with null op time", !lookupTime.isNull());

    std::unique_ptr<TransactionHistoryIteratorBase> iterator(
        _mongoProcessInterface->createTransactionHistoryIterator(lookupTime));

    return iterator->next(opCtx);
}

void DocumentSourceReshardingIterateTransaction::TransactionOpIterator::
    _collectAllOpTimesFromTransaction(OperationContext* opCtx, repl::OpTime firstOpTime) {
    std::unique_ptr<TransactionHistoryIteratorBase> iterator(
        _mongoProcessInterface->createTransactionHistoryIterator(firstOpTime));

    while (iterator->hasNext()) {
        _txnOplogEntries.push(iterator->nextOpTime(opCtx));
    }
}
}  // namespace mongo
