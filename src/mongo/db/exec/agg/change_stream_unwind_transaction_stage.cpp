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


#include "mongo/db/exec/agg/change_stream_unwind_transaction_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/pipeline/change_stream_helpers.h"
#include "mongo/db/pipeline/document_source_change_stream_unwind_transaction.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/transaction/transaction_history_iterator.h"

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceChangeStreamUnwindTransactionToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* changeStreamUnwindTransactionDS =
        dynamic_cast<DocumentSourceChangeStreamUnwindTransaction*>(documentSource.get());

    tassert(10561305,
            "expected 'DocumentSourceChangeStreamUnwindTransaction' type",
            changeStreamUnwindTransactionDS);

    return make_intrusive<exec::agg::ChangeStreamUnwindTransactionStage>(
        changeStreamUnwindTransactionDS->kStageName,
        changeStreamUnwindTransactionDS->getExpCtx(),
        changeStreamUnwindTransactionDS->_filter.getOwned(),
        changeStreamUnwindTransactionDS->_expression);
}

namespace change_stream_filter {
/**
 * This filter is only used internally, to test the applicability of the EOT event we generate for
 * unprepared transactions.
 */
std::unique_ptr<MatchExpression> buildEndOfTransactionFilter(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    if (!expCtx->getChangeStreamSpec()->getShowExpandedEvents()) {
        return std::make_unique<AlwaysFalseMatchExpression>();
    }

    auto nsRegex = DocumentSourceChangeStream::getNsRegexForChangeStream(expCtx);
    return std::make_unique<RegexMatchExpression>(
        "o2.endOfTransaction"_sd, nsRegex, "" /*options*/);
}
}  // namespace change_stream_filter

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(_internalChangeStreamUnwindTransaction,
                           DocumentSourceChangeStreamUnwindTransaction::id,
                           documentSourceChangeStreamUnwindTransactionToStageFn)

ChangeStreamUnwindTransactionStage::ChangeStreamUnwindTransactionStage(
    StringData stageName,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    BSONObj filter,
    std::shared_ptr<MatchExpression> expression)
    : Stage(stageName, pExpCtx), _filter(std::move(filter)), _expression(std::move(expression)) {}

GetNextResult ChangeStreamUnwindTransactionStage::doGetNext() {
    uassert(5543812,
            str::stream() << DocumentSourceChangeStreamUnwindTransaction::kStageName
                          << " cannot be executed from router",
            !pExpCtx->getInRouter());

    while (true) {
        // If we're unwinding an 'applyOps' from a transaction, check if there are any documents
        // we have stored that can be returned.
        if (_txnIterator) {
            if (auto next = _txnIterator->getNextTransactionOp(pExpCtx->getOperationContext())) {
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

        // If the oplog entry is not part of a transaction, allow it to pass through.
        if (!_isTransactionOplogEntry(doc)) {
            return doc;
        }

        // The only two commands we will see here are an applyOps or a commit, which both mean
        // we need to open a "transaction context" representing a group of updates that all
        // occurred at once as part of a transaction. If we already have a transaction context
        // open, that would mean we are looking at an applyOps or commit nested within an
        // applyOps, which is not allowed in the oplog.
        tassert(5543801, "Transaction iterator not found", !_txnIterator);

        // Once we initialize the transaction iterator, we can loop back to the top in order to
        // call 'getNextTransactionOp' on it. Note that is possible for the transaction iterator
        // to be empty of any relevant operations, meaning that this loop may need to execute
        // multiple times before it encounters a relevant change to return.
        _txnIterator.emplace(pExpCtx, doc, _expression.get());
    }
}


bool ChangeStreamUnwindTransactionStage::_isTransactionOplogEntry(const Document& doc) {
    auto op = doc[repl::OplogEntry::kOpTypeFieldName];
    auto opType = repl::OpType_parse(op.getStringData(), IDLParserContext("ChangeStreamEntry.op"));
    auto commandVal = doc["o"];

    if (opType != repl::OpTypeEnum::kCommand ||
        (commandVal["applyOps"].missing() && commandVal["commitTransaction"].missing())) {
        // We should never see an "abortTransaction" command at this point.
        tassert(5543802,
                str::stream() << "Unexpected op at " << doc["ts"].getTimestamp().toString(),
                opType != repl::OpTypeEnum::kCommand || commandVal["abortTransaction"].missing());
        return false;
    }

    return true;
}

ChangeStreamUnwindTransactionStage::TransactionOpIterator::TransactionOpIterator(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const Document& input,
    const MatchExpression* expression)
    : _mongoProcessInterface(expCtx->getMongoProcessInterface()),
      _expression(expression),
      _endOfTransactionExpression(change_stream_filter::buildEndOfTransactionFilter(expCtx)) {

    Value multiOpTypeValue = input[repl::OplogEntry::kMultiOpTypeFieldName];
    DocumentSourceChangeStream::checkValueTypeOrMissing(
        multiOpTypeValue, repl::OplogEntry::kMultiOpTypeFieldName, BSONType::numberInt);
    const bool applyOpsAppliedSeparately = !multiOpTypeValue.missing() &&
        multiOpTypeValue.getInt() == int(repl::MultiOplogEntryType::kApplyOpsAppliedSeparately);

    // The lsid and txnNumber can be missing in case of batched writes, and are ignored when
    // multiOpType is kApplyOpsAppliedSeparately.  The latter indicates an applyOps that is part of
    // a retryable write and not a multi-document transaction.
    if (!applyOpsAppliedSeparately) {
        Value lsidValue = input["lsid"];
        DocumentSourceChangeStream::checkValueTypeOrMissing(lsidValue, "lsid", BSONType::object);
        _lsid =
            lsidValue.missing() ? boost::none : boost::optional<Document>(lsidValue.getDocument());
        Value txnNumberValue = input["txnNumber"];
        DocumentSourceChangeStream::checkValueTypeOrMissing(
            txnNumberValue, "txnNumber", BSONType::numberLong);
        _txnNumber = txnNumberValue.missing()
            ? boost::none
            : boost::optional<TxnNumber>(txnNumberValue.getLong());
    }
    // We want to parse the OpTime out of this document using the BSON OpTime parser. Instead of
    // converting the entire Document back to BSON, we convert only the fields we need.
    repl::OpTime txnOpTime = repl::OpTime::parse(BSON(repl::OpTime::kTimestampFieldName
                                                      << input[repl::OpTime::kTimestampFieldName]
                                                      << repl::OpTime::kTermFieldName
                                                      << input[repl::OpTime::kTermFieldName]));
    _clusterTime = txnOpTime.getTimestamp();

    Value wallTime = input[repl::OplogEntry::kWallClockTimeFieldName];
    DocumentSourceChangeStream::checkValueType(
        wallTime, repl::OplogEntry::kWallClockTimeFieldName, BSONType::date);
    _wallTime = wallTime.getDate();

    auto commandObj = input["o"].getDocument();
    Value applyOps = commandObj["applyOps"];

    if (!applyOps.missing()) {
        // We found an applyOps that implicitly commits a transaction. We include it in the
        // '_txnOplogEntries' stack of applyOps entries that the change stream should process as
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
        tassert(5543803,
                str::stream() << "Unexpected op at " << input["ts"].getTimestamp().toString(),
                !commandObj["commitTransaction"].missing());

        if (auto commitTimestamp = commandObj["commitTimestamp"]; !commitTimestamp.missing()) {
            // Track commit timestamp of the prepared transaction if it's present in the oplog
            // entry.
            _commitTimestamp = commitTimestamp.getTimestamp();
        }
    }

    // We need endOfTransaction only for unprepared transactions: so this must be an applyOps with
    // set lsid and txnNumber but not a retryable write.
    _needEndOfTransaction = feature_flags::gFeatureFlagEndOfTransactionChangeEvent.isEnabled(
                                serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) &&
        !applyOps.missing() && !applyOpsAppliedSeparately && _lsid.has_value() &&
        _txnNumber.has_value();

    // If there's no previous optime, or if this applyOps is of the kApplyOpsAppliedSeparately
    // multiOptype, we don't need to collect other apply ops operations.
    if (!applyOpsAppliedSeparately &&
        BSONType::object ==
            input[repl::OplogEntry::kPrevWriteOpTimeInTransactionFieldName].getType()) {
        // As with the 'txnOpTime' parsing above, we convert a portion of 'input' back to BSON
        // in order to parse an OpTime, this time from the "prevOpTime" field.
        repl::OpTime prevOpTime = repl::OpTime::parse(
            input[repl::OplogEntry::kPrevWriteOpTimeInTransactionFieldName].getDocument().toBson());
        _collectAllOpTimesFromTransaction(expCtx->getOperationContext(), prevOpTime);
    }

    // Pop the first OpTime off the stack and use it to load the first oplog entry into the
    // '_currentApplyOps' field.
    tassert(5543804, "No transaction oplog entries found", _txnOplogEntries.size() > 0);
    const auto firstTimestamp = _txnOplogEntries.top();
    _txnOplogEntries.pop();

    if (firstTimestamp == txnOpTime) {
        // This transaction consists of only one oplog entry, from which we have already
        // extracted the "applyOps" array, so there is no need to do any more work.
        tassert(5543805,
                str::stream() << "Expected no transaction entries, found "
                              << _txnOplogEntries.size(),
                _txnOplogEntries.size() == 0);
        _currentApplyOps = std::move(applyOps);
    } else {
        // This transaction consists of multiple oplog entries; grab the chronologically first
        // entry and extract its "applyOps" array.
        auto firstApplyOpsEntry =
            _lookUpOplogEntryByOpTime(expCtx->getOperationContext(), firstTimestamp);

        auto bsonOp = firstApplyOpsEntry.getOperationToApply();
        tassert(5543806,
                str::stream() << "Expected 'applyOps' type " << BSONType::array << ", found "
                              << bsonOp["applyOps"].type(),
                BSONType::array == bsonOp["applyOps"].type());
        _currentApplyOps = Value(bsonOp["applyOps"]);
    }

    DocumentSourceChangeStream::checkValueType(_currentApplyOps, "applyOps", BSONType::array);

    // Initialize iterators at the beginning of the transaction.
    _currentApplyOpsIt = _currentApplyOps.getArray().begin();
    _currentApplyOpsTs = firstTimestamp.getTimestamp();
    _currentApplyOpsIndex = 0;
    _txnOpIndex = 0;
}

boost::optional<Document>
ChangeStreamUnwindTransactionStage::TransactionOpIterator::getNextTransactionOp(
    OperationContext* opCtx) {
    while (true) {
        while (_currentApplyOpsIt != _currentApplyOps.getArray().end()) {
            Document doc = (_currentApplyOpsIt++)->getDocument();
            ++_currentApplyOpsIndex;
            ++_txnOpIndex;

            _assertExpectedTransactionEventFormat(doc);

            if (_needEndOfTransaction) {
                _addAffectedNamespaces(doc);
            }
            // If the document is relevant, update it with the required txn fields before returning.
            if (exec::matcher::matchesBSON(_expression, doc.toBson())) {
                return _addRequiredTransactionFields(doc);
            }
        }

        if (_txnOplogEntries.empty()) {
            // There are no more operations in this transaction.
            return _createEndOfTransactionIfNeeded();
        }

        // We've processed all the operations in the previous applyOps entry, but we have a new
        // one to process.
        auto applyOpsEntry = _lookUpOplogEntryByOpTime(opCtx, _txnOplogEntries.top());
        _txnOplogEntries.pop();

        auto bsonOp = applyOpsEntry.getOperationToApply();
        tassert(5543807,
                str::stream() << "Expected 'applyOps' type " << BSONType::array << ", found "
                              << bsonOp["applyOps"].type(),
                BSONType::array == bsonOp["applyOps"].type());

        _currentApplyOps = Value(bsonOp["applyOps"]);
        _currentApplyOpsTs = applyOpsEntry.getTimestamp();
        _currentApplyOpsIt = _currentApplyOps.getArray().begin();
        _currentApplyOpsIndex = 0;
    }
}

void ChangeStreamUnwindTransactionStage::TransactionOpIterator::
    _assertExpectedTransactionEventFormat(const Document& d) const {
    tassert(5543808,
            str::stream() << "Unexpected format for entry within a transaction oplog entry: "
                             "'op' field was type "
                          << typeName(d["op"].getType()),
            d["op"].getType() == BSONType::string);
    tassert(5543809,
            str::stream() << "Unexpected noop entry within a transaction "
                          << redact(d["o"].toString()),
            ValueComparator::kInstance.evaluate(d["op"] != Value("n"_sd)));
}

Document ChangeStreamUnwindTransactionStage::TransactionOpIterator::_addRequiredTransactionFields(
    const Document& doc) const {
    MutableDocument newDoc(doc);
    // The 'getNextTransactionOp' increments the '_txnOpIndex' by 1, to point to the next
    // transaction number. The 'txnOpIndex()' must be called to get the current transaction number.
    newDoc.addField(DocumentSourceChangeStream::kTxnOpIndexField,
                    Value(static_cast<long long>(txnOpIndex())));

    // The 'getNextTransactionOp' updates the '_currentApplyOpsIndex' to point to the next entry in
    // the '_currentApplyOps' array. The 'applyOpsIndex()' method must be called to get the index of
    // the current entry.
    newDoc.addField(DocumentSourceChangeStream::kApplyOpsIndexField,
                    Value(static_cast<long long>(applyOpsIndex())));
    newDoc.addField(DocumentSourceChangeStream::kApplyOpsTsField, Value(applyOpsTs()));

    newDoc.addField(repl::OplogEntry::kTimestampFieldName, Value(_clusterTime));
    newDoc.addField(repl::OplogEntry::kWallClockTimeFieldName, Value(_wallTime));

    newDoc.addField(DocumentSourceChangeStream::kCommitTimestampField,
                    _commitTimestamp ? Value(*_commitTimestamp) : Value());

    newDoc.addField(repl::OplogEntry::kSessionIdFieldName, _lsid ? Value(*_lsid) : Value());
    newDoc.addField(repl::OplogEntry::kTxnNumberFieldName,
                    _txnNumber ? Value(static_cast<long long>(*_txnNumber)) : Value());

    return newDoc.freeze();
}

repl::OplogEntry
ChangeStreamUnwindTransactionStage::TransactionOpIterator::_lookUpOplogEntryByOpTime(
    OperationContext* opCtx, repl::OpTime lookupTime) const {
    tassert(5543811, "Cannot look up transaction entry with null op time", !lookupTime.isNull());

    std::unique_ptr<TransactionHistoryIteratorBase> iterator(
        _mongoProcessInterface->createTransactionHistoryIterator(lookupTime));

    try {
        return iterator->next(opCtx);
    } catch (ExceptionFor<ErrorCodes::IncompleteTransactionHistory>& ex) {
        ex.addContext(
            "Oplog no longer has history necessary for $changeStream to observe operations "
            "from a "
            "committed transaction.");
        uasserted(ErrorCodes::ChangeStreamHistoryLost, ex.reason());
    }
}

void ChangeStreamUnwindTransactionStage::TransactionOpIterator::_collectAllOpTimesFromTransaction(
    OperationContext* opCtx, repl::OpTime firstOpTime) {
    std::unique_ptr<TransactionHistoryIteratorBase> iterator(
        _mongoProcessInterface->createTransactionHistoryIterator(firstOpTime));

    try {
        while (iterator->hasNext()) {
            _txnOplogEntries.push(iterator->nextOpTime(opCtx));
        }
    } catch (ExceptionFor<ErrorCodes::IncompleteTransactionHistory>& ex) {
        ex.addContext(
            "Oplog no longer has history necessary for $changeStream to observe operations "
            "from a "
            "committed transaction.");
        uasserted(ErrorCodes::ChangeStreamHistoryLost, ex.reason());
    }
}

void ChangeStreamUnwindTransactionStage::TransactionOpIterator::_addAffectedNamespaces(
    const Document& doc) {
    // TODO SERVER-78670:  move namespace extraction to change_stream_helpers.cpp and make generic
    const auto tid = [&]() -> boost::optional<TenantId> {
        if (!gMultitenancySupport) {
            return boost::none;
        }
        const auto tidField = doc["tid"];
        return !tidField.missing() ? boost::make_optional(TenantId(tidField.getOid()))
                                   : boost::none;
    }();

    const auto dbCmdNs = NamespaceStringUtil::deserialize(
        tid, doc["ns"].getStringData(), SerializationContext::stateDefault());
    if (doc["op"].getStringData() != "c") {
        _affectedNamespaces.insert(dbCmdNs);
        return;
    }

    constexpr std::array<StringData, 2> kCollectionField = {"create"_sd, "createIndexes"_sd};
    const Document& object = doc["o"].getDocument();
    for (const auto& fieldName : kCollectionField) {
        const auto field = object[fieldName];
        if (field.getType() == BSONType::string) {
            _affectedNamespaces.insert(
                NamespaceStringUtil::deserialize(dbCmdNs.dbName(), field.getStringData()));
            return;
        }
    }

    tasserted(7694300,
              str::stream() << "Unexpected op in applyOps at " << _currentApplyOpsTs << " index "
                            << _currentApplyOpsIndex << " object " << redact(object.toString()));
}

boost::optional<Document>
ChangeStreamUnwindTransactionStage::TransactionOpIterator::_createEndOfTransactionIfNeeded() {
    if (!_needEndOfTransaction || _endOfTransactionReturned) {
        return boost::none;
    }
    ++_currentApplyOpsIndex;
    ++_txnOpIndex;
    _endOfTransactionReturned = true;

    std::vector<NamespaceString> namespaces{_affectedNamespaces.begin(), _affectedNamespaces.end()};
    auto lsid = LogicalSessionId::parse(_lsid->toBson(), IDLParserContext("LogicalSessionId"));
    repl::MutableOplogEntry oplogEntry = change_stream::createEndOfTransactionOplogEntry(
        lsid, *_txnNumber, namespaces, _clusterTime, _wallTime);

    MutableDocument newDoc(Document(oplogEntry.toBSON()));
    newDoc.addField(DocumentSourceChangeStream::kTxnOpIndexField,
                    Value(static_cast<long long>(txnOpIndex())));
    newDoc.addField(DocumentSourceChangeStream::kApplyOpsIndexField,
                    Value(static_cast<long long>(applyOpsIndex())));
    newDoc.addField(DocumentSourceChangeStream::kApplyOpsTsField, Value(applyOpsTs()));

    Document endOfTransaction = newDoc.freeze();
    return {
        exec::matcher::matchesBSON(_endOfTransactionExpression.get(), endOfTransaction.toBson()),
        endOfTransaction};
}

}  // namespace exec::agg
}  // namespace mongo
