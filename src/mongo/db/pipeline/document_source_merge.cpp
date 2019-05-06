/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_merge.h"

#include <fmt/format.h>
#include <map>

#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/pipeline/document_path_support.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/util/log.h"

namespace mongo {
using namespace fmt::literals;

MONGO_FAIL_POINT_DEFINE(hangWhileBuildingDocumentSourceMergeBatch);
REGISTER_DOCUMENT_SOURCE(merge,
                         DocumentSourceMerge::LiteParsed::parse,
                         DocumentSourceMerge::createFromBson);

namespace {
using MergeStrategyDescriptor = DocumentSourceMerge::MergeStrategyDescriptor;
using MergeMode = MergeStrategyDescriptor::MergeMode;
using MergeStrategy = MergeStrategyDescriptor::MergeStrategy;
using MergeStrategyDescriptorsMap = std::map<const MergeMode, const MergeStrategyDescriptor>;
using WhenMatched = MergeStrategyDescriptor::WhenMatched;
using WhenNotMatched = MergeStrategyDescriptor::WhenNotMatched;
using BatchTransform = std::function<void(DocumentSourceMerge::BatchedObjects&)>;

constexpr auto kStageName = DocumentSourceMerge::kStageName;
constexpr auto kDefaultWhenMatched = WhenMatched::kMerge;
constexpr auto kDefaultWhenNotMatched = WhenNotMatched::kInsert;
constexpr auto kReplaceWithNewInsertMode =
    MergeMode{WhenMatched::kReplaceWithNew, WhenNotMatched::kInsert};
constexpr auto kMergeInsertMode = MergeMode{WhenMatched::kMerge, WhenNotMatched::kInsert};
constexpr auto kKeepExistingInsertMode =
    MergeMode{WhenMatched::kKeepExisting, WhenNotMatched::kInsert};
constexpr auto kFailInsertMode = MergeMode{WhenMatched::kFail, WhenNotMatched::kInsert};

/**
 * Creates a merge strategy which uses update semantics to do perform a merge operation. If
 * 'BatchTransform' function is provided, it will be called to transform batched objects before
 * passing them to the 'update'.
 */
MergeStrategy makeUpdateStrategy(bool upsert, BatchTransform transform) {
    return [upsert, transform](
        const auto& expCtx, const auto& ns, const auto& wc, auto epoch, auto&& batch) {
        if (transform) {
            transform(batch);
        }
        constexpr auto multi = false;
        expCtx->mongoProcessInterface->update(expCtx,
                                              ns,
                                              std::move(batch.uniqueKeys),
                                              std::move(batch.objects),
                                              wc,
                                              upsert,
                                              multi,
                                              epoch);
    };
}

/**
 * Creates a merge strategy which uses insert semantics to perform a merge operation.
 */
MergeStrategy makeInsertStrategy() {
    return [](const auto& expCtx, const auto& ns, const auto& wc, auto epoch, auto&& batch) {
        expCtx->mongoProcessInterface->insert(expCtx, ns, std::move(batch.objects), wc, epoch);
    };
}

/**
 * Creates a batched objects transformation function which wraps each element of the 'batch.objects'
 * array into the given 'updateOp' operator.
 */
BatchTransform makeUpdateTransform(const std::string& updateOp) {
    return [updateOp](auto& batch) {
        std::transform(batch.objects.begin(),
                       batch.objects.end(),
                       batch.objects.begin(),
                       [updateOp](const auto& obj) { return BSON(updateOp << obj); });
    };
}

/**
 * Returns a map that contains descriptors for all supported merge strategies for the $merge stage.
 * Each descriptor is constant and stateless and thus, can be shared by all $merge stages. A
 * descriptor is accessed using a pair of whenMatched/whenNotMatched merge modes, which defines the
 * semantics of the merge operation. When a $merge stage is created, a merge descriptor is selected
 * from this map based on the requested merge modes, and then passed to the $merge stage
 * constructor.
 */
const MergeStrategyDescriptorsMap& getDescriptors() {
    // Rather than defining this map as a global object, we'll wrap the static map into a function
    // to prevent static initialization order fiasco which may happen since ActionType instances
    // are also defined as global objects and there is no way to tell the linker which objects must
    // be initialized first. By wrapping the map into a function we can guarantee that it won't be
    // initialized until the first use, which is when the program already started and all global
    // variables had been initialized.
    static const auto mergeStrategyDescriptors = MergeStrategyDescriptorsMap{
        {kReplaceWithNewInsertMode,
         {kReplaceWithNewInsertMode,
          {ActionType::insert, ActionType::update},
          makeUpdateStrategy(true, {})}},
        {kMergeInsertMode,
         {kMergeInsertMode,
          {ActionType::insert, ActionType::update},
          makeUpdateStrategy(true, makeUpdateTransform("$set"))}},
        {kKeepExistingInsertMode,
         {kKeepExistingInsertMode,
          {ActionType::insert, ActionType::update},
          makeUpdateStrategy(true, makeUpdateTransform("$setOnInsert"))}},
        {kFailInsertMode, {kFailInsertMode, {ActionType::insert}, makeInsertStrategy()}}};
    return mergeStrategyDescriptors;
}

/**
 * Checks if a pair of whenMatched/whenNotMatched merge modes is supported.
 */
bool isSupportedMergeMode(WhenMatched whenMatched, WhenNotMatched whenNotMatched) {
    return getDescriptors().count({whenMatched, whenNotMatched}) > 0;
}

/**
 * Parses the fields of the $merge 'on' from the user-specified 'fields', returning a set of field
 * paths. Throws if 'fields' contains duplicate elements.
 */
std::set<FieldPath> parseMergeOnFieldsFromSpec(const std::vector<std::string>& fields) {
    std::set<FieldPath> mergeOnFields;

    for (const auto& field : fields) {
        const auto res = mergeOnFields.insert(FieldPath(field));
        uassert(ErrorCodes::BadValue,
                "Found a duplicate field '{}' in {} 'on'"_format(field, kStageName),
                res.second);
    }

    return mergeOnFields;
}

/**
 * Extracts the fields of $merge 'on' from 'doc' and returns the key as a BSONObj. Throws if any
 * field of the 'on' extracted from 'doc' is nullish or an array.
 */
BSONObj extractMergeOnFieldsFromDoc(const Document& doc, const std::set<FieldPath>& mergeOnFields) {
    MutableDocument result;
    for (const auto& field : mergeOnFields) {
        auto value = doc.getNestedField(field);
        uassert(51185,
                "{} write error: 'on' field '{}' is an array"_format(kStageName, field.fullPath()),
                !value.isArray());
        uassert(
            51132,
            "{} write error: 'on' field '{}' cannot be missing, null, undefined or an array"_format(
                kStageName, field.fullPath()),
            !value.nullish());
        result.addField(field.fullPath(), std::move(value));
    }
    return result.freeze().toBson();
}

/**
 * Extracts $merge 'on' fields from the $merge spec when the pipeline is executed on mongoD, or use
 * a default _id field if the user hasn't supplied the 'on' field. For the user supplied field
 * ensures that it can be used to uniquely identify documents for merge.
 */
std::pair<std::set<FieldPath>, boost::optional<ChunkVersion>> resolveMergeOnFieldsOnMongoD(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const DocumentSourceMergeSpec& spec,
    const NamespaceString& outputNs) {
    invariant(!expCtx->inMongos);
    auto targetCollectionVersion = spec.getTargetCollectionVersion();
    if (targetCollectionVersion) {
        uassert(51123, "Unexpected target chunk version specified", expCtx->fromMongos);
        // If mongos has sent us a target shard version, we need to be sure we are prepared to
        // act as a router which is at least as recent as that mongos.
        expCtx->mongoProcessInterface->checkRoutingInfoEpochOrThrow(
            expCtx, outputNs, *targetCollectionVersion);
    }

    auto userSpecifiedMergeOnFields = spec.getOn();
    if (!userSpecifiedMergeOnFields) {
        uassert(51124, "Expected 'on' field to be provided from mongos", !expCtx->fromMongos);
        return {std::set<FieldPath>{"_id"}, targetCollectionVersion};
    }

    // Make sure the 'on' field has a supporting index. Skip this check if the command is sent
    // from mongos since the 'on' field check would've happened already.
    auto mergeOnFields = parseMergeOnFieldsFromSpec(*userSpecifiedMergeOnFields);
    if (!expCtx->fromMongos) {
        uassert(51183,
                "Cannot find index to verify that 'on' fields will be unique",
                expCtx->mongoProcessInterface->uniqueKeyIsSupportedByIndex(
                    expCtx, outputNs, mergeOnFields));
    }
    return {mergeOnFields, targetCollectionVersion};
}

/**
 * Extracts $merge 'on' fields from the $merge spec when the pipeline is executed on mongoS. If the
 * user supplied the 'on' field, ensures that it can be used to uniquely identify documents for
 * merge. Otherwise, extracts the shard key and use it as the 'on' field.
 */
std::pair<std::set<FieldPath>, boost::optional<ChunkVersion>> resolveMergeOnFieldsOnMongoS(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const DocumentSourceMergeSpec& spec,
    const NamespaceString& outputNs) {
    invariant(expCtx->inMongos);
    uassert(51179,
            "{} received unexpected 'targetCollectionVersion' on mongos"_format(kStageName),
            !spec.getTargetCollectionVersion());

    if (auto userSpecifiedMergeOnFields = spec.getOn()) {
        // Convert 'on' array to a vector of FieldPaths.
        auto mergeOnFields = parseMergeOnFieldsFromSpec(*userSpecifiedMergeOnFields);
        uassert(51190,
                "Cannot find index to verify that 'on' fields will be unique",
                expCtx->mongoProcessInterface->uniqueKeyIsSupportedByIndex(
                    expCtx, outputNs, mergeOnFields));

        // If the user supplies the 'on' field we don't need to attach a ChunkVersion for the shards
        // since we are not at risk of 'guessing' the wrong shard key.
        return {mergeOnFields, boost::none};
    }

    // In case there are multiple shards which will perform this stage in parallel, we need to
    // figure out and attach the collection's shard version to ensure each shard is talking about
    // the same version of the collection. This mongos will coordinate that. We force a catalog
    // refresh to do so because there is no shard versioning protocol on this namespace and so we
    // otherwise could not be sure this node is (or will become) at all recent. We will also
    // figure out and attach the 'on' field to send to the shards.

    // There are cases where the aggregation could fail if the collection is dropped or re-created
    // during or near the time of the aggregation. This is okay - we are mostly paranoid that this
    // mongos is very stale and want to prevent returning an error if the collection was dropped a
    // long time ago. Because of this, we are okay with piggy-backing off another thread's request
    // to refresh the cache, simply waiting for that request to return instead of forcing another
    // refresh.
    boost::optional<ChunkVersion> targetCollectionVersion =
        expCtx->mongoProcessInterface->refreshAndGetCollectionVersion(expCtx, outputNs);

    auto docKeyPaths = expCtx->mongoProcessInterface->collectDocumentKeyFieldsActingAsRouter(
        expCtx->opCtx, outputNs);
    return {std::set<FieldPath>(std::make_move_iterator(docKeyPaths.begin()),
                                std::make_move_iterator(docKeyPaths.end())),
            targetCollectionVersion};
}

/**
 * Parses a $merge stage specification and resolves the target database name and collection name.
 * The $merge specification can be either a string or an object. If the target database name is not
 * explicitly specified, it will be defaulted to 'defaultDb'.
 */
DocumentSourceMergeSpec parseMergeSpecAndResolveTargetNamespace(const BSONElement& spec,
                                                                StringData defaultDb) {
    NamespaceString targetNss;
    DocumentSourceMergeSpec mergeSpec;

    // If the $merge spec is a simple string, then we're using a shortcut syntax and the string
    // value specifies a target collection name. Since it is not possible to specify a target
    // database name using the shortcut syntax (to match the semantics of the $out stage), the
    // target database will use the default name provided.
    if (spec.type() == BSONType::String) {
        targetNss = {defaultDb, spec.valueStringData()};
    } else {
        mergeSpec = DocumentSourceMergeSpec::parse({kStageName}, spec.embeddedObject());
        targetNss = mergeSpec.getTargetNss();
        if (targetNss.coll().empty()) {
            // If the $merge spec is an object, the target namespace can be specified as a string
            // on an object value of the 'into' field. In case it was a string, we want to use the
            // same semantics as above, that is, treat it as a collection name. This is different
            // from the NamespaceString semantics which treats it as a database name. So, if the
            // target namespace collection is empty, we'll use the default database name as a target
            // database, and the provided namespace value as a collection name.
            targetNss = {defaultDb, targetNss.ns()};
        } else if (targetNss.db().empty()) {
            // Use the default database name if it wasn't specified explicilty.
            targetNss = {defaultDb, targetNss.coll()};
        }
    }

    uassert(ErrorCodes::InvalidNamespace,
            "Invalid {} target namespace: '{}'"_format(kStageName, targetNss.ns()),
            targetNss.isValid());

    mergeSpec.setTargetNss(std::move(targetNss));

    return mergeSpec;
}
}  // namespace

std::unique_ptr<DocumentSourceMerge::LiteParsed> DocumentSourceMerge::LiteParsed::parse(
    const AggregationRequest& request, const BSONElement& spec) {
    uassert(ErrorCodes::TypeMismatch,
            "{} requires a string or object argument, but found {}"_format(kStageName,
                                                                           typeName(spec.type())),
            spec.type() == BSONType::String || spec.type() == BSONType::Object);

    auto mergeSpec =
        parseMergeSpecAndResolveTargetNamespace(spec, request.getNamespaceString().db());
    auto targetNss = mergeSpec.getTargetNss();
    auto whenMatched = mergeSpec.getWhenMatched().value_or(kDefaultWhenMatched);
    auto whenNotMatched = mergeSpec.getWhenNotMatched().value_or(kDefaultWhenNotMatched);
    uassert(51181,
            "Combination of {} modes 'whenMatched: {}' and 'whenNotMatched: {}' "
            "is not supported"_format(kStageName,
                                      MergeWhenMatchedMode_serializer(whenMatched),
                                      MergeWhenNotMatchedMode_serializer(whenNotMatched)),
            isSupportedMergeMode(whenMatched, whenNotMatched));

    auto actions = ActionSet{getDescriptors().at({whenMatched, whenNotMatched}).actions};
    if (request.shouldBypassDocumentValidation()) {
        actions.addAction(ActionType::bypassDocumentValidation);
    }

    PrivilegeVector privileges{{ResourcePattern::forExactNamespace(targetNss), actions}};

    return stdx::make_unique<DocumentSourceMerge::LiteParsed>(std::move(targetNss),
                                                              std::move(privileges));
}

DocumentSourceMerge::DocumentSourceMerge(NamespaceString outputNs,
                                         const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         const MergeStrategyDescriptor& descriptor,
                                         std::set<FieldPath> mergeOnFields,
                                         boost::optional<ChunkVersion> targetCollectionVersion,
                                         bool serializeAsOutStage)
    : DocumentSource(expCtx),
      _writeConcern(expCtx->opCtx->getWriteConcern()),
      _outputNs(std::move(outputNs)),
      _targetCollectionVersion(targetCollectionVersion),
      _done(false),
      _descriptor(descriptor),
      _mergeOnFields(std::move(mergeOnFields)),
      _mergeOnFieldsIncludesId(_mergeOnFields.count("_id") == 1),
      _serializeAsOutStage(serializeAsOutStage) {}

boost::intrusive_ptr<DocumentSource> DocumentSourceMerge::create(
    NamespaceString outputNs,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    WhenMatched whenMatched,
    WhenNotMatched whenNotMatched,
    std::set<FieldPath> mergeOnFields,
    boost::optional<ChunkVersion> targetCollectionVersion,
    bool serializeAsOutStage) {

    uassert(51189,
            "Combination of {} modes 'whenMatched: {}' and 'whenNotMatched: {}' "
            "is not supported"_format(kStageName,
                                      MergeWhenMatchedMode_serializer(whenMatched),
                                      MergeWhenNotMatchedMode_serializer(whenNotMatched)),
            isSupportedMergeMode(whenMatched, whenNotMatched));

    uassert(51188,
            "{} is not supported when the output collection is the same as "
            "the aggregation collection"_format(kStageName),
            expCtx->ns != outputNs);

    uassert(ErrorCodes::OperationNotSupportedInTransaction,
            "{} cannot be used in a transaction"_format(kStageName),
            !expCtx->inMultiDocumentTransaction);

    auto readConcernLevel = repl::ReadConcernArgs::get(expCtx->opCtx).getLevel();
    uassert(ErrorCodes::InvalidOptions,
            "{} cannot be used with a 'linearizable' read concern level"_format(kStageName),
            readConcernLevel != repl::ReadConcernLevel::kLinearizableReadConcern);

    uassert(51180,
            "Cannot {} into special collection: '{}'"_format(kStageName, outputNs.coll()),
            !outputNs.isSpecial());

    return new DocumentSourceMerge(outputNs,
                                   expCtx,
                                   getDescriptors().at({whenMatched, whenNotMatched}),
                                   mergeOnFields,
                                   targetCollectionVersion,
                                   serializeAsOutStage);
}

boost::intrusive_ptr<DocumentSource> DocumentSourceMerge::createFromBson(
    BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(51182,
            "{} only supports a string or object argument, not {}"_format(kStageName, spec.type()),
            spec.type() == BSONType::String || spec.type() == BSONType::Object);

    auto mergeSpec = parseMergeSpecAndResolveTargetNamespace(spec, expCtx->ns.db());
    auto targetNss = mergeSpec.getTargetNss();
    auto whenMatched = mergeSpec.getWhenMatched().value_or(kDefaultWhenMatched);
    auto whenNotMatched = mergeSpec.getWhenNotMatched().value_or(kDefaultWhenNotMatched);
    // TODO SERVER-40432: move resolveMergeOnFieldsOnMongo* into MongoProcessInterface.
    auto[mergeOnFields, targetCollectionVersion] = expCtx->inMongos
        ? resolveMergeOnFieldsOnMongoS(expCtx, mergeSpec, targetNss)
        : resolveMergeOnFieldsOnMongoD(expCtx, mergeSpec, targetNss);

    return DocumentSourceMerge::create(std::move(targetNss),
                                       expCtx,
                                       whenMatched,
                                       whenNotMatched,
                                       std::move(mergeOnFields),
                                       targetCollectionVersion,
                                       false /* serialize as $out stage */);
}

DocumentSource::GetNextResult DocumentSourceMerge::getNext() {
    pExpCtx->checkForInterrupt();

    if (_done) {
        return GetNextResult::makeEOF();
    }

    if (!_initialized) {
        // Explain of a $merge should never try to actually execute any writes. We only ever expect
        // getNext() to be called for the 'executionStats' and 'allPlansExecution' explain modes.
        // This assertion should not be triggered for 'queryPlanner' explain of a $merge, which is
        // perfectly legal.
        uassert(51184,
                "explain of {} is not allowed with verbosity {}"_format(
                    kStageName, ExplainOptions::verbosityString(*pExpCtx->explain)),
                !pExpCtx->explain);
        _initialized = true;
    }

    BatchedObjects batch;
    int bufferedBytes = 0;

    auto nextInput = pSource->getNext();
    for (; nextInput.isAdvanced(); nextInput = pSource->getNext()) {
        CurOpFailpointHelpers::waitWhileFailPointEnabled(
            &hangWhileBuildingDocumentSourceMergeBatch,
            pExpCtx->opCtx,
            "hangWhileBuildingDocumentSourceMergeBatch",
            []() {
                log() << "Hanging aggregation due to 'hangWhileBuildingDocumentSourceMergeBatch' "
                      << "failpoint";
            });

        auto doc = nextInput.releaseDocument();

        // Generate an _id if the uniqueKey includes _id but the document doesn't have one.
        if (_mergeOnFieldsIncludesId && doc.getField("_id"_sd).missing()) {
            MutableDocument mutableDoc(std::move(doc));
            mutableDoc["_id"_sd] = Value(OID::gen());
            doc = mutableDoc.freeze();
        }

        // Extract the 'on' fields before converting the document to BSON.
        auto mergeOnFields = extractMergeOnFieldsFromDoc(doc, _mergeOnFields);
        auto insertObj = doc.toBson();

        bufferedBytes += insertObj.objsize();
        if (!batch.empty() &&
            (bufferedBytes > BSONObjMaxUserSize || batch.size() >= write_ops::kMaxWriteBatchSize)) {
            spill(std::move(batch));
            batch.clear();
            bufferedBytes = insertObj.objsize();
        }
        batch.emplace(std::move(insertObj), std::move(mergeOnFields));
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
            _done = true;
            return nextInput;
        }
    }
    MONGO_UNREACHABLE;
}

Value DocumentSourceMerge::serialize(boost::optional<ExplainOptions::Verbosity> explain) const {
    if (_serializeAsOutStage) {
        uassert(ErrorCodes::BadValue,
                "Cannot serialize {} stage as $out for 'whenMatched: {}' and "
                "'whenNotMatched: {}'"_format(
                    kStageName,
                    MergeWhenMatchedMode_serializer(_descriptor.mode.first),
                    MergeWhenNotMatchedMode_serializer(_descriptor.mode.second)),
                (_descriptor.mode.first == WhenMatched::kFail ||
                 _descriptor.mode.first == WhenMatched::kReplaceWithNew) &&
                    (_descriptor.mode.second == WhenNotMatched::kInsert));
        DocumentSourceOutSpec spec;
        spec.setTargetDb(_outputNs.db());
        spec.setTargetCollection(_outputNs.coll());
        spec.setMode(_descriptor.mode.first == WhenMatched::kFail
                         ? WriteModeEnum::kModeInsertDocuments
                         : WriteModeEnum::kModeReplaceDocuments);
        spec.setUniqueKey([&]() {
            BSONObjBuilder uniqueKeyBob;
            for (auto path : _mergeOnFields) {
                uniqueKeyBob.append(path.fullPath(), 1);
            }
            return uniqueKeyBob.obj();
        }());
        spec.setTargetCollectionVersion(_targetCollectionVersion);
        return Value(Document{{DocumentSourceOut::kStageName.rawData(), spec.toBSON()}});
    } else {
        DocumentSourceMergeSpec spec;
        spec.setTargetNss(_outputNs);
        spec.setWhenMatched(_descriptor.mode.first);
        spec.setWhenNotMatched(_descriptor.mode.second);
        spec.setOn([&]() {
            std::vector<std::string> mergeOnFields;
            for (auto path : _mergeOnFields) {
                mergeOnFields.push_back(path.fullPath());
            }
            return mergeOnFields;
        }());
        spec.setTargetCollectionVersion(_targetCollectionVersion);
        return Value(Document{{getSourceName(), spec.toBSON()}});
    }
}
}  // namespace mongo
