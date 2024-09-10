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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <utility>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/bson/bsontypes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/change_stream_document_diff_parser.h"
#include "mongo/db/pipeline/change_stream_helpers.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/db/update/update_oplog_entry_version.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {
constexpr auto checkValueType = &DocumentSourceChangeStream::checkValueType;
constexpr auto checkValueTypeOrMissing = &DocumentSourceChangeStream::checkValueTypeOrMissing;
constexpr auto resolveResumeToken = &change_stream::resolveResumeTokenFromSpec;

const StringDataSet kOpsWithoutUUID = {
    DocumentSourceChangeStream::kInvalidateOpType,
    DocumentSourceChangeStream::kDropDatabaseOpType,
    DocumentSourceChangeStream::kEndOfTransactionOpType,
};

const StringDataSet kOpsWithoutNs = {
    DocumentSourceChangeStream::kEndOfTransactionOpType,
};

// Possible collection types, for the "type" field returned by collection / view create events.
enum class CollectionType {
    kCollection,
    kView,
    kTimeseries,
};

// Stringification for CollectionType.
StringData toString(CollectionType type) {
    switch (type) {
        case CollectionType::kCollection:
            return "collection"_sd;
        case CollectionType::kView:
            return "view"_sd;
        case CollectionType::kTimeseries:
            return "timeseries"_sd;
    }
    MONGO_UNREACHABLE_TASSERT(8814200);
}

// Determine type of collection / view created, based on oplog entry payload.
// Defaults to 'kCollection', and is changed to kView if "viewOn" field is set, except if "viewOn"
// indicates that it is a timeseries collection. In the latter case kTimeseries is returned.
CollectionType determineCollectionType(const Document& data, const DatabaseName& dbName) {
    Value viewOn = data.getField("viewOn");
    tassert(8814203,
            "'viewOn' should either be missing or a non-empty string",
            viewOn.missing() || viewOn.getType() == BSONType::String);
    if (viewOn.missing()) {
        return CollectionType::kCollection;
    }
    StringData viewOnNss = viewOn.getStringData();
    tassert(8814204, "'viewOn' should be a non-empty string", !viewOnNss.empty());
    if (NamespaceString nss = NamespaceStringUtil::deserialize(dbName, viewOnNss);
        nss.isTimeseriesBucketsCollection()) {
        return CollectionType::kTimeseries;
    }
    return CollectionType::kView;
}

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
    return repl::OpType_parse(IDLParserContext("ChangeStreamEntry.op"), opTypeField.getString());
}

Value makeChangeStreamNsField(const NamespaceString& nss) {
    // For certain types, such as dropDatabase, the collection name may be empty and should be
    // omitted. We never report the NamespaceString's tenantId in change stream events.
    return Value(Document{{"db", nss.dbName().serializeWithoutTenantPrefix_UNSAFE()},
                          {"coll", (nss.coll().empty() ? Value() : Value(nss.coll()))}});
}

void setResumeTokenForEvent(const ResumeTokenData& resumeTokenData, MutableDocument* doc) {
    auto resumeToken = Value(ResumeToken(resumeTokenData).toDocument());
    doc->addField(DocumentSourceChangeStream::kIdField, resumeToken);

    // We set the resume token as the document's sort key in both the sharded and non-sharded cases,
    // since we will subsequently rely upon it to generate a correct postBatchResumeToken.
    const bool isSingleElementKey = true;
    doc->metadata().setSortKey(resumeToken, isSingleElementKey);
}

NamespaceString createNamespaceStringFromOplogEntry(Value tid, StringData ns) {
    auto tenantId = tid.missing() ? boost::none : boost::optional<TenantId>{tid.getOid()};
    return NamespaceStringUtil::deserialize(tenantId, ns, SerializationContext::stateDefault());
}

void addTransactionIdFieldsIfPresent(const Document& input, MutableDocument& output) {
    // The lsid and txnNumber may be missing if this is a batched write.
    auto lsid = input[DocumentSourceChangeStream::kLsidField];
    checkValueTypeOrMissing(lsid, DocumentSourceChangeStream::kLsidField, BSONType::Object);
    auto txnNumber = input[DocumentSourceChangeStream::kTxnNumberField];
    checkValueTypeOrMissing(
        txnNumber, DocumentSourceChangeStream::kTxnNumberField, BSONType::NumberLong);
    // We are careful here not to overwrite existing lsid or txnNumber fields with MISSING.
    if (!txnNumber.missing()) {
        output.addField(DocumentSourceChangeStream::kTxnNumberField, txnNumber);
    }
    if (!lsid.missing()) {
        output.addField(DocumentSourceChangeStream::kLsidField, lsid);
    }
}

}  // namespace

ChangeStreamEventTransformation::ChangeStreamEventTransformation(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const DocumentSourceChangeStreamSpec& spec)
    : _changeStreamSpec(spec), _expCtx(expCtx), _resumeToken(resolveResumeToken(expCtx, spec)) {
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
    // have surpassed all events against which it may have been compared in the original stream, at
    // which point we can begin generating tokens with our default version.
    auto version = (clusterTime > _resumeToken.clusterTime || txnOpIndex > _resumeToken.txnOpIndex)
        ? _expCtx->changeStreamTokenVersion
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
                                            repl::OplogEntry::kWallClockTimeFieldName.toString(),
                                            repl::OplogEntry::kTidFieldName.toString()};

    if (_preImageRequested || _postImageRequested) {
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
    Value tenantId = input[repl::OplogEntry::kTidFieldName];
    checkValueType(ns, repl::OplogEntry::kNssFieldName, BSONType::String);
    Value uuid = input[repl::OplogEntry::kUuidFieldName];
    auto opType = getOplogOpType(input);

    NamespaceString nss = createNamespaceStringFromOplogEntry(tenantId, ns.getStringData());
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
                    auto deltaDesc = change_stream_document_diff_parser::parseDiff(
                        diffObj.getDocument().toBson());

                    updateDescription =
                        Value(Document{{"updatedFields", std::move(deltaDesc.updatedFields)},
                                       {"removedFields", std::move(deltaDesc.removedFields)},
                                       {"truncatedArrays", std::move(deltaDesc.truncatedArrays)},
                                       {"disambiguatedPaths",
                                        _changeStreamSpec.getShowExpandedEvents()
                                            ? Value(std::move(deltaDesc.disambiguatedPaths))
                                            : Value()}});
                }
            } else if (!oplogVersion.missing() || id.missing()) {
                // This is not a replacement op, and we did not see a valid update version number.
                uasserted(6741200,
                          str::stream() << "Unsupported or missing oplog version, $v: "
                                        << oplogVersion.toString());
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
                nss = NamespaceStringUtil::deserialize(nss.dbName(), nssField.getStringData());
            } else if (auto nssField = oField.getField("renameCollection"); !nssField.missing()) {
                operationType = DocumentSourceChangeStream::kRenameCollectionOpType;

                // The "o.renameCollection" field contains the namespace of the original collection.
                nss = createNamespaceStringFromOplogEntry(tenantId, nssField.getStringData());

                // The "to" field contains the target namespace for the rename.
                const auto renameTargetNss =
                    createNamespaceStringFromOplogEntry(tenantId, oField["to"].getStringData());
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
                nss = NamespaceString(nss.dbName());
            } else if (auto nssField = oField.getField("create"); !nssField.missing()) {
                auto collectionType = determineCollectionType(oField, nss.dbName());
                operationType = DocumentSourceChangeStream::kCreateOpType;
                nss = NamespaceStringUtil::deserialize(nss.dbName(), nssField.getStringData());

                tassert(8814201,
                        "'operationDescription.type' should always resolve to 'collection' for "
                        "collection create events",
                        collectionType == CollectionType::kCollection);

                // Include full details of the create in 'operationDescription'.
                MutableDocument opDescBuilder(copyDocExceptFields(oField, {"create"_sd}));
                opDescBuilder.setField(DocumentSourceChangeStream::kCollectionTypeField,
                                       Value(toString(collectionType)));
                operationDescription = opDescBuilder.freezeToValue();
            } else if (auto nssField = oField.getField("createIndexes"); !nssField.missing()) {
                operationType = DocumentSourceChangeStream::kCreateIndexesOpType;
                nss = NamespaceStringUtil::deserialize(nss.dbName(), nssField.getStringData());
                // Wrap the index spec in an "indexes" array for consistency with commitIndexBuild.
                auto indexSpec = Value(copyDocExceptFields(oField, {"createIndexes"_sd}));
                operationDescription = Value(Document{{"indexes", std::vector<Value>{indexSpec}}});
            } else if (auto nssField = oField.getField("commitIndexBuild"); !nssField.missing()) {
                operationType = DocumentSourceChangeStream::kCreateIndexesOpType;
                nss = NamespaceStringUtil::deserialize(nss.dbName(), nssField.getStringData());
                operationDescription = Value(Document{{"indexes", oField.getField("indexes")}});
            } else if (auto nssField = oField.getField("startIndexBuild"); !nssField.missing()) {
                operationType = DocumentSourceChangeStream::kStartIndexBuildOpType;
                nss = NamespaceStringUtil::deserialize(nss.dbName(), nssField.getStringData());
                operationDescription = Value(Document{{"indexes", oField.getField("indexes")}});
            } else if (auto nssField = oField.getField("dropIndexes"); !nssField.missing()) {
                const auto o2Field = input[repl::OplogEntry::kObject2FieldName].getDocument();
                operationType = DocumentSourceChangeStream::kDropIndexesOpType;
                nss = NamespaceStringUtil::deserialize(nss.dbName(), nssField.getStringData());
                // Wrap the index spec in an "indexes" array for consistency with createIndexes
                // and commitIndexBuild.
                auto indexSpec = Value(copyDocExceptFields(o2Field, {"dropIndexes"_sd}));
                operationDescription = Value(Document{{"indexes", std::vector<Value>{indexSpec}}});
            } else if (auto nssField = oField.getField("collMod"); !nssField.missing()) {
                operationType = DocumentSourceChangeStream::kModifyOpType;
                nss = NamespaceStringUtil::deserialize(nss.dbName(), nssField.getStringData());
                operationDescription = Value(copyDocExceptFields(oField, {"collMod"_sd}));

                const auto o2Field = input[repl::OplogEntry::kObject2FieldName].getDocument();
                stateBeforeChange =
                    Value(Document{{"collectionOptions", o2Field.getField("collectionOptions_old")},
                                   {"indexOptions", o2Field.getField("indexOptions_old")}});
            } else {
                // We should never see an unknown command.
                MONGO_UNREACHABLE_TASSERT(6654400);
            }

            // Make sure the result doesn't have a document key.
            documentKey = Value();
            break;
        }
        case repl::OpTypeEnum::kNoop: {
            const auto o2Field = input[repl::OplogEntry::kObject2FieldName].getDocument();

            // Check whether this is a shardCollection oplog entry.
            if (!o2Field["shardCollection"].missing()) {
                operationType = DocumentSourceChangeStream::kShardCollectionOpType;
                operationDescription = Value(copyDocExceptFields(o2Field, {"shardCollection"_sd}));
                break;
            }

            // Check if this is a migration of the last chunk off a shard.
            if (!o2Field["migrateLastChunkFromShard"].missing()) {
                operationType = DocumentSourceChangeStream::kMigrateLastChunkFromShardOpType;
                operationDescription =
                    Value(copyDocExceptFields(o2Field, {"migrateLastChunkFromShard"_sd}));
                break;
            }

            // Check whether this is a refineCollectionShardKey oplog entry.
            if (!o2Field["refineCollectionShardKey"].missing()) {
                operationType = DocumentSourceChangeStream::kRefineCollectionShardKeyOpType;
                operationDescription =
                    Value(copyDocExceptFields(o2Field, {"refineCollectionShardKey"_sd}));
                break;
            }

            // Check whether this is a reshardCollection oplog entry.
            if (!o2Field["reshardCollection"].missing()) {
                operationType = DocumentSourceChangeStream::kReshardCollectionOpType;
                operationDescription =
                    Value(copyDocExceptFields(o2Field, {"reshardCollection"_sd}));
                break;
            }

            if (!o2Field["migrateChunkToNewShard"].missing()) {
                operationType = DocumentSourceChangeStream::kNewShardDetectedOpType;
                operationDescription =
                    Value(copyDocExceptFields(o2Field, {"migrateChunkToNewShard"_sd}));
                break;
            }

            if (!o2Field["reshardBegin"].missing()) {
                operationType = DocumentSourceChangeStream::kReshardBeginOpType;
                doc.addField(DocumentSourceChangeStream::kReshardingUuidField,
                             o2Field["reshardingUUID"]);
                operationDescription = Value(copyDocExceptFields(o2Field, {"reshardBegin"_sd}));
                break;
            }

            if (!o2Field["reshardDoneCatchUp"].missing()) {
                operationType = DocumentSourceChangeStream::kReshardDoneCatchUpOpType;
                doc.addField(DocumentSourceChangeStream::kReshardingUuidField,
                             o2Field["reshardingUUID"]);
                operationDescription =
                    Value(copyDocExceptFields(o2Field, {"reshardDoneCatchUp"_sd}));
                break;
            }

            if (!o2Field["endOfTransaction"].missing()) {
                operationType = DocumentSourceChangeStream::kEndOfTransactionOpType;
                operationDescription = Value{copyDocExceptFields(o2Field, {"endOfTransaction"_sd})};
                addTransactionIdFieldsIfPresent(o2Field, doc);
                break;
            }

            // We should never see an unknown noop entry.
            MONGO_UNREACHABLE_TASSERT(5052201);
        }
        default: {
            MONGO_UNREACHABLE_TASSERT(6330501);
        }
    }

    // UUID should always be present except for a known set of types.
    tassert(7826901,
            "Saw a CRUD op without a UUID",
            !uuid.missing() || kOpsWithoutUUID.contains(operationType));

    // Extract the 'txnOpIndex' and 'applyOpsIndex' fields. These will be missing unless we are
    // unwinding a transaction.
    auto txnOpIndex = input[DocumentSourceChangeStream::kTxnOpIndexField];
    auto applyOpsIndex = input[DocumentSourceChangeStream::kApplyOpsIndexField];
    auto applyOpsEntryTs = input[DocumentSourceChangeStream::kApplyOpsTsField];

    // Add some additional fields only relevant to transactions.
    if (!txnOpIndex.missing()) {
        addTransactionIdFieldsIfPresent(input, doc);
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
        // Set 'kPreImageIdField' to the 'ChangeStreamPreImageId'. The DSCSAddPreImage stage
        // will use the id in order to fetch the pre-image from the pre-images collection.
        const auto preImageId = ChangeStreamPreImageId(
            uuid.getUuid(),
            applyOpsEntryTs.missing() ? ts.getTimestamp() : applyOpsEntryTs.getTimestamp(),
            applyOpsIndex.missing() ? 0 : applyOpsIndex.getLong());
        doc.addField(DocumentSourceChangeStream::kPreImageIdField, Value(preImageId.toBSON()));
    }

    // If needed, add the 'ns' field to the change stream document, based on the final value of nss.
    if (!kOpsWithoutNs.contains(operationType)) {
        doc.addField(DocumentSourceChangeStream::kNamespaceField, makeChangeStreamNsField(nss));
    }

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
                                 repl::OplogEntry::kWallClockTimeFieldName.toString(),
                                 repl::OplogEntry::kTidFieldName.toString()};
}

Document ChangeStreamViewDefinitionEventTransformation::applyTransformation(
    const Document& input) const {
    Value ts = input[repl::OplogEntry::kTimestampFieldName];
    auto opType = getOplogOpType(input);
    Value tenantId = input[repl::OplogEntry::kTidFieldName];

    StringData operationType;
    Value operationDescription;

    // For views, we are transforming the DML operation on the system.views
    // collection into a DDL event as follows:
    // - insert into system.views is turned into a create (collection) event.
    // - update in system.views is turned into a (collection) modify event.
    // - delete in system.views is turned into a drop (collection) event.
    Document oField = input[repl::OplogEntry::kObjectFieldName].getDocument();

    // The 'o._id' is the full namespace string of the view.
    const auto nss = createNamespaceStringFromOplogEntry(tenantId, oField["_id"].getStringData());

    switch (opType) {
        case repl::OpTypeEnum::kInsert: {
            auto collectionType = determineCollectionType(oField, nss.dbName());
            tassert(8814202,
                    "'operationDescription.type' should always resolve to 'view' or 'timeseries' "
                    "for view creation event",
                    collectionType == CollectionType::kView ||
                        collectionType == CollectionType::kTimeseries);

            operationType = DocumentSourceChangeStream::kCreateOpType;
            // Include full details of the create in 'operationDescription'.
            MutableDocument opDescBuilder(copyDocExceptFields(oField, {"_id"_sd}));
            opDescBuilder.setField(DocumentSourceChangeStream::kCollectionTypeField,
                                   Value(toString(collectionType)));
            operationDescription = opDescBuilder.freezeToValue();
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
    // The nss from the entry is only used here determine which type of transformation to use. This
    // is not dependent on the tenantId, so it is safe to ignore the tenantId in the oplog entry.
    // It is useful to avoid extracting the tenantId because we must make this determination for
    // every change stream event, and the check should therefore be as optimized as possible.

    if (!_isSingleCollStream &&
        NamespaceString::resolvesToSystemDotViews(
            oplog[repl::OplogEntry::kNssFieldName].getStringData())) {
        return _viewNsEventBuilder.get();
    }
    return _defaultEventBuilder.get();
}

}  // namespace mongo
