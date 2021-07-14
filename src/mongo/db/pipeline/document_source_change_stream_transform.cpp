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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_change_stream_transform.h"

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/pipeline/change_stream_document_diff_parser.h"
#include "mongo/db/pipeline/document_path_support.h"
#include "mongo/db/pipeline/document_source_check_resume_token.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_lookup_change_post_image.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/repl/bson_extract_optime.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/transaction_history_iterator.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/db/update/update_oplog_entry_version.h"
#include "mongo/db/vector_clock.h"
#include "mongo/util/assert_util.h"

namespace mongo {

using boost::intrusive_ptr;
using boost::optional;
using std::list;
using std::string;
using std::vector;

namespace {
constexpr auto checkValueType = &DocumentSourceChangeStream::checkValueType;
}  // namespace

REGISTER_INTERNAL_DOCUMENT_SOURCE(
    _internalChangeStreamTransform,
    LiteParsedDocumentSourceChangeStreamInternal::parse,
    DocumentSourceChangeStreamTransform::createFromBson,
    feature_flags::gFeatureFlagChangeStreamsOptimization.isEnabledAndIgnoreFCV());

intrusive_ptr<DocumentSourceChangeStreamTransform>
DocumentSourceChangeStreamTransform::createFromBson(
    BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(5467601,
            "the '$_internalChangeStreamTransform' object spec must be an object",
            spec.type() == BSONType::Object);

    return new DocumentSourceChangeStreamTransform(expCtx, spec.Obj());
}

DocumentSourceChangeStreamTransform::DocumentSourceChangeStreamTransform(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, BSONObj changeStreamSpec)
    : DocumentSource(DocumentSourceChangeStreamTransform::kStageName, expCtx),
      _changeStreamSpec(DocumentSourceChangeStreamSpec::parse(
          IDLParserErrorContext("$changeStream"), changeStreamSpec)),
      _isIndependentOfAnyCollection(expCtx->ns.isCollectionlessAggregateNS()) {

    // If the change stream spec requested a pre-image, make sure that we supply one.
    _includePreImageOptime =
        (_changeStreamSpec.getFullDocumentBeforeChange() != FullDocumentBeforeChangeModeEnum::kOff);

    // If the change stream spec includes a resumeToken with a shard key, populate the document key
    // cache with the field paths.
    auto resumeAfter = _changeStreamSpec.getResumeAfter();
    auto startAfter = _changeStreamSpec.getStartAfter();

    ResumeToken resumeToken;
    if (resumeAfter || startAfter) {
        resumeToken = resumeAfter ? resumeAfter.get() : startAfter.get();
        ResumeTokenData tokenData = resumeToken.getData();

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
            const bool isFinal = docKeyFields.size() > 1;

            _documentKeyCache[tokenData.uuid.get()] =
                DocumentKeyCacheEntry({docKeyFields, isFinal});
        }
    } else if (auto startAtOperationTime = _changeStreamSpec.getStartAtOperationTime()) {
        // TODO SERVER-56669: Move this change to populate resume token in 'ChangeStreamSpec' into
        // DocumentSourceChangeStream::create().
        resumeToken = ResumeToken::makeHighWaterMarkToken(*startAtOperationTime);
    } else {
        // If we do not have an explicit starting point, we should start from the latest majority
        // committed operation. If we are on mongoS and do not have a starting point, set it to the
        // current clusterTime so that all shards start in sync. We always start one tick beyond the
        // most recent operation, to ensure that the stream does not return it.
        auto replCoord = repl::ReplicationCoordinator::get(expCtx->opCtx);
        const auto currentTime = !expCtx->inMongos
            ? LogicalTime{replCoord->getMyLastAppliedOpTime().getTimestamp()}
            : [&] {
                  const auto currentTime = VectorClock::get(expCtx->opCtx)->getTime();
                  return currentTime.clusterTime();
              }();

        // If we haven't already populated the initial PBRT, then we are starting from a specific
        // timestamp rather than a resume token. Initialize the PBRT to a high water mark token.
        const auto startAtTime = currentTime.addTicks(1).asTimestamp();
        resumeToken = ResumeToken::makeHighWaterMarkToken(startAtTime);

        // Make sure we update the 'resumeAfter' in the '_changeStreamSpec' so that we serialize the
        // correct resume token when sending it to the shards.
        _changeStreamSpec.setResumeAfter(resumeToken);
    }
    expCtx->initialPostBatchResumeToken = resumeToken.toDocument().toBson();
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
                                 UnionRequirement::kNotAllowed,
                                 ChangeStreamRequirement::kChangeStreamStage);

    // This transformation could be part of a 'collectionless' change stream on an entire
    // database or cluster, mark as independent of any collection if so.
    constraints.isIndependentOfAnyCollection = _isIndependentOfAnyCollection;
    return constraints;
}

ResumeTokenData DocumentSourceChangeStreamTransform::getResumeToken(Value ts,
                                                                    Value uuid,
                                                                    Value documentKey,
                                                                    Value txnOpIndex) {
    ResumeTokenData resumeTokenData;

    resumeTokenData.clusterTime = ts.getTimestamp();
    resumeTokenData.documentKey = documentKey;

    if (!uuid.missing()) {
        resumeTokenData.uuid = uuid.getUuid();
    }

    if (!txnOpIndex.missing()) {
        resumeTokenData.txnOpIndex = txnOpIndex.getLong();
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
    Value preImageOpTime = input[repl::OplogEntry::kPreImageOpTimeFieldName];
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
            // The version of oplog entry format. 1 or missing value indicates the old format. 2
            // indicates the delta oplog entry.
            Value oplogVersion =
                input[repl::OplogEntry::kObjectFieldName][kUpdateOplogEntryVersionFieldName];
            if (!oplogVersion.missing() && oplogVersion.getInt() == 2) {
                // Parsing the delta oplog entry.
                operationType = DocumentSourceChangeStream::kUpdateOpType;
                Value diffObj = input[repl::OplogEntry::kObjectFieldName]
                                     [update_oplog_entry::kDiffObjectFieldName];
                checkValueType(diffObj,
                               repl::OplogEntry::kObjectFieldName + "." +
                                   update_oplog_entry::kDiffObjectFieldName,
                               BSONType::Object);

                const auto& deltaDesc =
                    change_stream_document_diff_parser::parseDiff(diffObj.getDocument().toBson());

                updateDescription = Value(Document{{"updatedFields", deltaDesc.updatedFields},
                                                   {"removedFields", deltaDesc.removedFields},
                                                   {"truncatedArrays", deltaDesc.truncatedArrays}});
            } else if (id.missing()) {
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
            auto o2Type = input.getNestedField("o2.type");
            tassert(5052200, "o2.type is missing from noop oplog event", !o2Type.missing());

            if (o2Type.getString() == "migrateChunkToNewShard"_sd) {
                operationType = DocumentSourceChangeStream::kNewShardDetectedOpType;
                // Generate a fake document Id for NewShardDetected operation so that we can
                // resume after this operation.
                documentKey = Value(Document{{DocumentSourceChangeStream::kIdField,
                                              input[repl::OplogEntry::kObject2FieldName]}});
            } else if (o2Type.getString() == "reshardBegin"_sd) {
                operationType = DocumentSourceChangeStream::kReshardBeginOpType;
                doc.addField(DocumentSourceChangeStream::kReshardingUuidField,
                             input.getNestedField("o2.reshardingUUID"));
                documentKey = Value(Document{{DocumentSourceChangeStream::kIdField,
                                              input[repl::OplogEntry::kObject2FieldName]}});
            } else if (o2Type.getString() == "reshardDoneCatchUp"_sd) {
                operationType = DocumentSourceChangeStream::kReshardDoneCatchUpOpType;
                doc.addField(DocumentSourceChangeStream::kReshardingUuidField,
                             input.getNestedField("o2.reshardingUUID"));
                documentKey = Value(Document{{DocumentSourceChangeStream::kIdField,
                                              input[repl::OplogEntry::kObject2FieldName]}});
            } else {
                // We should never see an unknown noop entry.
                MONGO_UNREACHABLE_TASSERT(5052201);
            }
            break;
        }
        default: { MONGO_UNREACHABLE; }
    }

    // UUID should always be present except for invalidate and dropDatabase entries.
    if (operationType != DocumentSourceChangeStream::kInvalidateOpType &&
        operationType != DocumentSourceChangeStream::kDropDatabaseOpType) {
        invariant(!uuid.missing(), "Saw a CRUD op without a UUID");
    }

    // Extract the txnOpIndex field. This will be missing unless we are unwinding a transaction.
    auto txnOpIndex = input[DocumentSourceChangeStream::kTxnOpIndexField];

    // Add some additional fields only relevant to transactions.
    if (!txnOpIndex.missing()) {
        auto lsid = input[DocumentSourceChangeStream::kLsidField];
        checkValueType(lsid, DocumentSourceChangeStream::kLsidField, BSONType::Object);

        auto txnNumber = input[DocumentSourceChangeStream::kTxnNumberField];
        checkValueType(
            txnNumber, DocumentSourceChangeStream::kTxnNumberField, BSONType::NumberLong);

        doc.addField(DocumentSourceChangeStream::kTxnNumberField, txnNumber);
        doc.addField(DocumentSourceChangeStream::kLsidField, lsid);
    }

    // Generate the resume token. Note that only 'ts' is always guaranteed to be present.
    auto resumeTokenData = getResumeToken(ts, uuid, documentKey, txnOpIndex);
    auto resumeToken = ResumeToken(resumeTokenData).toDocument();

    doc.addField(DocumentSourceChangeStream::kIdField, Value(resumeToken));
    doc.addField(DocumentSourceChangeStream::kOperationTypeField, Value(operationType));
    doc.addField(DocumentSourceChangeStream::kClusterTimeField, Value(resumeTokenData.clusterTime));

    // We set the resume token as the document's sort key in both the sharded and non-sharded cases,
    // since we will subsequently rely upon it to generate a correct postBatchResumeToken.
    const bool isSingleElementKey = true;
    doc.metadata().setSortKey(Value{resumeToken}, isSingleElementKey);

    // Invalidation, topology change, and resharding events have fewer fields.
    if (operationType == DocumentSourceChangeStream::kInvalidateOpType ||
        operationType == DocumentSourceChangeStream::kNewShardDetectedOpType ||
        operationType == DocumentSourceChangeStream::kReshardBeginOpType ||
        operationType == DocumentSourceChangeStream::kReshardDoneCatchUpOpType) {
        return doc.freeze();
    }

    // Add the post-image, pre-image, namespace, documentKey and other fields as appropriate.
    doc.addField(DocumentSourceChangeStream::kFullDocumentField, std::move(fullDocument));
    if (_includePreImageOptime) {
        // Set 'kFullDocumentBeforeChangeField' to the pre-image optime. The DSCSLookupPreImage
        // stage will replace this optime with the actual pre-image taken from the oplog.
        doc.addField(DocumentSourceChangeStream::kFullDocumentBeforeChangeField,
                     std::move(preImageOpTime));
    }
    doc.addField(DocumentSourceChangeStream::kNamespaceField,
                 operationType == DocumentSourceChangeStream::kDropDatabaseOpType
                     ? Value(Document{{"db", nss.db()}})
                     : Value(Document{{"db", nss.db()}, {"coll", nss.coll()}}));
    doc.addField(DocumentSourceChangeStream::kDocumentKeyField, std::move(documentKey));

    // Note that 'updateDescription' might be the 'missing' value, in which case it will not be
    // serialized.
    doc.addField("updateDescription", std::move(updateDescription));
    return doc.freeze();
}

Value DocumentSourceChangeStreamTransform::serializeLatest(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    if (explain) {
        return Value(Document{{getSourceName(),
                               Document{{"stage"_sd, "internalTransform"_sd},
                                        {"options"_sd, _changeStreamSpec.toBSON()}}}});
    }

    return Value(
        Document{{DocumentSourceChangeStreamTransform::kStageName, _changeStreamSpec.toBSON()}});
}

DepsTracker::State DocumentSourceChangeStreamTransform::getDependencies(DepsTracker* deps) const {
    deps->fields.insert(repl::OplogEntry::kOpTypeFieldName.toString());
    deps->fields.insert(repl::OplogEntry::kTimestampFieldName.toString());
    deps->fields.insert(repl::OplogEntry::kNssFieldName.toString());
    deps->fields.insert(repl::OplogEntry::kUuidFieldName.toString());
    deps->fields.insert(repl::OplogEntry::kObjectFieldName.toString());
    deps->fields.insert(repl::OplogEntry::kObject2FieldName.toString());
    deps->fields.insert(repl::OplogEntry::kSessionIdFieldName.toString());
    deps->fields.insert(repl::OplogEntry::kTxnNumberFieldName.toString());
    deps->fields.insert(DocumentSourceChangeStream::kTxnOpIndexField.toString());

    if (_includePreImageOptime) {
        deps->fields.insert(repl::OplogEntry::kPreImageOpTimeFieldName.toString());
    }
    return DepsTracker::State::EXHAUSTIVE_ALL;
}

DocumentSource::GetModPathsReturn DocumentSourceChangeStreamTransform::getModifiedPaths() const {
    // All paths are modified.
    return {DocumentSource::GetModPathsReturn::Type::kAllPaths, std::set<string>{}, {}};
}

DocumentSource::GetNextResult DocumentSourceChangeStreamTransform::doGetNext() {
    uassert(50988,
            "Illegal attempt to execute an internal change stream stage on mongos. A $changeStream "
            "stage must be the first stage in a pipeline",
            !pExpCtx->inMongos);

    // Get the next input document.
    auto input = pSource->getNext();
    if (!input.isAdvanced()) {
        return input;
    }

    return applyTransformation(input.releaseDocument());
}

}  // namespace mongo
