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
#include <fmt/ostream.h>
#include <map>

#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/pipeline/document_path_support.h"
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
using UpdateModification = write_ops::UpdateModification;

constexpr auto kStageName = DocumentSourceMerge::kStageName;
constexpr auto kDefaultWhenMatched = WhenMatched::kMerge;
constexpr auto kDefaultWhenNotMatched = WhenNotMatched::kInsert;
constexpr auto kReplaceInsertMode = MergeMode{WhenMatched::kReplace, WhenNotMatched::kInsert};
constexpr auto kReplaceFailMode = MergeMode{WhenMatched::kReplace, WhenNotMatched::kFail};
constexpr auto kReplaceDiscardMode = MergeMode{WhenMatched::kReplace, WhenNotMatched::kDiscard};
constexpr auto kMergeInsertMode = MergeMode{WhenMatched::kMerge, WhenNotMatched::kInsert};
constexpr auto kMergeFailMode = MergeMode{WhenMatched::kMerge, WhenNotMatched::kFail};
constexpr auto kMergeDiscardMode = MergeMode{WhenMatched::kMerge, WhenNotMatched::kDiscard};
constexpr auto kKeepExistingInsertMode =
    MergeMode{WhenMatched::kKeepExisting, WhenNotMatched::kInsert};
constexpr auto kFailInsertMode = MergeMode{WhenMatched::kFail, WhenNotMatched::kInsert};
constexpr auto kPipelineInsertMode = MergeMode{WhenMatched::kPipeline, WhenNotMatched::kInsert};
constexpr auto kPipelineFailMode = MergeMode{WhenMatched::kPipeline, WhenNotMatched::kFail};
constexpr auto kPipelineDiscardMode = MergeMode{WhenMatched::kPipeline, WhenNotMatched::kDiscard};

/**
 * Creates a merge strategy which uses update semantics to perform a merge operation. If
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
        uassertStatusOK(expCtx->mongoProcessInterface->update(
            expCtx, ns, std::move(batch), wc, upsert, multi, epoch));
    };
}

/**
 * Creates a merge strategy which uses update semantics to perform a merge operation and ensures
 * that each document in the batch has a matching document in the 'ns' collection (note that a
 * matching document may not be modified as a result of an update operation, yet it still will be
 * counted as matching). If at least one document doesn't have a match, this strategy returns an
 * error. If 'BatchTransform' function is provided, it will be called to transform batched objects
 * before passing them to the 'update'.
 */
MergeStrategy makeStrictUpdateStrategy(bool upsert, BatchTransform transform) {
    return [upsert, transform](
               const auto& expCtx, const auto& ns, const auto& wc, auto epoch, auto&& batch) {
        if (transform) {
            transform(batch);
        }

        const int64_t batchSize = batch.size();
        constexpr auto multi = false;
        auto updateResult = uassertStatusOK(expCtx->mongoProcessInterface->update(
            expCtx, ns, std::move(batch), wc, upsert, multi, epoch));
        uassert(ErrorCodes::MergeStageNoMatchingDocument,
                "{} could not find a matching document in the target collection "
                "for at least one document in the source collection"_format(kStageName),
                updateResult.nMatched == batchSize);
    };
}

/**
 * Creates a merge strategy which uses insert semantics to perform a merge operation.
 */
MergeStrategy makeInsertStrategy() {
    return [](const auto& expCtx, const auto& ns, const auto& wc, auto epoch, auto&& batch) {
        std::vector<BSONObj> objectsToInsert(batch.size());
        // The batch stores replacement style updates, but for this "insert" style of $merge we'd
        // like to just insert the new document without attempting any sort of replacement.
        std::transform(batch.begin(), batch.end(), objectsToInsert.begin(), [](const auto& obj) {
            return std::get<UpdateModification>(obj).getUpdateClassic();
        });
        uassertStatusOK(expCtx->mongoProcessInterface->insert(
            expCtx, ns, std::move(objectsToInsert), wc, epoch));
    };
}

/**
 * Creates a batched objects transformation function which wraps each element of the
 * 'batch.modifications' array into the given 'updateOp' operator.
 */
BatchTransform makeUpdateTransform(const std::string& updateOp) {
    return [updateOp](auto& batch) {
        for (auto&& obj : batch) {
            std::get<UpdateModification>(obj) =
                BSON(updateOp << std::get<UpdateModification>(obj).getUpdateClassic());
        }
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
        // whenMatched: replace, whenNotMatched: insert
        {kReplaceInsertMode,
         {kReplaceInsertMode,
          {ActionType::insert, ActionType::update},
          makeUpdateStrategy(true, {})}},
        // whenMatched: replace, whenNotMatched: fail
        {kReplaceFailMode,
         {kReplaceFailMode, {ActionType::update}, makeStrictUpdateStrategy(false, {})}},
        // whenMatched: replace, whenNotMatched: discard
        {kReplaceDiscardMode,
         {kReplaceDiscardMode, {ActionType::update}, makeUpdateStrategy(false, {})}},
        // whenMatched: merge, whenNotMatched: insert
        {kMergeInsertMode,
         {kMergeInsertMode,
          {ActionType::insert, ActionType::update},
          makeUpdateStrategy(true, makeUpdateTransform("$set"))}},
        // whenMatched: merge, whenNotMatched: fail
        {kMergeFailMode,
         {kMergeFailMode,
          {ActionType::update},
          makeStrictUpdateStrategy(false, makeUpdateTransform("$set"))}},
        // whenMatched: merge, whenNotMatched: discard
        {kMergeDiscardMode,
         {kMergeDiscardMode,
          {ActionType::update},
          makeUpdateStrategy(false, makeUpdateTransform("$set"))}},
        // whenMatched: keepExisting, whenNotMatched: insert
        {kKeepExistingInsertMode,
         {kKeepExistingInsertMode,
          {ActionType::insert, ActionType::update},
          makeUpdateStrategy(true, makeUpdateTransform("$setOnInsert"))}},
        // whenMatched: [pipeline], whenNotMatched: insert
        {kPipelineInsertMode,
         {kPipelineInsertMode,
          {ActionType::insert, ActionType::update},
          makeUpdateStrategy(true, {})}},
        // whenMatched: [pipeline], whenNotMatched: fail
        {kPipelineFailMode,
         {kPipelineFailMode, {ActionType::update}, makeStrictUpdateStrategy(false, {})}},
        // whenMatched: [pipeline], whenNotMatched: discard
        {kPipelineDiscardMode,
         {kPipelineDiscardMode, {ActionType::update}, makeUpdateStrategy(false, {})}},
        // whenMatched: fail, whenNotMatched: insert
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
    auto whenMatched =
        mergeSpec.getWhenMatched() ? mergeSpec.getWhenMatched()->mode : kDefaultWhenMatched;
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

    return std::make_unique<DocumentSourceMerge::LiteParsed>(std::move(targetNss),
                                                             std::move(privileges));
}

DocumentSourceMerge::DocumentSourceMerge(NamespaceString outputNs,
                                         const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         const MergeStrategyDescriptor& descriptor,
                                         boost::optional<BSONObj> letVariables,
                                         boost::optional<std::vector<BSONObj>> pipeline,
                                         std::set<FieldPath> mergeOnFields,
                                         boost::optional<ChunkVersion> targetCollectionVersion)
    : DocumentSourceWriter(kStageName.rawData(), std::move(outputNs), expCtx),
      _targetCollectionVersion(targetCollectionVersion),
      _descriptor(descriptor),
      _pipeline(std::move(pipeline)),
      _mergeOnFields(std::move(mergeOnFields)),
      _mergeOnFieldsIncludesId(_mergeOnFields.count("_id") == 1) {
    if (letVariables) {
        _letVariables.emplace();

        for (auto&& varElem : *letVariables) {
            const auto varName = varElem.fieldNameStringData();
            Variables::uassertValidNameForUserWrite(varName);

            _letVariables->emplace(
                varName.toString(),
                Expression::parseOperand(expCtx, varElem, expCtx->variablesParseState));
        }
    }
}

boost::intrusive_ptr<DocumentSource> DocumentSourceMerge::create(
    NamespaceString outputNs,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    WhenMatched whenMatched,
    WhenNotMatched whenNotMatched,
    boost::optional<BSONObj> letVariables,
    boost::optional<std::vector<BSONObj>> pipeline,
    std::set<FieldPath> mergeOnFields,
    boost::optional<ChunkVersion> targetCollectionVersion) {
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

    if (whenMatched == WhenMatched::kPipeline) {
        if (!letVariables) {
            // For custom pipeline-style updates, default the 'let' variables to {new: "$$ROOT"},
            // if the user has omitted the 'let' argument.
            letVariables = BSON("new"
                                << "$$ROOT");
        }
    } else {
        // Ensure the 'let' argument cannot be used with any other merge modes.
        uassert(51199,
                "Cannot use 'let' variables with 'whenMatched: {}' mode"_format(
                    MergeWhenMatchedMode_serializer(whenMatched)),
                !letVariables);
    }

    return new DocumentSourceMerge(outputNs,
                                   expCtx,
                                   getDescriptors().at({whenMatched, whenNotMatched}),
                                   std::move(letVariables),
                                   std::move(pipeline),
                                   std::move(mergeOnFields),
                                   targetCollectionVersion);
}

boost::intrusive_ptr<DocumentSource> DocumentSourceMerge::createFromBson(
    BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(51182,
            "{} only supports a string or object argument, not {}"_format(kStageName, spec.type()),
            spec.type() == BSONType::String || spec.type() == BSONType::Object);

    auto mergeSpec = parseMergeSpecAndResolveTargetNamespace(spec, expCtx->ns.db());
    auto targetNss = mergeSpec.getTargetNss();
    auto whenMatched =
        mergeSpec.getWhenMatched() ? mergeSpec.getWhenMatched()->mode : kDefaultWhenMatched;
    auto whenNotMatched = mergeSpec.getWhenNotMatched().value_or(kDefaultWhenNotMatched);
    auto pipeline = mergeSpec.getWhenMatched() ? mergeSpec.getWhenMatched()->pipeline : boost::none;
    auto [mergeOnFields, targetCollectionVersion] =
        expCtx->mongoProcessInterface->ensureFieldsUniqueOrResolveDocumentKey(
            expCtx, mergeSpec.getOn(), mergeSpec.getTargetCollectionVersion(), targetNss);

    return DocumentSourceMerge::create(std::move(targetNss),
                                       expCtx,
                                       whenMatched,
                                       whenNotMatched,
                                       mergeSpec.getLet(),
                                       std::move(pipeline),
                                       std::move(mergeOnFields),
                                       targetCollectionVersion);
}

Value DocumentSourceMerge::serialize(boost::optional<ExplainOptions::Verbosity> explain) const {
    DocumentSourceMergeSpec spec;
    spec.setTargetNss(_outputNs);
    spec.setLet([&]() -> boost::optional<BSONObj> {
        if (!_letVariables) {
            return boost::none;
        }

        BSONObjBuilder bob;
        for (auto&& [name, expr] : *_letVariables) {
            bob << name << expr->serialize(static_cast<bool>(explain));
        }
        return bob.obj();
    }());
    spec.setWhenMatched(MergeWhenMatchedPolicy{_descriptor.mode.first, _pipeline});
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

std::pair<DocumentSourceMerge::BatchObject, int> DocumentSourceMerge::makeBatchObject(
    Document&& doc) const {
    // Generate an _id if the uniqueKey includes _id but the document doesn't have one.
    if (_mergeOnFieldsIncludesId && doc.getField("_id"_sd).missing()) {
        MutableDocument mutableDoc(std::move(doc));
        mutableDoc["_id"_sd] = Value(OID::gen());
        doc = mutableDoc.freeze();
    }

    auto mergeOnFields = extractMergeOnFieldsFromDoc(doc, _mergeOnFields);
    auto mod = makeBatchUpdateModification(doc);
    auto vars = resolveLetVariablesIfNeeded(doc);
    auto modSize = mod.objsize() + (vars ? vars->objsize() : 0);
    return {{std::move(mergeOnFields), std::move(mod), std::move(vars)}, modSize};
}

void DocumentSourceMerge::waitWhileFailPointEnabled() {
    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangWhileBuildingDocumentSourceMergeBatch,
        pExpCtx->opCtx,
        "hangWhileBuildingDocumentSourceMergeBatch",
        []() {
            log() << "Hanging aggregation due to 'hangWhileBuildingDocumentSourceMergeBatch' "
                  << "failpoint";
        });
}

}  // namespace mongo
