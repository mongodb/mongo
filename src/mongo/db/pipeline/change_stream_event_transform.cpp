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

#include "mongo/bson/bsontypes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/change_stream.h"
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

#include <array>
#include <cstddef>
#include <initializer_list>
#include <utility>
#include <vector>

#include <boost/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>


namespace mongo {
namespace {
constexpr auto checkValueType = &DocumentSourceChangeStream::checkValueType;
constexpr auto checkValueTypeOrMissing = &DocumentSourceChangeStream::checkValueTypeOrMissing;
constexpr auto resolveResumeToken = &change_stream::resolveResumeTokenFromSpec;

constexpr std::array kBuiltInNoopEvents = {
    DocumentSourceChangeStream::kShardCollectionOpType,
    DocumentSourceChangeStream::kMigrateLastChunkFromShardOpType,
    DocumentSourceChangeStream::kRefineCollectionShardKeyOpType,
    DocumentSourceChangeStream::kReshardCollectionOpType,
    DocumentSourceChangeStream::kNewShardDetectedOpType,
    DocumentSourceChangeStream::kReshardBeginOpType,
    DocumentSourceChangeStream::kReshardBlockingWritesOpType,
    DocumentSourceChangeStream::kReshardDoneCatchUpOpType,
    DocumentSourceChangeStream::kEndOfTransactionOpType};

const StringDataSet kOpsWithoutUUID = {
    DocumentSourceChangeStream::kInvalidateOpType,
    DocumentSourceChangeStream::kDropDatabaseOpType,
    DocumentSourceChangeStream::kEndOfTransactionOpType,
};

const StringDataSet kOpsWithoutNs = {
    DocumentSourceChangeStream::kEndOfTransactionOpType,
};

const StringDataSet kOpsWithReshardingUUIDs = {
    DocumentSourceChangeStream::kReshardBeginOpType,
    DocumentSourceChangeStream::kReshardBlockingWritesOpType,
    DocumentSourceChangeStream::kReshardDoneCatchUpOpType,
};

const StringDataSet kPreImageOps = {DocumentSourceChangeStream::kUpdateOpType,
                                    DocumentSourceChangeStream::kReplaceOpType,
                                    DocumentSourceChangeStream::kDeleteOpType};
const StringDataSet kPostImageOps = {DocumentSourceChangeStream::kUpdateOpType};

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
            viewOn.missing() || viewOn.getType() == BSONType::string);
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

Document copyDocExceptFields(const Document& source, std::initializer_list<StringData> fieldNames) {
    MutableDocument doc(source);
    for (auto fieldName : fieldNames) {
        doc.remove(fieldName);
    }
    return doc.freeze();
}

repl::OpTypeEnum getOplogOpType(const Document& oplog) {
    auto opTypeField = oplog[repl::OplogEntry::kOpTypeFieldName];
    checkValueType(opTypeField, repl::OplogEntry::kOpTypeFieldName, BSONType::string);
    return repl::OpType_parse(opTypeField.getString(), IDLParserContext("ChangeStreamEntry.op"));
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
    constexpr bool isSingleElementKey = true;
    doc->metadata().setSortKey(resumeToken, isSingleElementKey);
}

NamespaceString createNamespaceStringFromOplogEntry(Value tid, StringData ns) {
    auto tenantId = tid.missing() ? boost::none : boost::optional<TenantId>{tid.getOid()};
    return NamespaceStringUtil::deserialize(tenantId, ns, SerializationContext::stateDefault());
}

void addTransactionIdFieldsIfPresent(const Document& input, MutableDocument& output) {
    // The lsid and txnNumber may be missing if this is a batched write.
    auto lsid = input[DocumentSourceChangeStream::kLsidField];
    checkValueTypeOrMissing(lsid, DocumentSourceChangeStream::kLsidField, BSONType::object);
    auto txnNumber = input[DocumentSourceChangeStream::kTxnNumberField];
    checkValueTypeOrMissing(
        txnNumber, DocumentSourceChangeStream::kTxnNumberField, BSONType::numberLong);
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
        ? _expCtx->getChangeStreamTokenVersion()
        : _resumeToken.version;

    // Construct and return the final resume token.
    return {clusterTime, version, txnOpIndex, uuid, operationType, documentKey, opDescription};
}

ChangeStreamDefaultEventTransformation::ChangeStreamDefaultEventTransformation(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const DocumentSourceChangeStreamSpec& spec)
    : ChangeStreamEventTransformation(expCtx, spec) {
    _supportedEvents = buildSupportedEvents();
}

ChangeStreamDefaultEventTransformation::SupportedEvents
ChangeStreamDefaultEventTransformation::buildSupportedEvents() const {
    SupportedEvents result;

    // Check if field 'supportedEvents' is present, and handle it if so.
    if (auto supportedEvents = _changeStreamSpec.getSupportedEvents()) {
        // Ensure that event names in 'supportedEvents' are unique.
        for (auto&& supportedEvent : *supportedEvents) {
            uassert(10498500,
                    "Expecting valid, unique event names in 'supportedEvents'",
                    !supportedEvent.empty() && result.insert(supportedEvent).second);
        }
    }


    // Add built-in sharding events to list of noop events we need to handle.
    result.insert(kBuiltInNoopEvents.begin(), kBuiltInNoopEvents.end());

    return result;
}

std::set<std::string> ChangeStreamDefaultEventTransformation::getFieldNameDependencies() const {
    std::set<std::string> accessedFields = {
        std::string{repl::OplogEntry::kOpTypeFieldName},
        std::string{repl::OplogEntry::kTimestampFieldName},
        std::string{repl::OplogEntry::kNssFieldName},
        std::string{repl::OplogEntry::kUuidFieldName},
        std::string{repl::OplogEntry::kObjectFieldName},
        std::string{repl::OplogEntry::kObject2FieldName},
        std::string{repl::OplogEntry::kSessionIdFieldName},
        std::string{repl::OplogEntry::kTxnNumberFieldName},
        std::string{DocumentSourceChangeStream::kTxnOpIndexField},
        std::string{repl::OplogEntry::kWallClockTimeFieldName},
        std::string{DocumentSourceChangeStream::kCommitTimestampField},
        std::string{repl::OplogEntry::kTidFieldName}};

    if (_preImageRequested || _postImageRequested) {
        accessedFields.insert(std::string{DocumentSourceChangeStream::kApplyOpsIndexField});
        accessedFields.insert(std::string{DocumentSourceChangeStream::kApplyOpsTsField});
    }
    return accessedFields;
}

Document ChangeStreamDefaultEventTransformation::applyTransformation(const Document& input) const {
    // Extract the fields we need.
    Value ts = input[repl::OplogEntry::kTimestampFieldName];
    Value ns = input[repl::OplogEntry::kNssFieldName];
    Value tenantId = input[repl::OplogEntry::kTidFieldName];
    checkValueType(ns, repl::OplogEntry::kNssFieldName, BSONType::string);
    Value uuid = input[repl::OplogEntry::kUuidFieldName];
    auto opType = getOplogOpType(input);

    NamespaceString nss = createNamespaceStringFromOplogEntry(tenantId, ns.getStringData());
    Value id = input.getNestedField("o._id");
    // Non-replace updates have the _id in field "o2".
    StringData operationType;
    Value fullDocument;
    Value updateDescription;
    Value documentKey;
    // Used to populate the 'operationDescription' output field and also to build the resumeToken
    // for some events. Note that any change to the 'operationDescription' for existing events can
    // break changestream resumability between different mongod versions and should thus be avoided!
    Value operationDescription;
    Value stateBeforeChange;
    // Optional value containing the namespace type for changestream create events. This will be
    // emitted as 'nsType' field.
    Value nsType;

    MutableDocument doc;

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
                               BSONType::object);

                if (_changeStreamSpec.getShowRawUpdateDescription()) {
                    updateDescription = input[repl::OplogEntry::kObjectFieldName];
                } else {
                    auto deltaDesc = change_stream_document_diff_parser::parseDiff(
                        diffObj.getDocument().toBson());

                    // If the 'showExpandedEvents' flag is set, the update description will also
                    // contain the 'disambiguatedPaths' sub-field. The field will not be emitted
                    // otherwise.
                    if (_changeStreamSpec.getShowExpandedEvents()) {
                        updateDescription = Value(Document{
                            {"updatedFields", std::move(deltaDesc.updatedFields)},
                            {"removedFields", std::move(deltaDesc.removedFields)},
                            {"truncatedArrays", std::move(deltaDesc.truncatedArrays)},
                            {"disambiguatedPaths", std::move(deltaDesc.disambiguatedPaths)}});
                    } else {
                        updateDescription = Value(
                            Document{{"updatedFields", std::move(deltaDesc.updatedFields)},
                                     {"removedFields", std::move(deltaDesc.removedFields)},
                                     {"truncatedArrays", std::move(deltaDesc.truncatedArrays)}});
                    }
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
                operationType = DocumentSourceChangeStream::kCreateOpType;
                nss = NamespaceStringUtil::deserialize(nss.dbName(), nssField.getStringData());
                Document opDesc = copyDocExceptFields(oField, {"create"_sd});
                operationDescription = Value(opDesc);

                // Populate 'nsType' field with collection type (always "collection" here).
                auto collectionType = determineCollectionType(oField, nss.dbName());
                tassert(8814201,
                        "'operationDescription.type' should always resolve to 'collection' for "
                        "collection create events",
                        collectionType == CollectionType::kCollection);
                nsType = Value(toString(collectionType));
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
            } else if (auto nssField = oField.getField("abortIndexBuild"); !nssField.missing()) {
                operationType = DocumentSourceChangeStream::kAbortIndexBuildOpType;
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

            // Check for dynamic events that were specified via the 'supportedEvents' change stream
            // parameter.
            // This also checks for some hard-code sharding-related events.
            if (auto result = handleSupportedEvent(o2Field)) {
                // Apply returned event name and operationDescription.
                operationType = result->first;
                operationDescription = result->second;

                // Check if the 'reshardingUUID' field needs to be added to the event.
                if (kOpsWithReshardingUUIDs.contains(operationType)) {
                    doc.addField(DocumentSourceChangeStream::kReshardingUuidField,
                                 o2Field[DocumentSourceChangeStream::kReshardingUuidField]);
                }

                // Check if the 'txnNumber' and 'lsid' fields need to be added to the event. This is
                // currently only true for 'endOfTransaction' events.
                if (operationType == DocumentSourceChangeStream::kEndOfTransactionOpType) {
                    addTransactionIdFieldsIfPresent(o2Field, doc);
                }
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
    // All of the extra event types specified in the 'supportedEvents' change stream parameter
    // require the UUID field to be set in their oplog entry. Otherwise the following tassert will
    // trigger.
    tassert(7826901,
            "Saw a CRUD op without a UUID",
            !uuid.missing() || kOpsWithoutUUID.contains(operationType));

    // Extract the 'txnOpIndex' field. This will be missing unless we are unwinding a transaction.
    auto txnOpIndex = input[DocumentSourceChangeStream::kTxnOpIndexField];

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

    if (_changeStreamSpec.getShowCommitTimestamp()) {
        // Commit timestamp for CRUD events in prepared transactions.
        auto commitTimestamp = input[DocumentSourceChangeStream::kCommitTimestampField];
        if (!commitTimestamp.missing()) {
            doc.addField(DocumentSourceChangeStream::kCommitTimestampField, commitTimestamp);
        }
    }

    if (_changeStreamSpec.getShowExpandedEvents() && !uuid.missing()) {
        doc.addField(DocumentSourceChangeStream::kCollectionUuidField, uuid);
    }

    const auto wallTime = input[repl::OplogEntry::kWallClockTimeFieldName];
    checkValueType(wallTime, repl::OplogEntry::kWallClockTimeFieldName, BSONType::date);
    doc.addField(DocumentSourceChangeStream::kWallTimeField, wallTime);

    // Add the post-image, pre-image id, namespace, documentKey and other fields as appropriate.
    doc.addField(DocumentSourceChangeStream::kFullDocumentField, std::move(fullDocument));

    // Determine whether the preImageId should be included, for eligible operations. Note that we
    // will include preImageId even if the user requested a post-image but no pre-image, because the
    // pre-image is required to compute the post-image.
    if ((_preImageRequested && kPreImageOps.count(operationType)) ||
        (_postImageRequested && kPostImageOps.count(operationType))) {
        // Extract the 'applyOpsIndex' and 'applyOpsTs' fields. These will be missing unless we are
        // unwinding a transaction.
        auto applyOpsIndex = input[DocumentSourceChangeStream::kApplyOpsIndexField];
        auto applyOpsEntryTs = input[DocumentSourceChangeStream::kApplyOpsTsField];

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
    if (_changeStreamSpec.getShowExpandedEvents() && !operationDescription.missing()) {
        doc.addField(DocumentSourceChangeStream::kOperationDescriptionField,
                     std::move(operationDescription));
    }

    // Note that the update description field might be the 'missing' value, in which case it will
    // not be serialized.
    if (!updateDescription.missing()) {
        auto updateDescriptionFieldName = _changeStreamSpec.getShowRawUpdateDescription()
            ? DocumentSourceChangeStream::kRawUpdateDescriptionField
            : DocumentSourceChangeStream::kUpdateDescriptionField;
        doc.addField(updateDescriptionFieldName, std::move(updateDescription));
    }

    // For a 'modify' event we add the state before modification if appropriate.
    if (!stateBeforeChange.missing()) {
        doc.addField(DocumentSourceChangeStream::kStateBeforeChangeField, stateBeforeChange);
    }

    if (!nsType.missing()) {
        doc.addField(DocumentSourceChangeStream::kNsTypeField, nsType);
    }

    return doc.freeze();
}

boost::optional<std::pair<StringData, Value>>
ChangeStreamDefaultEventTransformation::handleSupportedEvent(const Document& o2Field) const {
    for (auto&& supportedEvent : _supportedEvents) {
        if (auto lookup = o2Field[supportedEvent]; !o2Field[supportedEvent].missing()) {
            // Known event.
            return std::make_pair(supportedEvent,
                                  Value{copyDocExceptFields(o2Field, {supportedEvent})});
        }
    }
    return boost::none;
}

ChangeStreamViewDefinitionEventTransformation::ChangeStreamViewDefinitionEventTransformation(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const DocumentSourceChangeStreamSpec& spec)
    : ChangeStreamEventTransformation(expCtx, spec) {}

std::set<std::string> ChangeStreamViewDefinitionEventTransformation::getFieldNameDependencies()
    const {
    return std::set<std::string>{std::string{repl::OplogEntry::kOpTypeFieldName},
                                 std::string{repl::OplogEntry::kTimestampFieldName},
                                 std::string{repl::OplogEntry::kUuidFieldName},
                                 std::string{repl::OplogEntry::kObjectFieldName},
                                 std::string{DocumentSourceChangeStream::kTxnOpIndexField},
                                 std::string{repl::OplogEntry::kWallClockTimeFieldName},
                                 std::string{repl::OplogEntry::kTidFieldName}};
}

Document ChangeStreamViewDefinitionEventTransformation::applyTransformation(
    const Document& input) const {
    Value ts = input[repl::OplogEntry::kTimestampFieldName];
    auto opType = getOplogOpType(input);
    Value tenantId = input[repl::OplogEntry::kTidFieldName];

    StringData operationType;
    // Used to populate the 'operationDescription' output field and also to build the resumeToken
    // for some events. Note that any change to the 'operationDescription' for existing events can
    // break changestream resumability between different mongod versions and should thus be avoided!
    Value operationDescription;
    // Optional value containing the namespace type for changestream create events. This will be
    // emitted as 'nsType' field.
    Value nsType;

    // For views, we are transforming the DML operation on the system.views
    // collection into a DDL event as follows:
    // - insert into system.views is turned into a create (collection) event.
    // - update in system.views is turned into a (collection) modify event.
    // - delete in system.views is turned into a drop (collection) event.
    Document oField = input[repl::OplogEntry::kObjectFieldName].getDocument();

    // The 'o._id' is the full namespace string of the view.
    const auto nss = createNamespaceStringFromOplogEntry(tenantId, oField["_id"].getStringData());

    // Note: we are intentionally *not* handling any configurable events from the 'supportedEvents'
    // change stream parameter for view-type events here. Handling these events makes no sense here,
    // as the view event transformer will only handle CRUD oplog events in the "system.views"
    // namespace and cannot handle any "noop" oplog entries at all.
    switch (opType) {
        case repl::OpTypeEnum::kInsert: {
            operationType = DocumentSourceChangeStream::kCreateOpType;
            Document opDesc = copyDocExceptFields(oField, {"_id"_sd});
            operationDescription = Value(opDesc);

            if (_changeStreamSpec.getShowExpandedEvents()) {
                // Populate 'nsType' field with either "view" or "timeseries".
                auto collectionType = determineCollectionType(oField, nss.dbName());
                tassert(
                    8814202,
                    "'operationDescription.type' should always resolve to 'view' or 'timeseries' "
                    "for view creation event",
                    collectionType == CollectionType::kView ||
                        collectionType == CollectionType::kTimeseries);
                nsType = Value(toString(collectionType));
            }
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
    }

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

    if (!nsType.missing()) {
        doc.addField(DocumentSourceChangeStream::kNsTypeField, nsType);
    }

    return doc.freeze();
}

ChangeStreamEventTransformer::ChangeStreamEventTransformer(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const DocumentSourceChangeStreamSpec& spec)
    : _defaultEventBuilder(std::make_unique<ChangeStreamDefaultEventTransformation>(expCtx, spec)),
      _viewNsEventBuilder(
          std::make_unique<ChangeStreamViewDefinitionEventTransformation>(expCtx, spec)),
      _isSingleCollStream(ChangeStream::getChangeStreamType(expCtx->getNamespaceString()) ==
                          ChangeStreamType::kCollection) {}

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
