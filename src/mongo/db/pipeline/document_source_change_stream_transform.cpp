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
#include "mongo/db/catalog/uuid_catalog.h"
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
    if (_txnContext) {
        // We're in the middle of unwinding an 'applyOps'.

        // Use the clusterTime from the higher level applyOps
        resumeTokenData.clusterTime = _txnContext->clusterTime;

        // 'pos' points to the _next_ applyOps index, so we must subtract one to get the index of
        // the entry being examined right now.
        invariant(_txnContext->pos >= 1);
        resumeTokenData.applyOpsIndex = _txnContext->pos - 1;
    } else {
        resumeTokenData.clusterTime = ts.getTimestamp();
        resumeTokenData.applyOpsIndex = 0;
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
    if (_txnContext) {
        doc.addField(DocumentSourceChangeStream::kTxnNumberField,
                     Value(static_cast<long long>(_txnContext->txnNumber)));
        doc.addField(DocumentSourceChangeStream::kLsidField, Value(_txnContext->lsid));
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

void DocumentSourceChangeStreamTransform::initializeTransactionContext(const Document& input) {
    // The only two commands we will see here are an applyOps or a commit, which both mean we
    // need to open a "transaction context" representing a group of updates that all occurred at
    // once as part of a transaction. If we already have a transaction context open, that would
    // mean we are looking at an applyOps or commit nested within an applyOps, which is not
    // allowed in the oplog.
    invariant(!_txnContext);

    Value lsid = input["lsid"];
    checkValueType(lsid, "lsid", BSONType::Object);

    Value txnNumber = input["txnNumber"];
    checkValueType(txnNumber, "txnNumber", BSONType::NumberLong);

    Value ts = input[repl::OplogEntry::kTimestampFieldName];
    Timestamp txnApplyTime = ts.getTimestamp();

    auto commandObj = input["o"].getDocument();
    Value applyOps = commandObj["applyOps"];
    if (!applyOps.missing()) {
        // An "applyOps" command represents an immediately-committed transaction. We place the
        // operations within the "applyOps" array directly into the transaction context.
        applyOps = input.getNestedField("o.applyOps");
    } else {
        invariant(!commandObj["commitTransaction"].missing());

        // A "commit" command is the second part of a transaction that has been split up into
        // two oplog entries. The lsid, txnNumber, and timestamp are in this entry, but the
        // "applyOps" array is in a previous entry, which we must look up.
        repl::OpTime opTime;
        uassertStatusOK(bsonExtractOpTimeField(input.toBson(), "prevOpTime", &opTime));

        auto applyOpsEntry =
            pExpCtx->mongoProcessInterface->lookUpOplogEntryByOpTime(pExpCtx->opCtx, opTime);
        invariant(applyOpsEntry.isCommand() &&
                  (repl::OplogEntry::CommandType::kApplyOps == applyOpsEntry.getCommandType()));
        invariant(applyOpsEntry.shouldPrepare());

        auto bsonOp = applyOpsEntry.getOperationToApply();
        invariant(BSONType::Array == bsonOp["applyOps"].type());
        applyOps = Value(bsonOp["applyOps"]);
    }

    checkValueType(applyOps, "applyOps", BSONType::Array);
    invariant(applyOps.getArrayLength() > 0);

    _txnContext.emplace(applyOps, txnApplyTime, lsid.getDocument(), txnNumber.getLong());
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
        if (_txnContext) {
            if (auto next = extractNextApplyOpsEntry()) {
                return applyTransformation(*next);
            }
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

        initializeTransactionContext(doc);

        // Once we initialize the transaction context, we can loop back to the top in order to call
        // 'extractNextApplyOpsEntry' on it. Note that is possible for the transaction context to be
        // empty of any relevant operations, meaning that this loop may need to execute multiple
        // times before it encounters a relevant change to return.
    }
}

bool DocumentSourceChangeStreamTransform::isDocumentRelevant(const Document& d) {
    invariant(
        d["op"].getType() == BSONType::String,
        str::stream()
            << "Unexpected format for entry within a transaction oplog entry: 'op' field was type "
            << typeName(d["op"].getType()));
    invariant(ValueComparator::kInstance.evaluate(d["op"] != Value("n"_sd)),
              "Unexpected noop entry within a transaction");

    Value nsField = d["ns"];
    invariant(!nsField.missing());

    return _nsRegex->PartialMatch(nsField.getString());
}

boost::optional<Document> DocumentSourceChangeStreamTransform::extractNextApplyOpsEntry() {

    while (_txnContext && _txnContext->pos < _txnContext->arr.size()) {
        Document d = _txnContext->arr[_txnContext->pos++].getDocument();
        if (isDocumentRelevant(d)) {
            return d;
        }
    }

    _txnContext = boost::none;

    return boost::none;
}

}  // namespace mongo
