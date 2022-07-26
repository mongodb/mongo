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

#include "mongo/db/pipeline/document_source_change_stream_unwind_transaction.h"

#include "mongo/db/pipeline/change_stream_filter_helpers.h"
#include "mongo/db/pipeline/change_stream_rewrite_helpers.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/transaction_history_iterator.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

REGISTER_INTERNAL_DOCUMENT_SOURCE(_internalChangeStreamUnwindTransaction,
                                  LiteParsedDocumentSourceChangeStreamInternal::parse,
                                  DocumentSourceChangeStreamUnwindTransaction::createFromBson,
                                  true);

namespace change_stream_filter {
/**
 * Build a filter, similar to the optimized oplog filter, designed to reject individual transaction
 * entries that we know would eventually get rejected by the 'userMatch' filter if they continued
 * through the rest of the pipeline. We must also adjust the filter slightly for user rewrites, as
 * events within a transaction do not have certain fields that are common to other oplog entries.
 *
 * NB: The new filter may contain references to strings in the BSONObj that 'userMatch' originated
 * from. Callers that keep the new filter long-term should serialize and re-parse it to guard
 * against the possibility of stale string references.
 */
std::unique_ptr<MatchExpression> buildUnwindTransactionFilter(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, const MatchExpression* userMatch) {
    // The transaction unwind filter is the same as the operation filter applied to the oplog. This
    // includes a namespace filter, which ensures that it will discard all documents that would be
    // filtered out by the default 'ns' filter this stage gets initialized with.
    auto unwindFilter = std::make_unique<AndMatchExpression>(buildOperationFilter(expCtx, nullptr));

    // To correctly handle filtering out entries of direct write operations on orphaned documents,
    // we include a filter for "fromMigrate" flagged operations, unless "fromMigrate" events are
    // explicitly requested in the spec.
    if (!expCtx->changeStreamSpec->getShowMigrationEvents()) {
        unwindFilter->add(buildNotFromMigrateFilter(expCtx, userMatch));
    }

    // Attempt to rewrite the user's filter and combine it with the standard operation filter. We do
    // this separately because we need to exclude certain fields from the user's filters. Unwound
    // transaction events do not have these fields until we populate them from the commitTransaction
    // event. We already applied these predicates during the oplog scan, so we know that they match.
    static const std::set<std::string> excludedFields = {"clusterTime", "lsid", "txnNumber"};
    if (auto rewrittenMatch =
            change_stream_rewrite::rewriteFilterForFields(expCtx, userMatch, {}, excludedFields)) {
        unwindFilter->add(std::move(rewrittenMatch));
    }
    return MatchExpression::optimize(std::move(unwindFilter));
}
}  // namespace change_stream_filter

boost::intrusive_ptr<DocumentSourceChangeStreamUnwindTransaction>
DocumentSourceChangeStreamUnwindTransaction::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    std::unique_ptr<MatchExpression> matchExpr =
        change_stream_filter::buildUnwindTransactionFilter(expCtx, nullptr);
    return new DocumentSourceChangeStreamUnwindTransaction(matchExpr->serialize(), expCtx);
}

boost::intrusive_ptr<DocumentSourceChangeStreamUnwindTransaction>
DocumentSourceChangeStreamUnwindTransaction::createFromBson(
    const BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(5467605,
            str::stream() << "the '" << kStageName << "' stage spec must be an object",
            elem.type() == BSONType::Object);
    auto parsedSpec = DocumentSourceChangeStreamUnwindTransactionSpec::parse(
        IDLParserContext("DocumentSourceChangeStreamUnwindTransactionSpec"), elem.Obj());
    return new DocumentSourceChangeStreamUnwindTransaction(parsedSpec.getFilter(), expCtx);
}

DocumentSourceChangeStreamUnwindTransaction::DocumentSourceChangeStreamUnwindTransaction(
    const BSONObj& filter, const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSource(kStageName, expCtx) {
    rebuild(filter);
}

void DocumentSourceChangeStreamUnwindTransaction::rebuild(BSONObj filter) {
    _filter = filter.getOwned();
    _expression = MatchExpressionParser::parseAndNormalize(filter, pExpCtx);
}

StageConstraints DocumentSourceChangeStreamUnwindTransaction::constraints(
    Pipeline::SplitState pipeState) const {
    return StageConstraints(StreamType::kStreaming,
                            PositionRequirement::kNone,
                            HostTypeRequirement::kNone,
                            DiskUseRequirement::kNoDiskUse,
                            FacetRequirement::kNotAllowed,
                            TransactionRequirement::kNotAllowed,
                            LookupRequirement::kNotAllowed,
                            UnionRequirement::kNotAllowed,
                            ChangeStreamRequirement::kChangeStreamStage);
}

Value DocumentSourceChangeStreamUnwindTransaction::serialize(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    tassert(5467604, "expression has not been initialized", _expression);

    if (explain) {
        return Value(
            DOC(DocumentSourceChangeStream::kStageName << DOC("stage"
                                                              << "internalUnwindTransaction"_sd
                                                              << "filter" << _filter)));
    }

    DocumentSourceChangeStreamUnwindTransactionSpec spec(_filter);
    return Value(Document{{kStageName, Value(spec.toBSON())}});
}

DepsTracker::State DocumentSourceChangeStreamUnwindTransaction::getDependencies(
    DepsTracker* deps) const {
    deps->fields.insert(repl::OplogEntry::kOpTypeFieldName.toString());
    deps->fields.insert(repl::OplogEntry::kTimestampFieldName.toString());
    deps->fields.insert(repl::OplogEntry::kObjectFieldName.toString());
    deps->fields.insert(repl::OplogEntry::kPrevWriteOpTimeInTransactionFieldName.toString());
    deps->fields.insert(repl::OplogEntry::kSessionIdFieldName.toString());
    deps->fields.insert(repl::OplogEntry::kTermFieldName.toString());
    deps->fields.insert(repl::OplogEntry::kTxnNumberFieldName.toString());
    deps->fields.insert(repl::OplogEntry::kWallClockTimeFieldName.toString());

    return DepsTracker::State::SEE_NEXT;
}

DocumentSource::GetModPathsReturn DocumentSourceChangeStreamUnwindTransaction::getModifiedPaths()
    const {
    return {DocumentSource::GetModPathsReturn::Type::kAllPaths, OrderedPathSet{}, {}};
}

DocumentSource::GetNextResult DocumentSourceChangeStreamUnwindTransaction::doGetNext() {
    uassert(5543812,
            str::stream() << kStageName << " cannot be executed from mongos",
            !pExpCtx->inMongos);

    while (true) {
        // If we're unwinding an 'applyOps' from a transaction, check if there are any documents
        // we have stored that can be returned.
        if (_txnIterator) {
            if (auto next = _txnIterator->getNextTransactionOp(pExpCtx->opCtx)) {
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
        _txnIterator.emplace(
            pExpCtx->opCtx, pExpCtx->mongoProcessInterface, doc, _expression.get());
    }
}

bool DocumentSourceChangeStreamUnwindTransaction::_isTransactionOplogEntry(const Document& doc) {
    auto op = doc[repl::OplogEntry::kOpTypeFieldName];
    auto opType = repl::OpType_parse(IDLParserContext("ChangeStreamEntry.op"), op.getStringData());
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

DocumentSourceChangeStreamUnwindTransaction::TransactionOpIterator::TransactionOpIterator(
    OperationContext* opCtx,
    std::shared_ptr<MongoProcessInterface> mongoProcessInterface,
    const Document& input,
    const MatchExpression* expression)
    : _mongoProcessInterface(mongoProcessInterface), _expression(expression) {

    // The lsid and txnNumber can be missing in case of batched writes.
    Value lsidValue = input["lsid"];
    DocumentSourceChangeStream::checkValueTypeOrMissing(lsidValue, "lsid", BSONType::Object);
    _lsid = lsidValue.missing() ? boost::none : boost::optional<Document>(lsidValue.getDocument());
    Value txnNumberValue = input["txnNumber"];
    DocumentSourceChangeStream::checkValueTypeOrMissing(
        txnNumberValue, "txnNumber", BSONType::NumberLong);
    _txnNumber = txnNumberValue.missing() ? boost::none
                                          : boost::optional<TxnNumber>(txnNumberValue.getLong());

    // We want to parse the OpTime out of this document using the BSON OpTime parser. Instead of
    // converting the entire Document back to BSON, we convert only the fields we need.
    repl::OpTime txnOpTime = repl::OpTime::parse(BSON(repl::OpTime::kTimestampFieldName
                                                      << input[repl::OpTime::kTimestampFieldName]
                                                      << repl::OpTime::kTermFieldName
                                                      << input[repl::OpTime::kTermFieldName]));
    _clusterTime = txnOpTime.getTimestamp();

    Value wallTime = input[repl::OplogEntry::kWallClockTimeFieldName];
    DocumentSourceChangeStream::checkValueType(
        wallTime, repl::OplogEntry::kWallClockTimeFieldName, BSONType::Date);
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
    }

    if (BSONType::Object ==
        input[repl::OplogEntry::kPrevWriteOpTimeInTransactionFieldName].getType()) {
        // As with the 'txnOpTime' parsing above, we convert a portion of 'input' back to BSON
        // in order to parse an OpTime, this time from the "prevOpTime" field.
        repl::OpTime prevOpTime = repl::OpTime::parse(
            input[repl::OplogEntry::kPrevWriteOpTimeInTransactionFieldName].getDocument().toBson());
        _collectAllOpTimesFromTransaction(opCtx, prevOpTime);
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
        auto firstApplyOpsEntry = _lookUpOplogEntryByOpTime(opCtx, firstTimestamp);

        auto bsonOp = firstApplyOpsEntry.getOperationToApply();
        tassert(5543806,
                str::stream() << "Expected 'applyOps' type " << BSONType::Array << ", found "
                              << bsonOp["applyOps"].type(),
                BSONType::Array == bsonOp["applyOps"].type());
        _currentApplyOps = Value(bsonOp["applyOps"]);
    }

    DocumentSourceChangeStream::checkValueType(_currentApplyOps, "applyOps", BSONType::Array);

    // Initialize iterators at the beginning of the transaction.
    _currentApplyOpsIt = _currentApplyOps.getArray().begin();
    _currentApplyOpsTs = firstTimestamp.getTimestamp();
    _currentApplyOpsIndex = 0;
    _txnOpIndex = 0;
}

boost::optional<Document>
DocumentSourceChangeStreamUnwindTransaction::TransactionOpIterator::getNextTransactionOp(
    OperationContext* opCtx) {
    while (true) {
        while (_currentApplyOpsIt != _currentApplyOps.getArray().end()) {
            Document doc = (_currentApplyOpsIt++)->getDocument();
            ++_currentApplyOpsIndex;
            ++_txnOpIndex;

            // If the document is relevant, update it with the required txn fields before returning.
            if (_isDocumentRelevant(doc)) {
                return _addRequiredTransactionFields(doc);
            }
        }

        if (_txnOplogEntries.empty()) {
            // There are no more operations in this transaction.
            return boost::none;
        }

        // We've processed all the operations in the previous applyOps entry, but we have a new
        // one to process.
        auto applyOpsEntry = _lookUpOplogEntryByOpTime(opCtx, _txnOplogEntries.top());
        _txnOplogEntries.pop();

        auto bsonOp = applyOpsEntry.getOperationToApply();
        tassert(5543807,
                str::stream() << "Expected 'applyOps' type " << BSONType::Array << ", found "
                              << bsonOp["applyOps"].type(),
                BSONType::Array == bsonOp["applyOps"].type());

        _currentApplyOps = Value(bsonOp["applyOps"]);
        _currentApplyOpsTs = applyOpsEntry.getTimestamp();
        _currentApplyOpsIt = _currentApplyOps.getArray().begin();
        _currentApplyOpsIndex = 0;
    }
}

bool DocumentSourceChangeStreamUnwindTransaction::TransactionOpIterator::_isDocumentRelevant(
    const Document& d) const {
    tassert(5543808,
            str::stream() << "Unexpected format for entry within a transaction oplog entry: "
                             "'op' field was type "
                          << typeName(d["op"].getType()),
            d["op"].getType() == BSONType::String);
    tassert(5543809,
            "Unexpected noop entry within a transaction",
            ValueComparator::kInstance.evaluate(d["op"] != Value("n"_sd)));

    return _expression->matchesBSON(d.toBson());
}

Document
DocumentSourceChangeStreamUnwindTransaction::TransactionOpIterator::_addRequiredTransactionFields(
    const Document& doc) {
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
    newDoc.addField(repl::OplogEntry::kSessionIdFieldName, _lsid ? Value(*_lsid) : Value());
    newDoc.addField(repl::OplogEntry::kTxnNumberFieldName,
                    _txnNumber ? Value(static_cast<long long>(*_txnNumber)) : Value());
    newDoc.addField(repl::OplogEntry::kWallClockTimeFieldName, Value(_wallTime));

    return newDoc.freeze();
}

repl::OplogEntry
DocumentSourceChangeStreamUnwindTransaction::TransactionOpIterator::_lookUpOplogEntryByOpTime(
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

void DocumentSourceChangeStreamUnwindTransaction::TransactionOpIterator::
    _collectAllOpTimesFromTransaction(OperationContext* opCtx, repl::OpTime firstOpTime) {
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

Pipeline::SourceContainer::iterator DocumentSourceChangeStreamUnwindTransaction::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    tassert(5687205, "Iterator mismatch during optimization", *itr == this);

    auto nextChangeStreamStageItr = std::next(itr);

    if (!feature_flags::gFeatureFlagChangeStreamsRewrite.isEnabledAndIgnoreFCV()) {
        return nextChangeStreamStageItr;
    }

    // The additional filtering added by this optimization may incorrectly filter out events if it
    // runs with the non-simple collation.
    if (pExpCtx->getCollator()) {
        return nextChangeStreamStageItr;
    }

    // Seek to the stage that immediately follows the change streams stages.
    itr = std::find_if_not(itr, container->end(), [](const auto& stage) {
        return stage->constraints().isChangeStreamStage();
    });

    if (itr == container->end()) {
        // This pipeline is just the change stream.
        return itr;
    }

    auto matchStage = dynamic_cast<DocumentSourceMatch*>(itr->get());
    if (!matchStage) {
        // This function only attempts to optimize a $match that immediately follows expanded
        // $changeStream stages. That does not apply here, and we resume optimization at the last
        // change stream stage, in case a "swap" optimization can apply between it and the stage
        // that follows it. For example, $project stages can swap in front of the last change stream
        // stages.
        return std::prev(itr);
    }

    // Build a filter which discards unwound transaction operations which would have been filtered
    // out later in the pipeline by the user's $match filter.
    auto unwindTransactionFilter = change_stream_filter::buildUnwindTransactionFilter(
        pExpCtx, matchStage->getMatchExpression());

    // Replace the default filter with the new, more restrictive one. Note that the resulting
    // expression must be serialized then deserialized to ensure it does not retain unsafe
    // references to strings in the 'matchStage' filter.
    rebuild(unwindTransactionFilter->serialize());

    // Continue optimization at the next change stream stage.
    return nextChangeStreamStageItr;
}
}  // namespace mongo
