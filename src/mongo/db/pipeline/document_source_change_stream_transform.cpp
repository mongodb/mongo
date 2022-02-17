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
#include "mongo/db/pipeline/change_stream_filter_helpers.h"
#include "mongo/db/pipeline/change_stream_helpers_legacy.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/pipeline/document_path_support.h"
#include "mongo/db/pipeline/document_source_change_stream_add_post_image.h"
#include "mongo/db/pipeline/document_source_change_stream_check_resumability.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/repl/bson_extract_optime.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/transaction_history_iterator.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/db/update/update_oplog_entry_version.h"
#include "mongo/util/assert_util.h"

namespace mongo {

using boost::intrusive_ptr;
using boost::optional;
using std::list;
using std::string;
using std::vector;

namespace {
constexpr auto checkValueType = &DocumentSourceChangeStream::checkValueType;

Document copyDocExceptFields(const Document& source, const std::set<StringData>& fieldNames) {
    MutableDocument doc(source);
    for (auto fieldName : fieldNames) {
        doc.remove(fieldName);
    }
    return doc.freeze();
}

ResumeTokenData makeResumeToken(Value ts,
                                Value txnOpIndex,
                                Value uuid,
                                StringData operationType,
                                Value documentKey,
                                Value opDescription) {
    ResumeTokenData resumeTokenData;
    resumeTokenData.clusterTime = ts.getTimestamp();
    if (!uuid.missing()) {
        resumeTokenData.uuid = uuid.getUuid();
    }
    if (!txnOpIndex.missing()) {
        resumeTokenData.txnOpIndex = txnOpIndex.getLong();
    }
    resumeTokenData.eventIdentifier =
        ResumeToken::makeEventIdentifier(operationType, documentKey, opDescription);

    return resumeTokenData;
}
}  // namespace

REGISTER_INTERNAL_DOCUMENT_SOURCE(_internalChangeStreamTransform,
                                  LiteParsedDocumentSourceChangeStreamInternal::parse,
                                  DocumentSourceChangeStreamTransform::createFromBson,
                                  true);

intrusive_ptr<DocumentSourceChangeStreamTransform> DocumentSourceChangeStreamTransform::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const DocumentSourceChangeStreamSpec& spec) {
    return new DocumentSourceChangeStreamTransform(expCtx, spec);
}

intrusive_ptr<DocumentSourceChangeStreamTransform>
DocumentSourceChangeStreamTransform::createFromBson(
    BSONElement rawSpec, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(5467601,
            "the '$_internalChangeStreamTransform' object spec must be an object",
            rawSpec.type() == BSONType::Object);
    auto spec = DocumentSourceChangeStreamSpec::parse(IDLParserErrorContext("$changeStream"),
                                                      rawSpec.Obj());
    return new DocumentSourceChangeStreamTransform(expCtx, std::move(spec));
}

DocumentSourceChangeStreamTransform::DocumentSourceChangeStreamTransform(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, DocumentSourceChangeStreamSpec spec)
    : DocumentSource(DocumentSourceChangeStreamTransform::kStageName, expCtx),
      _changeStreamSpec(std::move(spec)),
      _isIndependentOfAnyCollection(expCtx->ns.isCollectionlessAggregateNS()) {

    // Determine whether the user requested a point-in-time pre-image, which will affect this
    // stage's output.
    _preImageRequested =
        _changeStreamSpec.getFullDocumentBeforeChange() != FullDocumentBeforeChangeModeEnum::kOff;

    // Determine whether the user requested a point-in-time post-image, which will affect this
    // stage's output.
    _postImageRequested =
        _changeStreamSpec.getFullDocument() == FullDocumentModeEnum::kWhenAvailable ||
        _changeStreamSpec.getFullDocument() == FullDocumentModeEnum::kRequired;

    // Extract the resume token or high-water-mark from the spec.
    auto tokenData = DocumentSourceChangeStream::resolveResumeTokenFromSpec(_changeStreamSpec);

    // Set the initialPostBatchResumeToken on the expression context.
    expCtx->initialPostBatchResumeToken = ResumeToken(tokenData).toBSON();

    // If the change stream spec includes a resumeToken with a shard key, populate the document key
    // cache with the field paths.
    _documentKeyCache = change_stream_legacy::buildDocumentKeyCache(tokenData);
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

    // Deal with CRUD operations and commands.
    auto opType = repl::OpType_parse(IDLParserErrorContext("ChangeStreamEntry.op"), op);

    NamespaceString nss(ns.getString());
    Value id = input.getNestedField("o._id");
    // Non-replace updates have the _id in field "o2".
    StringData operationType;
    Value fullDocument;
    Value updateDescription;
    Value documentKey;
    Value operationDescription;

    switch (opType) {
        case repl::OpTypeEnum::kInsert: {
            operationType = DocumentSourceChangeStream::kInsertOpType;
            fullDocument = input[repl::OplogEntry::kObjectFieldName];
            documentKey = input[repl::OplogEntry::kObject2FieldName];
            // For oplog entries written on an older version of the server, the documentKey may be
            // missing.
            if (documentKey.missing()) {
                // If we are resuming from an 'insert' oplog entry that does not have a documentKey,
                // it may have been read on an older version of the server that populated the
                // documentKey fields from the sharding catalog. We populate the fields we observed
                // in the resume token in order to retain consistent event ordering around the
                // resume point during upgrade. Otherwise, we default to _id as the only document
                // key field.
                if (_documentKeyCache && _documentKeyCache->first == uuid.getUuid()) {
                    documentKey = Value(document_path_support::extractPathsFromDoc(
                        fullDocument.getDocument(), _documentKeyCache->second));
                } else {
                    documentKey = Value(Document{{"_id", id}});
                }
            }
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

                if (_changeStreamSpec.getShowRawUpdateDescription()) {
                    updateDescription = input[repl::OplogEntry::kObjectFieldName];
                } else {
                    const auto& deltaDesc = change_stream_document_diff_parser::parseDiff(
                        diffObj.getDocument().toBson());

                    updateDescription =
                        Value(Document{{"updatedFields", deltaDesc.updatedFields},
                                       {"removedFields", deltaDesc.removedFields},
                                       {"truncatedArrays", deltaDesc.truncatedArrays}});
                }
            } else if (id.missing()) {
                operationType = DocumentSourceChangeStream::kUpdateOpType;
                checkValueType(input[repl::OplogEntry::kObjectFieldName],
                               repl::OplogEntry::kObjectFieldName,
                               BSONType::Object);

                if (_changeStreamSpec.getShowRawUpdateDescription()) {
                    updateDescription = input[repl::OplogEntry::kObjectFieldName];
                } else {
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

                    updateDescription = Value(
                        Document{{"updatedFields",
                                  updatedFields.missing() ? Value(Document()) : updatedFields},
                                 {"removedFields", removedFieldsVector}});
                }
            } else {
                operationType = DocumentSourceChangeStream::kReplaceOpType;
                fullDocument = input[repl::OplogEntry::kObjectFieldName];
            }

            // Add update modification for post-image computation.
            if (_postImageRequested && operationType == DocumentSourceChangeStream::kUpdateOpType) {
                doc.addField(DocumentSourceChangeStream::kRawOplogUpdateSpecField,
                             input[repl::OplogEntry::kObjectFieldName]);
            }
            documentKey = input[repl::OplogEntry::kObject2FieldName];
            break;
        }
        case repl::OpTypeEnum::kCommand: {
            const auto oField = input[repl::OplogEntry::kObjectFieldName].getDocument();
            if (auto nssField = oField.getField("drop"); !nssField.missing()) {
                operationType = DocumentSourceChangeStream::kDropCollectionOpType;

                // The "o.drop" field will contain the actual collection name.
                nss = NamespaceString(nss.db(), nssField.getString());
            } else if (auto nssField = oField.getField("renameCollection"); !nssField.missing()) {
                operationType = DocumentSourceChangeStream::kRenameCollectionOpType;

                // The "o.renameCollection" field contains the namespace of the original collection.
                nss = NamespaceString(nssField.getString());

                // The "to" field contains the target namespace for the rename.
                const auto renameTargetNss = NamespaceString(oField["to"].getString());
                const Value renameTarget(Document{
                    {"db", renameTargetNss.db()},
                    {"coll", renameTargetNss.coll()},
                });

                // The 'to' field predates the 'operationDescription' field which was added in 5.3.
                // We keep the top-level 'to' field for backwards-compatibility.
                doc.addField(DocumentSourceChangeStream::kRenameTargetNssField, renameTarget);

                // If 'showExpandedEvents' is set, include full details of the rename in
                // 'operationDescription'.
                if (_changeStreamSpec.getShowExpandedEvents()) {
                    MutableDocument opDescBuilder(
                        copyDocExceptFields(oField, {"renameCollection"_sd, "stayTemp"_sd}));
                    opDescBuilder.setField(DocumentSourceChangeStream::kRenameTargetNssField,
                                           renameTarget);
                    operationDescription = opDescBuilder.freezeToValue();
                }
            } else if (!oField.getField("dropDatabase").missing()) {
                operationType = DocumentSourceChangeStream::kDropDatabaseOpType;

                // Extract the database name from the namespace field and leave the collection name
                // empty.
                nss = NamespaceString(nss.db());
            } else if (auto nssField = oField.getField("create"); !nssField.missing()) {
                operationType = DocumentSourceChangeStream::kCreateOpType;
                nss = NamespaceString(nss.db(), nssField.getString());
                operationDescription = Value(copyDocExceptFields(oField, {"create"_sd}));
            } else if (auto nssField = oField.getField("createIndexes"); !nssField.missing()) {
                operationType = DocumentSourceChangeStream::kCreateIndexesOpType;
                nss = NamespaceString(nss.db(), nssField.getString());
                // Wrap the index spec in an "indexes" array for consistency with commitIndexBuild.
                auto indexSpec = Value(copyDocExceptFields(oField, {"createIndexes"_sd}));
                operationDescription = Value(Document{{"indexes", std::vector<Value>{indexSpec}}});
            } else if (auto nssField = oField.getField("commitIndexBuild"); !nssField.missing()) {
                operationType = DocumentSourceChangeStream::kCreateIndexesOpType;
                nss = NamespaceString(nss.db(), nssField.getString());
                operationDescription = Value(Document{{"indexes", oField.getField("indexes")}});
            } else if (auto nssField = oField.getField("dropIndexes"); !nssField.missing()) {
                const auto o2Field = input[repl::OplogEntry::kObject2FieldName].getDocument();
                operationType = DocumentSourceChangeStream::kDropIndexesOpType;
                nss = NamespaceString(nss.db(), nssField.getString());
                // Wrap the index spec in an "indexes" array for consistency with createIndexes
                // and commitIndexBuild.
                auto indexSpec = Value(copyDocExceptFields(o2Field, {"dropIndexes"_sd}));
                operationDescription = Value(Document{{"indexes", std::vector<Value>{indexSpec}}});
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

    // Extract the 'txnOpIndex' and 'applyOpsIndex' fields. These will be missing unless we are
    // unwinding a transaction.
    auto txnOpIndex = input[DocumentSourceChangeStream::kTxnOpIndexField];
    auto applyOpsIndex = input[DocumentSourceChangeStream::kApplyOpsIndexField];
    auto applyOpsEntryTs = input[DocumentSourceChangeStream::kApplyOpsTsField];

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
    auto resumeTokenData =
        makeResumeToken(ts, txnOpIndex, uuid, operationType, documentKey, operationDescription);
    auto resumeToken = ResumeToken(resumeTokenData).toDocument();

    doc.addField(DocumentSourceChangeStream::kIdField, Value(resumeToken));
    doc.addField(DocumentSourceChangeStream::kOperationTypeField, Value(operationType));
    doc.addField(DocumentSourceChangeStream::kClusterTimeField, Value(resumeTokenData.clusterTime));

    // We set the resume token as the document's sort key in both the sharded and non-sharded cases,
    // since we will subsequently rely upon it to generate a correct postBatchResumeToken.
    const bool isSingleElementKey = true;
    doc.metadata().setSortKey(Value{resumeToken}, isSingleElementKey);

    if (_changeStreamSpec.getShowExpandedEvents()) {
        // Note: If the UUID is a missing value (which can be true for events like 'dropDatabase'),
        // 'addField' will not add anything to the document.
        doc.addField(DocumentSourceChangeStream::kCollectionUuidField, uuid);

        const auto wallTime = input[repl::OplogEntry::kWallClockTimeFieldName];
        checkValueType(wallTime, repl::OplogEntry::kWallClockTimeFieldName, BSONType::Date);
        doc.addField(DocumentSourceChangeStream::kWallTimeField, wallTime);
    }

    // Invalidation, topology change, and resharding events have fewer fields.
    if (operationType == DocumentSourceChangeStream::kInvalidateOpType ||
        operationType == DocumentSourceChangeStream::kNewShardDetectedOpType ||
        operationType == DocumentSourceChangeStream::kReshardBeginOpType ||
        operationType == DocumentSourceChangeStream::kReshardDoneCatchUpOpType) {
        return doc.freeze();
    }

    // Add the post-image, pre-image id, namespace, documentKey and other fields as appropriate.
    doc.addField(DocumentSourceChangeStream::kFullDocumentField, std::move(fullDocument));

    // Determine whether the preImageId should be included, for eligible operations. Note that we
    // will include preImageId even if the user requested a post-image but no pre-image, because the
    // pre-image is required to compute the post-image.
    static const std::set<StringData> preImageOps = {DocumentSourceChangeStream::kUpdateOpType,
                                                     DocumentSourceChangeStream::kReplaceOpType,
                                                     DocumentSourceChangeStream::kDeleteOpType};
    static const std::set<StringData> postImageOps = {DocumentSourceChangeStream::kUpdateOpType};
    if ((_preImageRequested && preImageOps.count(operationType)) ||
        (_postImageRequested && postImageOps.count(operationType))) {
        auto preImageOpTime = input[repl::OplogEntry::kPreImageOpTimeFieldName];
        if (!preImageOpTime.missing()) {
            // Set 'kPreImageIdField' to the pre-image optime. The DSCSAddPreImage stage will use
            // this optime in order to fetch the pre-image from the oplog.
            doc.addField(DocumentSourceChangeStream::kPreImageIdField, std::move(preImageOpTime));
        } else {
            // Set 'kPreImageIdField' to the 'ChangeStreamPreImageId'. The DSCSAddPreImage stage
            // will use the id in order to fetch the pre-image from the pre-images collection.
            const auto preImageId = ChangeStreamPreImageId(
                uuid.getUuid(),
                applyOpsEntryTs.missing() ? ts.getTimestamp() : applyOpsEntryTs.getTimestamp(),
                applyOpsIndex.missing() ? 0 : applyOpsIndex.getLong());
            doc.addField(DocumentSourceChangeStream::kPreImageIdField, Value(preImageId.toBSON()));
        }
    }
    doc.addField(DocumentSourceChangeStream::kNamespaceField,
                 operationType == DocumentSourceChangeStream::kDropDatabaseOpType
                     ? Value(Document{{"db", nss.db()}})
                     : Value(Document{{"db", nss.db()}, {"coll", nss.coll()}}));

    // The event may have a documentKey OR an operationDescription, but not both. We already
    // validated this while creating the resume token.
    doc.addField(DocumentSourceChangeStream::kDocumentKeyField, std::move(documentKey));
    if (_changeStreamSpec.getShowExpandedEvents()) {
        doc.addField(DocumentSourceChangeStream::kOperationDescriptionField, operationDescription);
    }

    // Note that the update description field might be the 'missing' value, in which case it will
    // not be serialized.
    auto updateDescriptionFieldName = _changeStreamSpec.getShowRawUpdateDescription()
        ? DocumentSourceChangeStream::kRawUpdateDescriptionField
        : DocumentSourceChangeStream::kUpdateDescriptionField;
    doc.addField(updateDescriptionFieldName, std::move(updateDescription));

    return doc.freeze();
}

Value DocumentSourceChangeStreamTransform::serialize(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    if (explain) {
        return Value(Document{{DocumentSourceChangeStream::kStageName,
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
    deps->fields.insert(repl::OplogEntry::kWallClockTimeFieldName.toString());

    if (_preImageRequested || _postImageRequested) {
        deps->fields.insert(repl::OplogEntry::kPreImageOpTimeFieldName.toString());
        deps->fields.insert(DocumentSourceChangeStream::kApplyOpsIndexField.toString());
        deps->fields.insert(DocumentSourceChangeStream::kApplyOpsTsField.toString());
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
