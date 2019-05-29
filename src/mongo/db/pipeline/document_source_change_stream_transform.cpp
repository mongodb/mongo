/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_change_stream_transform.h"

#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/db/bson/bson_helper.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/commands/feature_compatibility_version_documentation.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/pipeline/change_stream_constants.h"
#include "mongo/db/pipeline/document_path_support.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_check_resume_token.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_lookup_change_post_image.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/repl/bson_extract_optime.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/transaction_history_iterator.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace mongo {

using boost::intrusive_ptr;
using boost::optional;
using std::list;
using std::string;
using std::vector;

namespace {
constexpr auto checkValueType = &DocumentSourceChangeStream::checkValueType;
}  // namespace

boost::intrusive_ptr<DocumentSourceChangeStreamTransform>
DocumentSourceChangeStreamTransform::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ServerGlobalParams::FeatureCompatibility::Version& fcv,
    BSONObj changeStreamSpec) {
    return new DocumentSourceChangeStreamTransform(expCtx, fcv, changeStreamSpec);
}

DocumentSourceChangeStreamTransform::DocumentSourceChangeStreamTransform(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ServerGlobalParams::FeatureCompatibility::Version& fcv,
    BSONObj changeStreamSpec)
    : DocumentSource(expCtx),
      _changeStreamSpec(changeStreamSpec.getOwned()),
      _isIndependentOfAnyCollection(expCtx->ns.isCollectionlessAggregateNS()),
      _fcv(fcv) {

    _nsRegex.emplace(DocumentSourceChangeStream::getNsRegexForChangeStream(expCtx->ns));

    auto spec = DocumentSourceChangeStreamSpec::parse(IDLParserErrorContext("$changeStream"),
                                                      _changeStreamSpec);

    // If the change stream spec includes a resumeToken with a shard key, populate the document key
    // cache with the field paths.
    auto resumeAfter = spec.getResumeAfter();
    auto startAfter = spec.getStartAfter();
    if (resumeAfter || startAfter) {
        ResumeToken token = resumeAfter ? resumeAfter.get() : startAfter.get();
        ResumeTokenData tokenData = token.getData();

        if (!tokenData.documentKey.missing() && tokenData.uuid) {
            std::vector<FieldPath> docKeyFields;
            auto docKey = tokenData.documentKey.getDocument();

            auto iter = docKey.fieldIterator();
            while (iter.more()) {
                auto fieldPair = iter.next();
                docKeyFields.push_back(fieldPair.first);
            }

            // If the document key from the resume token has more than one field, that means it
            // includes the shard key and thus should never change.
            auto isFinal = docKey.size() > 1;

            _documentKeyCache[tokenData.uuid.get()] =
                DocumentKeyCacheEntry({docKeyFields, isFinal});
        }
    }
}

StageConstraints DocumentSourceChangeStreamTransform::constraints(
    Pipeline::SplitState pipeState) const {
    StageConstraints constraints(StreamType::kStreaming,
                                 PositionRequirement::kNone,
                                 HostTypeRequirement::kNone,
                                 DiskUseRequirement::kNoDiskUse,
                                 FacetRequirement::kNotAllowed,
                                 TransactionRequirement::kNotAllowed,
                                 LookupRequirement::kNotAllowed,
                                 ChangeStreamRequirement::kChangeStreamStage);

    // This transformation could be part of a 'collectionless' change stream on an entire
    // database or cluster, mark as independent of any collection if so.
    constraints.isIndependentOfAnyCollection = _isIndependentOfAnyCollection;
    return constraints;
}

ResumeTokenData DocumentSourceChangeStreamTransform::getResumeToken(Value ts,
                                                                    Value uuid,
                                                                    Value documentKey) {
    ResumeTokenData resumeTokenData;
    if (_txnIterator) {
        // We're in the middle of unwinding an 'applyOps'.

        // Use the clusterTime from the higher level applyOps
        resumeTokenData.clusterTime = _txnIterator->clusterTime();
        resumeTokenData.txnOpIndex = _txnIterator->txnOpIndex();
    } else {
        resumeTokenData.clusterTime = ts.getTimestamp();
        resumeTokenData.txnOpIndex = 0;
    }

    resumeTokenData.documentKey = documentKey;
    if (!uuid.missing())
        resumeTokenData.uuid = uuid.getUuid();

    // If 'needsMerge' is true, 'mergeByPBRT' is false, and FCV is less than 4.2, then we are
    // running on a sharded cluster that is mid-upgrade, and so we generate v0 resume tokens.
    // Otherwise, we always generate v1 resume tokens whether the FCV is 4.0 or 4.2.
    if (pExpCtx->needsMerge && !pExpCtx->mergeByPBRT &&
        _fcv < ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo42) {
        resumeTokenData.version = 0;
    }

    return resumeTokenData;
}

Document DocumentSourceChangeStreamTransform::applyTransformation(const Document& input) {
    // If we're executing a change stream pipeline that was forwarded from mongos, then we expect it
    // to "need merge"---we expect to be executing the shards part of a split pipeline. It is never
    // correct for mongos to pass through the change stream without splitting into into a merging
    // part executed on mongos and a shards part.
    //
    // This is necessary so that mongos can correctly handle "invalidate" and "retryNeeded" change
    // notifications. See SERVER-31978 for an example of why the pipeline must be split.
    //
    // We have to check this invariant at run-time of the change stream rather than parse time,
    // since a mongos may forward a change stream in an invalid position (e.g. in a nested $lookup
    // or $facet pipeline). In this case, mongod is responsible for parsing the pipeline and
    // throwing an error without ever executing the change stream.
    if (pExpCtx->fromMongos) {
        invariant(pExpCtx->needsMerge);
    }

    MutableDocument doc;

    // Extract the fields we need.
    checkValueType(input[repl::OplogEntry::kOpTypeFieldName],
                   repl::OplogEntry::kOpTypeFieldName,
                   BSONType::String);
    string op = input[repl::OplogEntry::kOpTypeFieldName].getString();
    Value ts = input[repl::OplogEntry::kTimestampFieldName];
    Value ns = input[repl::OplogEntry::kNssFieldName];
    checkValueType(ns, repl::OplogEntry::kNssFieldName, BSONType::String);
    Value uuid = input[repl::OplogEntry::kUuidFieldName];
    std::vector<FieldPath> documentKeyFields;

    // Deal with CRUD operations and commands.
    auto opType = repl::OpType_parse(IDLParserErrorContext("ChangeStreamEntry.op"), op);

    NamespaceString nss(ns.getString());
    // Ignore commands in the oplog when looking up the document key fields since a command implies
    // that the change stream is about to be invalidated (e.g. collection drop).
    if (!uuid.missing() && opType != repl::OpTypeEnum::kCommand) {
        checkValueType(uuid, repl::OplogEntry::kUuidFieldName, BSONType::BinData);
        // We need to retrieve the document key fields if our cache does not have an entry for this
        // UUID or if the cache entry is not definitively final, indicating that the collection was
        // unsharded when the entry was last populated.
        auto it = _documentKeyCache.find(uuid.getUuid());
        if (it == _documentKeyCache.end() || !it->second.isFinal) {
            auto docKeyFields =
                pExpCtx->mongoProcessInterface->collectDocumentKeyFieldsForHostedCollection(
                    pExpCtx->opCtx, nss, uuid.getUuid());
            if (it == _documentKeyCache.end() || docKeyFields.second) {
                _documentKeyCache[uuid.getUuid()] = DocumentKeyCacheEntry(docKeyFields);
            }
        }

        documentKeyFields = _documentKeyCache.find(uuid.getUuid())->second.documentKeyFields;
    }
    Value id = input.getNestedField("o._id");
    // Non-replace updates have the _id in field "o2".
    StringData operationType;
    Value fullDocument;
    Value updateDescription;
    Value documentKey;

    switch (opType) {
        case repl::OpTypeEnum::kInsert: {
            operationType = DocumentSourceChangeStream::kInsertOpType;
            fullDocument = input[repl::OplogEntry::kObjectFieldName];
            documentKey = Value(document_path_support::extractPathsFromDoc(
                fullDocument.getDocument(), documentKeyFields));
            break;
        }
        case repl::OpTypeEnum::kDelete: {
            operationType = DocumentSourceChangeStream::kDeleteOpType;
            documentKey = input[repl::OplogEntry::kObjectFieldName];
            break;
        }
        case repl::OpTypeEnum::kUpdate: {
            if (id.missing()) {
                operationType = DocumentSourceChangeStream::kUpdateOpType;
                checkValueType(input[repl::OplogEntry::kObjectFieldName],
                               repl::OplogEntry::kObjectFieldName,
                               BSONType::Object);
                Document opObject = input[repl::OplogEntry::kObjectFieldName].getDocument();
                Value updatedFields = opObject["$set"];
                Value removedFields = opObject["$unset"];

                // Extract the field names of $unset document.
                vector<Value> removedFieldsVector;
                if (removedFields.getType() == BSONType::Object) {
                    auto iter = removedFields.getDocument().fieldIterator();
                    while (iter.more()) {
                        removedFieldsVector.push_back(Value(iter.next().first));
                    }
                }
                updateDescription = Value(Document{
                    {"updatedFields", updatedFields.missing() ? Value(Document()) : updatedFields},
                    {"removedFields", removedFieldsVector}});
            } else {
                operationType = DocumentSourceChangeStream::kReplaceOpType;
                fullDocument = input[repl::OplogEntry::kObjectFieldName];
            }
            documentKey = input[repl::OplogEntry::kObject2FieldName];
            break;
        }
        case repl::OpTypeEnum::kCommand: {
            if (!input.getNestedField("o.drop").missing()) {
                operationType = DocumentSourceChangeStream::kDropCollectionOpType;

                // The "o.drop" field will contain the actual collection name.
                nss = NamespaceString(nss.db(), input.getNestedField("o.drop").getString());
            } else if (!input.getNestedField("o.renameCollection").missing()) {
                operationType = DocumentSourceChangeStream::kRenameCollectionOpType;

                // The "o.renameCollection" field contains the namespace of the original collection.
                nss = NamespaceString(input.getNestedField("o.renameCollection").getString());

                // The "o.to" field contains the target namespace for the rename.
                const auto renameTargetNss =
                    NamespaceString(input.getNestedField("o.to").getString());
                doc.addField(DocumentSourceChangeStream::kRenameTargetNssField,
                             Value(Document{{"db", renameTargetNss.db()},
                                            {"coll", renameTargetNss.coll()}}));
            } else if (!input.getNestedField("o.dropDatabase").missing()) {
                operationType = DocumentSourceChangeStream::kDropDatabaseOpType;

                // Extract the database name from the namespace field and leave the collection name
                // empty.
                nss = NamespaceString(nss.db());
            } else {
                // All other commands will invalidate the stream.
                operationType = DocumentSourceChangeStream::kInvalidateOpType;
            }

            // Make sure the result doesn't have a document key.
            documentKey = Value();
            break;
        }
        case repl::OpTypeEnum::kNoop: {
            operationType = DocumentSourceChangeStream::kNewShardDetectedOpType;
            // Generate a fake document Id for NewShardDetected operation so that we can resume
            // after this operation.
            documentKey = Value(Document{{DocumentSourceChangeStream::kIdField,
                                          input[repl::OplogEntry::kObject2FieldName]}});
            break;
        }
        default: { MONGO_UNREACHABLE; }
    }

    // UUID should always be present except for invalidate and dropDatabase entries.
    if (operationType != DocumentSourceChangeStream::kInvalidateOpType &&
        operationType != DocumentSourceChangeStream::kDropDatabaseOpType) {
        invariant(!uuid.missing(), "Saw a CRUD op without a UUID");
    }

    // Note that 'documentKey' and/or 'uuid' might be missing, in which case they will not appear
    // in the output.
    auto resumeTokenData = getResumeToken(ts, uuid, documentKey);
    auto resumeToken = ResumeToken(resumeTokenData).toDocument();

    // Add some additional fields only relevant to transactions.
    if (_txnIterator) {
        doc.addField(DocumentSourceChangeStream::kTxnNumberField,
                     Value(static_cast<long long>(_txnIterator->txnNumber())));
        doc.addField(DocumentSourceChangeStream::kLsidField, Value(_txnIterator->lsid()));
    }

    doc.addField(DocumentSourceChangeStream::kIdField, Value(resumeToken));
    doc.addField(DocumentSourceChangeStream::kOperationTypeField, Value(operationType));
    doc.addField(DocumentSourceChangeStream::kClusterTimeField, Value(resumeTokenData.clusterTime));

    // We set the resume token as the document's sort key in both the sharded and non-sharded cases,
    // since we will subsequently rely upon it to generate a correct postBatchResumeToken.
    // TODO SERVER-38539: when returning results for merging, we first check whether 'mergeByPBRT'
    // has been set. If not, then the request was sent from an older mongoS which cannot merge by
    // raw resume tokens, and we must use the old sort key format. This check, and the 'mergeByPBRT'
    // flag, are no longer necessary in 4.4; all change streams will be merged by resume token.
    if (pExpCtx->needsMerge && !pExpCtx->mergeByPBRT) {
        doc.setSortKeyMetaField(BSON("" << ts << "" << uuid << "" << documentKey));
    } else {
        doc.setSortKeyMetaField(resumeToken.toBson());
    }

    // "invalidate" and "newShardDetected" entries have fewer fields.
    if (operationType == DocumentSourceChangeStream::kInvalidateOpType ||
        operationType == DocumentSourceChangeStream::kNewShardDetectedOpType) {
        return doc.freeze();
    }

    doc.addField(DocumentSourceChangeStream::kFullDocumentField, fullDocument);
    doc.addField(DocumentSourceChangeStream::kNamespaceField,
                 operationType == DocumentSourceChangeStream::kDropDatabaseOpType
                     ? Value(Document{{"db", nss.db()}})
                     : Value(Document{{"db", nss.db()}, {"coll", nss.coll()}}));
    doc.addField(DocumentSourceChangeStream::kDocumentKeyField, documentKey);

    // Note that 'updateDescription' might be the 'missing' value, in which case it will not be
    // serialized.
    doc.addField("updateDescription", updateDescription);
    return doc.freeze();
}

Value DocumentSourceChangeStreamTransform::serialize(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    Document changeStreamOptions(_changeStreamSpec);
    // If we're on a mongos and no other start time is specified, we want to start at the current
    // cluster time on the mongos.  This ensures all shards use the same start time.
    if (pExpCtx->inMongos &&
        changeStreamOptions[DocumentSourceChangeStreamSpec::kResumeAfterFieldName].missing() &&
        changeStreamOptions[DocumentSourceChangeStreamSpec::kStartAtOperationTimeFieldName]
            .missing() &&
        changeStreamOptions[DocumentSourceChangeStreamSpec::kStartAfterFieldName].missing()) {
        MutableDocument newChangeStreamOptions(changeStreamOptions);

        // Use the current cluster time plus 1 tick since the oplog query will include all
        // operations/commands equal to or greater than the 'startAtOperationTime' timestamp. In
        // particular, avoid including the last operation that went through mongos in an attempt to
        // match the behavior of a replica set more closely.
        auto clusterTime = LogicalClock::get(pExpCtx->opCtx)->getClusterTime();
        clusterTime.addTicks(1);
        newChangeStreamOptions[DocumentSourceChangeStreamSpec::kStartAtOperationTimeFieldName] =
            Value(clusterTime.asTimestamp());
        changeStreamOptions = newChangeStreamOptions.freeze();
    }
    return Value(Document{{getSourceName(), changeStreamOptions}});
}

DepsTracker::State DocumentSourceChangeStreamTransform::getDependencies(DepsTracker* deps) const {
    deps->fields.insert(repl::OplogEntry::kOpTypeFieldName.toString());
    deps->fields.insert(repl::OplogEntry::kTimestampFieldName.toString());
    deps->fields.insert(repl::OplogEntry::kNssFieldName.toString());
    deps->fields.insert(repl::OplogEntry::kUuidFieldName.toString());
    deps->fields.insert(repl::OplogEntry::kObjectFieldName.toString());
    deps->fields.insert(repl::OplogEntry::kObject2FieldName.toString());
    return DepsTracker::State::EXHAUSTIVE_ALL;
}

DocumentSource::GetModPathsReturn DocumentSourceChangeStreamTransform::getModifiedPaths() const {
    // All paths are modified.
    return {DocumentSource::GetModPathsReturn::Type::kAllPaths, std::set<string>{}, {}};
}

DocumentSource::GetNextResult DocumentSourceChangeStreamTransform::getNext() {
    pExpCtx->checkForInterrupt();

    uassert(50988,
            "Illegal attempt to execute an internal change stream stage on mongos. A $changeStream "
            "stage must be the first stage in a pipeline",
            !pExpCtx->inMongos);

    while (1) {
        // If we're unwinding an 'applyOps' from a transaction, check if there are any documents we
        // have stored that can be returned.
        if (_txnIterator) {
            if (auto next = _txnIterator->getNextTransactionOp(pExpCtx->opCtx)) {
                return applyTransformation(*next);
            }
            _txnIterator = boost::none;
        }

        // Get the next input document.
        auto input = pSource->getNext();
        if (!input.isAdvanced()) {
            return input;
        }

        auto doc = input.releaseDocument();

        auto op = doc[repl::OplogEntry::kOpTypeFieldName];
        auto opType =
            repl::OpType_parse(IDLParserErrorContext("ChangeStreamEntry.op"), op.getStringData());
        auto commandVal = doc["o"];
        if (opType != repl::OpTypeEnum::kCommand ||
            (commandVal["applyOps"].missing() && commandVal["commitTransaction"].missing())) {
            // We should never see an "abortTransaction" command at this point.
            invariant(opType != repl::OpTypeEnum::kCommand ||
                      commandVal["abortTransaction"].missing());

            // This oplog entry represents a single change. Apply the transform to it and return the
            // resulting document.
            return applyTransformation(doc);
        }

        // The only two commands we will see here are an applyOps or a commit, which both mean we
        // need to open a "transaction context" representing a group of updates that all occurred at
        // once as part of a transaction. If we already have a transaction context open, that would
        // mean we are looking at an applyOps or commit nested within an applyOps, which is not
        // allowed in the oplog.
        invariant(!_txnIterator);
        _txnIterator.emplace(pExpCtx->opCtx, pExpCtx->mongoProcessInterface, doc, *_nsRegex);

        // Once we initialize the transaction iterator, we can loop back to the top in order to call
        // 'getNextTransactionOp' on it. Note that is possible for the transaction iterator
        // to be empty of any relevant operations, meaning that this loop may need to execute
        // multiple times before it encounters a relevant change to return.
    }
}

DocumentSourceChangeStreamTransform::TransactionOpIterator::TransactionOpIterator(
    OperationContext* opCtx,
    std::shared_ptr<MongoProcessInterface> mongoProcessInterface,
    const Document& input,
    const pcrecpp::RE& nsRegex)
    : _mongoProcessInterface(mongoProcessInterface), _nsRegex(nsRegex) {
    Value lsidValue = input["lsid"];
    checkValueType(lsidValue, "lsid", BSONType::Object);
    _lsid = lsidValue.getDocument();

    Value txnNumberValue = input["txnNumber"];
    checkValueType(txnNumberValue, "txnNumber", BSONType::NumberLong);
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
        // '_txnOplogEntries' stack of applyOps entries that the change stream should process as
        // part of this transaction. There may be additional applyOps entries linked through the
        // 'prevOpTime' field, which will also get added to '_txnOplogEntries' later in this
        // function. Note that this style of transaction does not have a 'commitTransaction'
        // command.
        _txnOplogEntries.push(txnOpTime);
    } else {
        // This must be a "commitTransaction" command, which commits a prepared transaction. This
        // style of transaction does not have an applyOps entry that implicitly commits it, as in
        // the previous case. We're going to iterate through the other oplog entries in the
        // transaction, but this entry does not have any updates in it, so we do not include it in
        // the '_txnOplogEntries' stack.
        invariant(!commandObj["commitTransaction"].missing());
    }

    if (BSONType::Object ==
        input[repl::OplogEntry::kPrevWriteOpTimeInTransactionFieldName].getType()) {
        // As with the 'txnOpTime' parsing above, we convert a portion of 'input' back to BSON in
        // order to parse an OpTime, this time from the "prevOpTime" field.
        repl::OpTime prevOpTime = repl::OpTime::parse(
            input[repl::OplogEntry::kPrevWriteOpTimeInTransactionFieldName].getDocument().toBson());
        _collectAllOpTimesFromTransaction(opCtx, prevOpTime);
    }

    // Pop the first OpTime off the stack and use it to load the first oplog entry into the
    // '_currentApplyOps' field.
    invariant(_txnOplogEntries.size() > 0);
    const auto firstTimestamp = _txnOplogEntries.top();
    _txnOplogEntries.pop();

    if (firstTimestamp == txnOpTime) {
        // This transaction consists of only one oplog entry, from which we have already extracted
        // the "applyOps" array, so there is no need to do any more work.
        invariant(_txnOplogEntries.size() == 0);
        _currentApplyOps = std::move(applyOps);
    } else {
        // This transaction consists of multiple oplog entries; grab the chronologically first entry
        // and extract its "applyOps" array.
        auto firstApplyOpsEntry = _lookUpOplogEntryByOpTime(opCtx, firstTimestamp);

        auto bsonOp = firstApplyOpsEntry.getOperationToApply();
        invariant(BSONType::Array == bsonOp["applyOps"].type());
        _currentApplyOps = Value(bsonOp["applyOps"]);
    }

    checkValueType(_currentApplyOps, "applyOps", BSONType::Array);
    invariant(_currentApplyOps.getArrayLength() > 0);

    // Initialize iterators at the beginning of the transaction.
    _currentApplyOpsIt = _currentApplyOps.getArray().begin();
    _txnOpIndex = 0;
}

bool DocumentSourceChangeStreamTransform::TransactionOpIterator::_isDocumentRelevant(
    const Document& d) const {
    invariant(
        d["op"].getType() == BSONType::String,
        str::stream()
            << "Unexpected format for entry within a transaction oplog entry: 'op' field was type "
            << typeName(d["op"].getType()));
    invariant(ValueComparator::kInstance.evaluate(d["op"] != Value("n"_sd)),
              "Unexpected noop entry within a transaction");

    Value nsField = d["ns"];
    invariant(!nsField.missing());

    return _nsRegex.PartialMatch(nsField.getString());
}

boost::optional<Document>
DocumentSourceChangeStreamTransform::TransactionOpIterator::getNextTransactionOp(
    OperationContext* opCtx) {
    while (true) {
        while (_currentApplyOpsIt != _currentApplyOps.getArray().end()) {
            Document d = (_currentApplyOpsIt++)->getDocument();
            ++_txnOpIndex;
            if (_isDocumentRelevant(d)) {
                return d;
            }
        }

        if (_txnOplogEntries.empty()) {
            // There are no more operations in this transaction.
            return boost::none;
        }

        // We've processed all the operations in the previous applyOps entry, but we have a new one
        // to process.
        auto applyOpsEntry = _lookUpOplogEntryByOpTime(opCtx, _txnOplogEntries.top());
        _txnOplogEntries.pop();

        auto bsonOp = applyOpsEntry.getOperationToApply();
        invariant(BSONType::Array == bsonOp["applyOps"].type());

        _currentApplyOps = Value(bsonOp["applyOps"]);
        _currentApplyOpsIt = _currentApplyOps.getArray().begin();
    }
}

repl::OplogEntry
DocumentSourceChangeStreamTransform::TransactionOpIterator::_lookUpOplogEntryByOpTime(
    OperationContext* opCtx, repl::OpTime lookupTime) const {
    invariant(!lookupTime.isNull());

    std::unique_ptr<TransactionHistoryIteratorBase> iterator(
        _mongoProcessInterface->createTransactionHistoryIterator(lookupTime));
    try {
        return iterator->next(opCtx);
    } catch (ExceptionFor<ErrorCodes::IncompleteTransactionHistory>& ex) {
        ex.addContext(
            "Oplog no longer has history necessary for $changeStream to observe operations from a "
            "committed transaction.");
        uasserted(ErrorCodes::ChangeStreamHistoryLost, ex.reason());
    }
}

void DocumentSourceChangeStreamTransform::TransactionOpIterator::_collectAllOpTimesFromTransaction(
    OperationContext* opCtx, repl::OpTime firstOpTime) {
    std::unique_ptr<TransactionHistoryIteratorBase> iterator(
        _mongoProcessInterface->createTransactionHistoryIterator(firstOpTime));

    try {
        while (iterator->hasNext()) {
            _txnOplogEntries.push(iterator->nextOpTime(opCtx));
        }
    } catch (ExceptionFor<ErrorCodes::IncompleteTransactionHistory>& ex) {
        ex.addContext(
            "Oplog no longer has history necessary for $changeStream to observe operations from a "
            "committed transaction.");
        uasserted(ErrorCodes::ChangeStreamHistoryLost, ex.reason());
    }
}

}  // namespace mongo
