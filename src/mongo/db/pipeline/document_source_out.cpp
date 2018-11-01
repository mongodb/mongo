
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/pipeline/document_path_support.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/db/pipeline/document_source_out_gen.h"
#include "mongo/db/pipeline/document_source_out_in_place.h"
#include "mongo/db/pipeline/document_source_out_replace_coll.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangWhileBuildingDocumentSourceOutBatch);

using boost::intrusive_ptr;
using std::vector;

std::unique_ptr<DocumentSourceOut::LiteParsed> DocumentSourceOut::LiteParsed::parse(
    const AggregationRequest& request, const BSONElement& spec) {

    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "$out stage requires a string or object argument, but found "
                          << typeName(spec.type()),
            spec.type() == BSONType::String || spec.type() == BSONType::Object);

    NamespaceString targetNss;
    bool allowSharded;
    WriteModeEnum mode;
    if (spec.type() == BSONType::String) {
        targetNss = NamespaceString(request.getNamespaceString().db(), spec.valueStringData());
        allowSharded = false;
        mode = WriteModeEnum::kModeReplaceCollection;
    } else if (spec.type() == BSONType::Object) {
        auto outSpec =
            DocumentSourceOutSpec::parse(IDLParserErrorContext("$out"), spec.embeddedObject());

        if (auto targetDb = outSpec.getTargetDb()) {
            targetNss = NamespaceString(*targetDb, outSpec.getTargetCollection());
        } else {
            targetNss =
                NamespaceString(request.getNamespaceString().db(), outSpec.getTargetCollection());
        }

        mode = outSpec.getMode();

        // Sharded output collections are not allowed with mode "replaceCollection".
        allowSharded = mode != WriteModeEnum::kModeReplaceCollection;
    }

    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "Invalid $out target namespace, " << targetNss.ns(),
            targetNss.isValid());

    // All modes require the "insert" action.
    ActionSet actions{ActionType::insert};
    switch (mode) {
        case WriteModeEnum::kModeReplaceCollection:
            actions.addAction(ActionType::remove);
            break;
        case WriteModeEnum::kModeReplaceDocuments:
            actions.addAction(ActionType::update);
            break;
        case WriteModeEnum::kModeInsertDocuments:
            // "insertDocuments" mode only requires the "insert" action.
            break;
    }

    if (request.shouldBypassDocumentValidation()) {
        actions.addAction(ActionType::bypassDocumentValidation);
    }

    PrivilegeVector privileges{Privilege(ResourcePattern::forExactNamespace(targetNss), actions)};

    return stdx::make_unique<DocumentSourceOut::LiteParsed>(
        std::move(targetNss), std::move(privileges), allowSharded);
}

REGISTER_DOCUMENT_SOURCE(out,
                         DocumentSourceOut::LiteParsed::parse,
                         DocumentSourceOut::createFromBson);

const char* DocumentSourceOut::getSourceName() const {
    return "$out";
}

namespace {
/**
 * Parses the fields of the 'uniqueKey' from the user-specified 'obj' from the $out spec, returning
 * a set of field paths. Throws if 'obj' is invalid.
 */
std::set<FieldPath> parseUniqueKeyFromSpec(const BSONObj& obj) {
    std::set<FieldPath> uniqueKey;
    for (const auto& elem : obj) {
        uassert(ErrorCodes::TypeMismatch,
                str::stream() << "All fields of $out uniqueKey must be the number 1, but '"
                              << elem.fieldNameStringData()
                              << "' is of type "
                              << elem.type(),
                elem.isNumber());

        uassert(ErrorCodes::BadValue,
                str::stream() << "All fields of $out uniqueKey must be the number 1, but '"
                              << elem.fieldNameStringData()
                              << "' has the invalid value "
                              << elem.numberDouble(),
                elem.numberDouble() == 1.0);

        const auto res = uniqueKey.insert(FieldPath(elem.fieldNameStringData()));
        uassert(ErrorCodes::BadValue,
                str::stream() << "Found a duplicate field '" << elem.fieldNameStringData()
                              << "' in $out uniqueKey",
                res.second);
    }

    uassert(ErrorCodes::InvalidOptions,
            "If explicitly specifying $out uniqueKey, must include at least one field",
            uniqueKey.size() > 0);
    return uniqueKey;
}

/**
 * Extracts the fields of 'uniqueKey' from 'doc' and returns the key as a BSONObj. Throws if any
 * field of the 'uniqueKey' extracted from 'doc' is nullish or an array.
 */
BSONObj extractUniqueKeyFromDoc(const Document& doc, const std::set<FieldPath>& uniqueKey) {
    MutableDocument result;
    for (const auto& field : uniqueKey) {
        auto value = doc.getNestedField(field);
        uassert(50943,
                str::stream() << "$out write error: uniqueKey field '" << field.fullPath()
                              << "' is an array in the document '"
                              << doc.toString()
                              << "'",
                !value.isArray());
        uassert(
            50905,
            str::stream() << "$out write error: uniqueKey field '" << field.fullPath()
                          << "' cannot be missing, null, undefined or an array. Full document: '"
                          << doc.toString()
                          << "'",
            !value.nullish());
        result.addField(field.fullPath(), std::move(value));
    }
    return result.freeze().toBson();
}

void ensureUniqueKeyHasSupportingIndex(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       const NamespaceString& outputNs,
                                       const std::set<FieldPath>& uniqueKey,
                                       const BSONObj& userSpecifiedUniqueKey) {
    uassert(
        50938,
        str::stream() << "Cannot find index to verify that $out's unique key will be unique: "
                      << userSpecifiedUniqueKey,
        expCtx->mongoProcessInterface->uniqueKeyIsSupportedByIndex(expCtx, outputNs, uniqueKey));
}
}  // namespace

DocumentSource::GetNextResult DocumentSourceOut::getNext() {
    pExpCtx->checkForInterrupt();

    if (_done) {
        return GetNextResult::makeEOF();
    }

    if (!_initialized) {
        initializeWriteNs();
        _initialized = true;
    }

    BatchedObjects batch;
    int bufferedBytes = 0;

    auto nextInput = pSource->getNext();
    for (; nextInput.isAdvanced(); nextInput = pSource->getNext()) {
        // clang-format off
        CurOpFailpointHelpers::waitWhileFailPointEnabled(
            &hangWhileBuildingDocumentSourceOutBatch,
            pExpCtx->opCtx,
            "hangWhileBuildingDocumentSourceOutBatch",
            []() {
                log() << "Hanging aggregation due to 'hangWhileBuildingDocumentSourceOutBatch' "
                      << "failpoint";
            });
        // clang-format on

        auto doc = nextInput.releaseDocument();

        // Generate an _id if the uniqueKey includes _id but the document doesn't have one.
        if (_uniqueKeyIncludesId && doc.getField("_id"_sd).missing()) {
            MutableDocument mutableDoc(std::move(doc));
            mutableDoc["_id"_sd] = Value(OID::gen());
            doc = mutableDoc.freeze();
        }

        // Extract the unique key before converting the document to BSON.
        auto uniqueKey = extractUniqueKeyFromDoc(doc, _uniqueKeyFields);
        auto insertObj = doc.toBson();

        bufferedBytes += insertObj.objsize();
        if (!batch.empty() &&
            (bufferedBytes > BSONObjMaxUserSize || batch.size() >= write_ops::kMaxWriteBatchSize)) {
            spill(std::move(batch));
            batch.clear();
            bufferedBytes = insertObj.objsize();
        }
        batch.emplace(std::move(insertObj), std::move(uniqueKey));
    }
    if (!batch.empty()) {
        spill(std::move(batch));
        batch.clear();
    }

    switch (nextInput.getStatus()) {
        case GetNextResult::ReturnStatus::kAdvanced: {
            MONGO_UNREACHABLE;  // We consumed all advances above.
        }
        case GetNextResult::ReturnStatus::kPauseExecution: {
            return nextInput;  // Propagate the pause.
        }
        case GetNextResult::ReturnStatus::kEOF: {

            finalize();
            _done = true;

            // $out doesn't currently produce any outputs.
            return nextInput;
        }
    }
    MONGO_UNREACHABLE;
}

intrusive_ptr<DocumentSourceOut> DocumentSourceOut::create(
    NamespaceString outputNs,
    const intrusive_ptr<ExpressionContext>& expCtx,
    WriteModeEnum mode,
    std::set<FieldPath> uniqueKey,
    boost::optional<ChunkVersion> targetCollectionVersion) {

    // TODO (SERVER-36832): Allow this combination.
    uassert(
        50939,
        str::stream() << "$out with mode " << WriteMode_serializer(mode)
                      << " is not supported when the output collection is in a different database",
        !(mode == WriteModeEnum::kModeReplaceCollection && outputNs.db() != expCtx->ns.db()));

    uassert(50992,
            str::stream() << "$out with mode  " << WriteMode_serializer(mode)
                          << " is not supported when the output collection is the same as the"
                          << " aggregation collection",
            mode == WriteModeEnum::kModeReplaceCollection || expCtx->ns != outputNs);

    uassert(ErrorCodes::OperationNotSupportedInTransaction,
            "$out cannot be used in a transaction",
            !expCtx->inMultiDocumentTransaction);

    auto readConcernLevel = repl::ReadConcernArgs::get(expCtx->opCtx).getLevel();
    uassert(ErrorCodes::InvalidOptions,
            "$out cannot be used with a 'linearizable' read concern level",
            readConcernLevel != repl::ReadConcernLevel::kLinearizableReadConcern);

    // Although we perform a check for "replaceCollection" mode with a sharded output collection
    // during lite parsing, we need to do it here as well in case mongos is stale or the command is
    // sent directly to the shard.
    if (mode == WriteModeEnum::kModeReplaceCollection) {
        LocalReadConcernBlock readLocal(expCtx->opCtx);
        uassert(17017,
                str::stream() << "$out with mode " << WriteMode_serializer(mode)
                              << " is not supported to an existing *sharded* output collection.",
                !expCtx->mongoProcessInterface->isSharded(expCtx->opCtx, outputNs));
    }
    uassert(17385, "Can't $out to special collection: " + outputNs.coll(), !outputNs.isSpecial());

    switch (mode) {
        case WriteModeEnum::kModeReplaceCollection:
            return new DocumentSourceOutReplaceColl(
                std::move(outputNs), expCtx, mode, std::move(uniqueKey), targetCollectionVersion);
        case WriteModeEnum::kModeInsertDocuments:
            return new DocumentSourceOutInPlace(
                std::move(outputNs), expCtx, mode, std::move(uniqueKey), targetCollectionVersion);
        case WriteModeEnum::kModeReplaceDocuments:
            return new DocumentSourceOutInPlaceReplace(
                std::move(outputNs), expCtx, mode, std::move(uniqueKey), targetCollectionVersion);
        default:
            MONGO_UNREACHABLE;
    }
}

DocumentSourceOut::DocumentSourceOut(NamespaceString outputNs,
                                     const intrusive_ptr<ExpressionContext>& expCtx,
                                     WriteModeEnum mode,
                                     std::set<FieldPath> uniqueKey,
                                     boost::optional<ChunkVersion> targetCollectionVersion)
    : DocumentSource(expCtx),
      _writeConcern(expCtx->opCtx->getWriteConcern()),
      _outputNs(std::move(outputNs)),
      _targetCollectionVersion(targetCollectionVersion),
      _done(false),
      _mode(mode),
      _uniqueKeyFields(std::move(uniqueKey)),
      _uniqueKeyIncludesId(_uniqueKeyFields.count("_id") == 1) {}

intrusive_ptr<DocumentSource> DocumentSourceOut::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {

    auto mode = WriteModeEnum::kModeReplaceCollection;
    std::set<FieldPath> uniqueKey;
    NamespaceString outputNs;
    boost::optional<ChunkVersion> targetCollectionVersion;
    if (elem.type() == BSONType::String) {
        outputNs = NamespaceString(expCtx->ns.db().toString() + '.' + elem.str());
        uniqueKey.emplace("_id");
    } else if (elem.type() == BSONType::Object) {
        auto spec =
            DocumentSourceOutSpec::parse(IDLParserErrorContext("$out"), elem.embeddedObject());
        mode = spec.getMode();

        // Retrieve the target database from the user command, otherwise use the namespace from the
        // expression context.
        auto dbName = spec.getTargetDb() ? *spec.getTargetDb() : expCtx->ns.db();
        outputNs = NamespaceString(dbName, spec.getTargetCollection());

        std::tie(uniqueKey, targetCollectionVersion) = expCtx->inMongos
            ? resolveUniqueKeyOnMongoS(expCtx, spec, outputNs)
            : resolveUniqueKeyOnMongoD(expCtx, spec, outputNs);
    } else {
        uasserted(16990,
                  str::stream() << "$out only supports a string or object argument, not "
                                << typeName(elem.type()));
    }

    return create(std::move(outputNs), expCtx, mode, std::move(uniqueKey), targetCollectionVersion);
}

std::pair<std::set<FieldPath>, boost::optional<ChunkVersion>>
DocumentSourceOut::resolveUniqueKeyOnMongoD(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                            const DocumentSourceOutSpec& spec,
                                            const NamespaceString& outputNs) {
    invariant(!expCtx->inMongos);
    auto targetCollectionVersion = spec.getTargetCollectionVersion();
    if (targetCollectionVersion) {
        uassert(51018, "Unexpected target chunk version specified", expCtx->fromMongos);
        // If mongos has sent us a target shard version, we need to be sure we are prepared to
        // act as a router which is at least as recent as that mongos.
        expCtx->mongoProcessInterface->checkRoutingInfoEpochOrThrow(
            expCtx, outputNs, *targetCollectionVersion);
    }

    auto userSpecifiedUniqueKey = spec.getUniqueKey();
    if (!userSpecifiedUniqueKey) {
        uassert(51017, "Expected uniqueKey to be provided from mongos", !expCtx->fromMongos);
        return {std::set<FieldPath>{"_id"}, targetCollectionVersion};
    }

    // Make sure the uniqueKey has a supporting index. Skip this check if the command is sent
    // from mongos since the uniqueKey check would've happened already.
    auto uniqueKey = parseUniqueKeyFromSpec(userSpecifiedUniqueKey.get());
    if (!expCtx->fromMongos) {
        ensureUniqueKeyHasSupportingIndex(expCtx, outputNs, uniqueKey, *userSpecifiedUniqueKey);
    }
    return {uniqueKey, targetCollectionVersion};
}

std::pair<std::set<FieldPath>, boost::optional<ChunkVersion>>
DocumentSourceOut::resolveUniqueKeyOnMongoS(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                            const DocumentSourceOutSpec& spec,
                                            const NamespaceString& outputNs) {
    invariant(expCtx->inMongos);
    uassert(50984,
            "$out received unexpected 'targetCollectionVersion' on mongos",
            !spec.getTargetCollectionVersion());

    if (auto userSpecifiedUniqueKey = spec.getUniqueKey()) {
        // Convert unique key object to a vector of FieldPaths.
        auto uniqueKey = parseUniqueKeyFromSpec(userSpecifiedUniqueKey.get());
        ensureUniqueKeyHasSupportingIndex(expCtx, outputNs, uniqueKey, *userSpecifiedUniqueKey);

        // If the user supplies the uniqueKey we don't need to attach a ChunkVersion for the shards
        // since we are not at risk of 'guessing' the wrong shard key.
        return {uniqueKey, boost::none};
    }

    // In case there are multiple shards which will perform this $out in parallel, we need to figure
    // out and attach the collection's shard version to ensure each shard is talking about the same
    // version of the collection. This mongos will coordinate that. We force a catalog refresh to do
    // so because there is no shard versioning protocol on this namespace and so we otherwise could
    // not be sure this node is (or will be come) at all recent. We will also figure out and attach
    // the uniqueKey to send to the shards. We don't need to do this for 'replaceCollection' mode
    // since that mode cannot currently target a sharded collection.

    // There are cases where the aggregation could fail if the collection is dropped or re-created
    // during or near the time of the aggregation. This is okay - we are mostly paranoid that this
    // mongos is very stale and want to prevent returning an error if the collection was dropped a
    // long time ago. Because of this, we are okay with piggy-backing off another thread's request
    // to refresh the cache, simply waiting for that request to return instead of forcing another
    // refresh.
    boost::optional<ChunkVersion> targetCollectionVersion =
        spec.getMode() == WriteModeEnum::kModeReplaceCollection
        ? boost::none
        : expCtx->mongoProcessInterface->refreshAndGetCollectionVersion(expCtx, outputNs);

    auto docKeyPaths = expCtx->mongoProcessInterface->collectDocumentKeyFieldsActingAsRouter(
        expCtx->opCtx, outputNs);
    return {std::set<FieldPath>(std::make_move_iterator(docKeyPaths.begin()),
                                std::make_move_iterator(docKeyPaths.end())),
            targetCollectionVersion};
}

Value DocumentSourceOut::serialize(boost::optional<ExplainOptions::Verbosity> explain) const {
    DocumentSourceOutSpec spec;
    spec.setTargetDb(_outputNs.db());
    spec.setTargetCollection(_outputNs.coll());
    spec.setMode(_mode);
    spec.setUniqueKey([&]() {
        BSONObjBuilder uniqueKeyBob;
        for (auto path : _uniqueKeyFields) {
            uniqueKeyBob.append(path.fullPath(), 1);
        }
        return uniqueKeyBob.obj();
    }());
    spec.setTargetCollectionVersion(_targetCollectionVersion);
    return Value(Document{{getSourceName(), spec.toBSON()}});
}

DepsTracker::State DocumentSourceOut::getDependencies(DepsTracker* deps) const {
    deps->needWholeDocument = true;
    return DepsTracker::State::EXHAUSTIVE_ALL;
}
}  // namespace mongo
