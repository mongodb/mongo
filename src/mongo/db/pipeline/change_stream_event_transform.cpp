/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/pipeline/change_stream_event_transform.h"

#include "mongo/db/pipeline/change_stream_document_diff_parser.h"
#include "mongo/db/pipeline/change_stream_filter_helpers.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/pipeline/document_path_support.h"
#include "mongo/db/pipeline/document_source_change_stream_add_post_image.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/repl/bson_extract_optime.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"

namespace mongo {
namespace {
constexpr auto checkValueType = &DocumentSourceChangeStream::checkValueType;
constexpr auto checkValueTypeOrMissing = &DocumentSourceChangeStream::checkValueTypeOrMissing;

Document copyDocExceptFields(const Document& source, const std::set<StringData>& fieldNames) {
    MutableDocument doc(source);
    for (auto fieldName : fieldNames) {
        doc.remove(fieldName);
    }
    return doc.freeze();
}

repl::OpTypeEnum getOplogOpType(const Document& oplog) {
    auto opTypeField = oplog[repl::OplogEntry::kOpTypeFieldName];
    checkValueType(opTypeField, repl::OplogEntry::kOpTypeFieldName, BSONType::String);
    return repl::OpType_parse(IDLParserErrorContext("ChangeStreamEntry.op"),
                              opTypeField.getString());
}

Value makeChangeStreamNsField(const NamespaceString& nss) {
    return Value(Document{{"db", nss.db()}, {"coll", nss.coll()}});
}

void setResumeTokenForEvent(const ResumeTokenData& resumeTokenData, MutableDocument* doc) {
    auto resumeToken = Value(ResumeToken(resumeTokenData).toDocument());
    doc->addField(DocumentSourceChangeStream::kIdField, resumeToken);

    // We set the resume token as the document's sort key in both the sharded and non-sharded cases,
    // since we will subsequently rely upon it to generate a correct postBatchResumeToken.
    const bool isSingleElementKey = true;
    doc->metadata().setSortKey(resumeToken, isSingleElementKey);
}
}  // namespace

ChangeStreamEventTransformation::ChangeStreamEventTransformation(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const DocumentSourceChangeStreamSpec& spec)
    : _changeStreamSpec(spec), _expCtx(expCtx) {
    // Extract the resume token from the spec and store it.
    _resumeToken =
        DocumentSourceChangeStream::resolveResumeTokenFromSpec(_expCtx, _changeStreamSpec);

    // Determine whether the user requested a point-in-time pre-image, which will affect this
    // stage's output.
    _preImageRequested =
        _changeStreamSpec.getFullDocumentBeforeChange() != FullDocumentBeforeChangeModeEnum::kOff;

    // Determine whether the user requested a point-in-time post-image, which will affect this
    // stage's output.
    _postImageRequested =
        _changeStreamSpec.getFullDocument() == FullDocumentModeEnum::kWhenAvailable ||
        _changeStreamSpec.getFullDocument() == FullDocumentModeEnum::kRequired;
}

ResumeTokenData ChangeStreamEventTransformation::makeResumeToken(Value tsVal,
                                                                 Value txnOpIndexVal,
                                                                 Value uuidVal,
                                                                 StringData operationType,
                                                                 Value documentKey,
                                                                 Value opDescription) const {
    // Resolve the potentially-absent Value arguments to the expected resume token types.
    auto uuid = uuidVal.missing() ? boost::none : boost::optional<UUID>{uuidVal.getUuid()};
    size_t txnOpIndex = txnOpIndexVal.missing() ? 0 : txnOpIndexVal.getLong();
    auto clusterTime = tsVal.getTimestamp();

    // If we have a resume token, we need to match the version with which it was generated until we
    // have surpassed it, at which point we can begin generating tokens with our default version.
    auto version = (clusterTime > _resumeToken.clusterTime) ? _expCtx->changeStreamTokenVersion
                                                            : _resumeToken.version;

    // Construct and return the final resume token.
    return {clusterTime, version, txnOpIndex, uuid, operationType, documentKey, opDescription};
}

ChangeStreamDefaultEventTransformation::ChangeStreamDefaultEventTransformation(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const DocumentSourceChangeStreamSpec& spec)
    : ChangeStreamEventTransformation(expCtx, spec) {}

std::set<std::string> ChangeStreamDefaultEventTransformation::getFieldNameDependencies() const {
    std::set<std::string> accessedFields = {repl::OplogEntry::kOpTypeFieldName.toString(),
                                            repl::OplogEntry::kTimestampFieldName.toString(),
                                            repl::OplogEntry::kNssFieldName.toString(),
                                            repl::OplogEntry::kUuidFieldName.toString(),
                                            repl::OplogEntry::kObjectFieldName.toString(),
                                            repl::OplogEntry::kObject2FieldName.toString(),
                                            repl::OplogEntry::kSessionIdFieldName.toString(),
                                            repl::OplogEntry::kTxnNumberFieldName.toString(),
                                            DocumentSourceChangeStream::kTxnOpIndexField.toString(),
                                            repl::OplogEntry::kWallClockTimeFieldName.toString()};

    if (_preImageRequested || _postImageRequested) {
        accessedFields.insert(repl::OplogEntry::kPreImageOpTimeFieldName.toString());
        accessedFields.insert(DocumentSourceChangeStream::kApplyOpsIndexField.toString());
        accessedFields.insert(DocumentSourceChangeStream::kApplyOpsTsField.toString());
    }
    return accessedFields;
}

Document ChangeStreamDefaultEventTransformation::applyTransformation(const Document& input) const {
    MutableDocument doc;

    // Extract the fields we need.
    Value ts = input[repl::OplogEntry::kTimestampFieldName];
    Value ns = input[repl::OplogEntry::kNssFieldName];
    checkValueType(ns, repl::OplogEntry::kNssFieldName, BSONType::String);
    Value uuid = input[repl::OplogEntry::kUuidFieldName];
    auto opType = getOplogOpType(input);

    NamespaceString nss(ns.getString());
    Value id = input.getNestedField("o._id");
    // Non-replace updates have the _id in field "o2".
    StringData operationType;
    Value fullDocument;
    Value updateDescription;
    Value documentKey;
    Value operationDescription;
    Value stateBeforeChange;

    switch (opType) {
        case repl::OpTypeEnum::kInsert: {
            operationType = DocumentSourceChangeStream::kInsertOpType;
            fullDocument = input[repl::OplogEntry::kObjectFieldName];
            documentKey = input[repl::OplogEntry::kObject2FieldName];

            // For oplog entries written on an older version of the server (before 5.3), the
            // documentKey may be missing. This is an unlikely scenario to encounter on a post 6.0
            // node. We just default to _id as the only document key field for this case.
            if (documentKey.missing()) {
                documentKey = Value(Document{{"_id", id}});
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
                    std::vector<Value> removedFieldsVector;
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
                const auto renameTarget = makeChangeStreamNsField(renameTargetNss);

                // The 'to' field predates the 'operationDescription' field which was added in 5.3.
                // We keep the top-level 'to' field for backwards-compatibility.
                doc.addField(DocumentSourceChangeStream::kRenameTargetNssField, renameTarget);

                // Include full details of the rename in 'operationDescription'.
                MutableDocument opDescBuilder(
                    copyDocExceptFields(oField, {"renameCollection"_sd, "stayTemp"_sd}));
                opDescBuilder.setField(DocumentSourceChangeStream::kRenameTargetNssField,
                                       renameTarget);
                operationDescription = opDescBuilder.freezeToValue();
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
            } else if (auto nssField = oField.getField("collMod"); !nssField.missing()) {
                operationType = DocumentSourceChangeStream::kModifyOpType;
                nss = NamespaceString(nss.db(), nssField.getString());
                operationDescription = Value(copyDocExceptFields(oField, {"collMod"_sd}));

                const auto o2Field = input[repl::OplogEntry::kObject2FieldName].getDocument();
                stateBeforeChange =
                    Value(Document{{"collectionOptions", o2Field.getField("collectionOptions_old")},
                                   {"indexOptions", o2Field.getField("indexOptions_old")}});
            } else {
                // All other commands will invalidate the stream.
                operationType = DocumentSourceChangeStream::kInvalidateOpType;
            }

            // Make sure the result doesn't have a document key.
            documentKey = Value();
            break;
        }
        case repl::OpTypeEnum::kNoop: {
            // Check whether this is a shardCollection oplog entry.
            if (!input.getNestedField("o2.shardCollection").missing()) {
                const auto o2Field = input[repl::OplogEntry::kObject2FieldName].getDocument();
                operationType = DocumentSourceChangeStream::kShardCollectionOpType;
                operationDescription = Value(copyDocExceptFields(o2Field, {"shardCollection"_sd}));
                break;
            }

            // Check if this is a migration of the last chunk off a shard.
            if (!input.getNestedField("o2.migrateLastChunkFromShard").missing()) {
                const auto o2Field = input[repl::OplogEntry::kObject2FieldName].getDocument();
                operationType = DocumentSourceChangeStream::kMigrateLastChunkFromShardOpType;
                operationDescription =
                    Value(copyDocExceptFields(o2Field, {"migrateLastChunkFromShard"_sd}));
                break;
            }

            // Otherwise, o2.type determines the message type.
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
        default: { MONGO_UNREACHABLE_TASSERT(6330501); }
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
        // The lsid and txnNumber may be missing if this is a batched write.
        auto lsid = input[DocumentSourceChangeStream::kLsidField];
        checkValueTypeOrMissing(lsid, DocumentSourceChangeStream::kLsidField, BSONType::Object);
        auto txnNumber = input[DocumentSourceChangeStream::kTxnNumberField];
        checkValueTypeOrMissing(
            txnNumber, DocumentSourceChangeStream::kTxnNumberField, BSONType::NumberLong);
        doc.addField(DocumentSourceChangeStream::kTxnNumberField, txnNumber);
        doc.addField(DocumentSourceChangeStream::kLsidField, lsid);
    }

    // Generate the resume token. Note that only 'ts' is always guaranteed to be present.
    auto resumeTokenData =
        makeResumeToken(ts, txnOpIndex, uuid, operationType, documentKey, operationDescription);
    setResumeTokenForEvent(resumeTokenData, &doc);

    doc.addField(DocumentSourceChangeStream::kOperationTypeField, Value(operationType));
    doc.addField(DocumentSourceChangeStream::kClusterTimeField, Value(resumeTokenData.clusterTime));

    if (_changeStreamSpec.getShowExpandedEvents()) {
        // Note: If the UUID is a missing value (which can be true for events like 'dropDatabase'),
        // 'addField' will not add anything to the document.
        doc.addField(DocumentSourceChangeStream::kCollectionUuidField, uuid);
    }

    const auto wallTime = input[repl::OplogEntry::kWallClockTimeFieldName];
    checkValueType(wallTime, repl::OplogEntry::kWallClockTimeFieldName, BSONType::Date);
    doc.addField(DocumentSourceChangeStream::kWallTimeField, wallTime);

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
                     : makeChangeStreamNsField(nss));

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

    // For a 'modify' event we add the state before modification if appropriate.
    doc.addField(DocumentSourceChangeStream::kStateBeforeChangeField, stateBeforeChange);

    return doc.freeze();
}

ChangeStreamViewDefinitionEventTransformation::ChangeStreamViewDefinitionEventTransformation(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const DocumentSourceChangeStreamSpec& spec)
    : ChangeStreamEventTransformation(expCtx, spec) {}

std::set<std::string> ChangeStreamViewDefinitionEventTransformation::getFieldNameDependencies()
    const {
    return std::set<std::string>{repl::OplogEntry::kOpTypeFieldName.toString(),
                                 repl::OplogEntry::kTimestampFieldName.toString(),
                                 repl::OplogEntry::kUuidFieldName.toString(),
                                 repl::OplogEntry::kObjectFieldName.toString(),
                                 DocumentSourceChangeStream::kTxnOpIndexField.toString(),
                                 repl::OplogEntry::kWallClockTimeFieldName.toString()};
}

Document ChangeStreamViewDefinitionEventTransformation::applyTransformation(
    const Document& input) const {
    Value ts = input[repl::OplogEntry::kTimestampFieldName];
    auto opType = getOplogOpType(input);

    StringData operationType;
    Value operationDescription;

    Document oField = input[repl::OplogEntry::kObjectFieldName].getDocument();
    switch (opType) {
        case repl::OpTypeEnum::kInsert: {
            operationType = DocumentSourceChangeStream::kCreateOpType;
            operationDescription = Value(copyDocExceptFields(oField, {"_id"_sd}));
            break;
        }
        case repl::OpTypeEnum::kUpdate: {
            // To be able to generate a 'modify' event, we need the collMod of a view definition to
            // always log the update as replacement.
            tassert(6188601, "Expected replacement update", !oField["_id"].missing());

            operationType = DocumentSourceChangeStream::kModifyOpType;
            operationDescription = Value(copyDocExceptFields(oField, {"_id"_sd}));
            break;
        }
        case repl::OpTypeEnum::kDelete: {
            operationType = DocumentSourceChangeStream::kDropCollectionOpType;
            break;
        }
        default: {
            // We shouldn't see an op other than insert, update or delete.
            MONGO_UNREACHABLE_TASSERT(6188600);
        }
    };

    auto resumeTokenData = makeResumeToken(ts,
                                           input[DocumentSourceChangeStream::kTxnOpIndexField],
                                           input[repl::OplogEntry::kUuidFieldName],
                                           operationType,
                                           Value(),
                                           operationDescription);

    MutableDocument doc;
    setResumeTokenForEvent(resumeTokenData, &doc);
    doc.addField(DocumentSourceChangeStream::kOperationTypeField, Value(operationType));
    doc.addField(DocumentSourceChangeStream::kClusterTimeField, Value(resumeTokenData.clusterTime));
    doc.addField(DocumentSourceChangeStream::kWallTimeField,
                 input[repl::OplogEntry::kWallClockTimeFieldName]);

    // The 'o._id' is the full namespace string of the view.
    const auto nss = NamespaceString(oField["_id"].getString());
    doc.addField(DocumentSourceChangeStream::kNamespaceField, makeChangeStreamNsField(nss));
    doc.addField(DocumentSourceChangeStream::kOperationDescriptionField, operationDescription);

    return doc.freeze();
}

ChangeStreamEventTransformer::ChangeStreamEventTransformer(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const DocumentSourceChangeStreamSpec& spec) {
    _defaultEventBuilder = std::make_unique<ChangeStreamDefaultEventTransformation>(expCtx, spec);
    _viewNsEventBuilder =
        std::make_unique<ChangeStreamViewDefinitionEventTransformation>(expCtx, spec);
    _isSingleCollStream = DocumentSourceChangeStream::getChangeStreamType(expCtx->ns) ==
        DocumentSourceChangeStream::ChangeStreamType::kSingleCollection;
}

ChangeStreamEventTransformation* ChangeStreamEventTransformer::getBuilder(
    const Document& oplog) const {
    auto nss = NamespaceString(oplog[repl::OplogEntry::kNssFieldName].getStringData());
    if (!_isSingleCollStream && nss.isSystemDotViews()) {
        return _viewNsEventBuilder.get();
    }
    return _defaultEventBuilder.get();
}

}  // namespace mongo
