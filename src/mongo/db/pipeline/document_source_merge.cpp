// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_merge.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_merge_gen.h"
#include "mongo/db/pipeline/document_source_merge_spec.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/owned_lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/pipeline/variable_validation.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/version_context.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <string_view>
#include <tuple>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>
#include <fmt/ostream.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {
using namespace std::literals::string_view_literals;

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(merge,
                                     DocumentSourceMerge::LiteParsed::parse,
                                     AllowedWithApiStrict::kAlways);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(merge, DocumentSourceMerge, MergeStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(merge, DocumentSourceMerge::id)

namespace {

using WhenMatched = MergeStrategyDescriptor::WhenMatched;
using WhenNotMatched = MergeStrategyDescriptor::WhenNotMatched;

constexpr auto kStageName = DocumentSourceMerge::kStageName;
const auto kDefaultPipelineLet = BSON("new" << "$$ROOT");

/**
 * Checks if a pair of whenMatched/whenNotMatched merge modes is supported.
 */
bool isSupportedMergeMode(WhenMatched whenMatched, WhenNotMatched whenNotMatched) {
    // AllowInsertWithUpdateBackupStrategies doesn't affect the set of supported modes, so we can
    // pass any value.
    return getMergeStrategyDescriptors(MergeProcessor::AllowInsertWithUpdateBackupStrategies{false})
               .count({whenMatched, whenNotMatched}) > 0;
}

/**
 * Parses a $merge stage specification and resolves the target database name and collection
 * name. The $merge specification can be either a string or an object. If the target database
 * name is not explicitly specified, it will be defaulted to 'defaultDb'.
 */
DocumentSourceMergeSpec parseMergeSpecAndResolveTargetNamespace(
    const BSONElement& spec,
    const DatabaseName& defaultDb,
    const SerializationContext& sc = SerializationContext::stateDefault()) {
    NamespaceString targetNss;
    DocumentSourceMergeSpec mergeSpec;

    // If the $merge spec is a simple string, then we're using a shortcut syntax and the string
    // value specifies a target collection name. Since it is not possible to specify a target
    // database name using the shortcut syntax (to match the semantics of the $out stage), the
    // target database will use the default name provided.
    if (spec.type() == BSONType::string) {
        targetNss = NamespaceStringUtil::deserialize(defaultDb, spec.valueStringData());
    } else {
        const auto tenantId = defaultDb.tenantId();
        const auto vts = tenantId
            ? boost::make_optional(auth::ValidatedTenancyScopeFactory::create(
                  *tenantId, auth::ValidatedTenancyScopeFactory::TrustedForInnerOpMsgRequestTag{}))
            : boost::none;
        mergeSpec = DocumentSourceMergeSpec::parse(spec.embeddedObject(),
                                                   IDLParserContext(kStageName, vts, tenantId, sc));
        targetNss = mergeSpec.getTargetNss();
        if (targetNss.coll().empty()) {
            // If the $merge spec is an object, the target namespace can be specified as a
            // string on an object value of the 'into' field. In case it was a string, we want
            // to use the same semantics as above, that is, treat it as a collection name. This
            // is different from the NamespaceString semantics which treats it as a database
            // name. So, if the target namespace collection is empty, we'll use the default
            // database name as a target database, and the provided namespace value as a
            // collection name.
            targetNss = NamespaceStringUtil::deserialize(
                defaultDb, targetNss.serializeWithoutTenantPrefix_UNSAFE());
        } else if (targetNss.dbSize() == 0) {
            // Use the default database name if it wasn't specified explicilty.
            targetNss = NamespaceStringUtil::deserialize(defaultDb, targetNss.coll());
        }
    }

    mergeSpec.setTargetNss(std::move(targetNss));

    return mergeSpec;
}

/**
 * Converts an array of field names into a set of FieldPath. Throws if 'fields' contains
 * duplicate elements.
 */
boost::optional<std::set<FieldPath>> convertToFieldPaths(
    const boost::optional<std::vector<std::string>>& fields) {

    if (!fields)
        return boost::none;

    std::set<FieldPath> fieldPaths;

    for (const auto& field : *fields) {
        const auto res = fieldPaths.insert(FieldPath(field));
        uassert(31465, str::stream() << "Found a duplicate field '" << field << "'", res.second);
    }
    return fieldPaths;
}

auto withErrorContext(const auto&& callback, std::string_view errorMessage) {
    try {
        return callback();
    } catch (DBException& ex) {
        ex.addContext(errorMessage);
        throw;
    }
}
}  // namespace

std::unique_ptr<DocumentSourceMerge::LiteParsed> DocumentSourceMerge::LiteParsed::parse(
    const NamespaceString& nss, const BSONElement& spec, const LiteParserOptions& options) {
    uassert(ErrorCodes::TypeMismatch,
            fmt::format("{} requires a string or object argument, but found {}",
                        kStageName,
                        typeName(spec.type())),
            spec.type() == BSONType::string || spec.type() == BSONType::object);

    auto mergeSpec = parseMergeSpecAndResolveTargetNamespace(spec, nss.dbName());
    auto targetNss = mergeSpec.getTargetNss();

    uassert(ErrorCodes::InvalidNamespace,
            fmt::format(
                "Invalid {} target namespace: '{}'", kStageName, targetNss.toStringForErrorMsg()),
            targetNss.isValid());

    auto whenMatched =
        mergeSpec.getWhenMatched() ? mergeSpec.getWhenMatched()->mode : kDefaultWhenMatched;
    auto whenNotMatched = mergeSpec.getWhenNotMatched().value_or(kDefaultWhenNotMatched);

    uassert(51181,
            fmt::format("Combination of {} modes 'whenMatched: {}' and 'whenNotMatched: {}' "
                        "is not supported",
                        kStageName,
                        idl::serialize(whenMatched),
                        idl::serialize(whenNotMatched)),
            isSupportedMergeMode(whenMatched, whenNotMatched));
    boost::optional<OwnedLiteParsedPipeline> ownedPipeline;
    if (whenMatched == MergeWhenMatchedModeEnum::kPipeline) {
        auto pipeline = mergeSpec.getWhenMatched()->pipeline;
        tassert(11282975, "$merge spec is missing the whenMatched pipeline", pipeline);
        // The whenMatched pipeline runs against documents in the target collection, so parse it
        // with targetNss to match the convention used by $lookup and $unionWith.
        ownedPipeline = OwnedLiteParsedPipeline(targetNss, *pipeline, options);
    }
    return std::make_unique<DocumentSourceMerge::LiteParsed>(
        spec, std::move(targetNss), whenMatched, whenNotMatched, std::move(ownedPipeline));
}

PrivilegeVector DocumentSourceMerge::LiteParsed::requiredPrivileges(
    bool isMongos, bool bypassDocumentValidation) const {
    tassert(11282974, "Missing foreignNss", _foreignNss);
    // AllowInsertWithUpdateBackupStrategies doesn't affect required priviledges, so we can pass any
    // value.
    auto actions = ActionSet{
        getMergeStrategyDescriptors(MergeProcessor::AllowInsertWithUpdateBackupStrategies{false})
            .at({_whenMatched, _whenNotMatched})
            .actions};
    if (bypassDocumentValidation) {
        actions.addAction(ActionType::bypassDocumentValidation);
    }

    return {{ResourcePattern::forExactNamespace(*_foreignNss), actions}};
}

DocumentSourceMerge::DocumentSourceMerge(
    NamespaceString outputNs,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    WhenMatched whenMatched,
    WhenNotMatched whenNotMatched,
    boost::optional<BSONObj> letVariables,
    boost::optional<std::vector<BSONObj>> pipeline,
    std::set<FieldPath> mergeOnFields,
    boost::optional<ChunkVersion> collectionPlacementVersion,
    bool allowMergeOnNullishValues,
    MergeProcessor::AllowInsertWithUpdateBackupStrategies allowInsertWithUpdateBackupStrategies)
    : DocumentSourceWriter(kStageName, std::move(outputNs), expCtx),
      _mergeOnFields(std::make_shared<std::set<FieldPath>>(std::move(mergeOnFields))),
      _mergeOnFieldsIncludesId(_mergeOnFields->count("_id") == 1),
      _mergeProcessor(std::make_shared<MergeProcessor>(expCtx,
                                                       whenMatched,
                                                       whenNotMatched,
                                                       std::move(letVariables),
                                                       std::move(pipeline),
                                                       std::move(collectionPlacementVersion),
                                                       allowMergeOnNullishValues,
                                                       allowInsertWithUpdateBackupStrategies)) {};


boost::intrusive_ptr<DocumentSource> DocumentSourceMerge::create(
    NamespaceString outputNs,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    WhenMatched whenMatched,
    WhenNotMatched whenNotMatched,
    boost::optional<BSONObj> letVariables,
    boost::optional<std::vector<BSONObj>> pipeline,
    std::set<FieldPath> mergeOnFields,
    boost::optional<ChunkVersion> collectionPlacementVersion,
    bool allowMergeOnNullishValues) {
    return DocumentSourceMerge::create(
        std::move(outputNs),
        expCtx,
        whenMatched,
        whenNotMatched,
        std::move(letVariables),
        std::move(pipeline),
        std::move(mergeOnFields),
        std::move(collectionPlacementVersion),
        allowMergeOnNullishValues,
        MergeProcessor::AllowInsertWithUpdateBackupStrategies{
            feature_flags::gFeatureFlagMergeStageInsertWithUpdateBackup.isEnabled()});
}

boost::intrusive_ptr<DocumentSource> DocumentSourceMerge::create(
    NamespaceString outputNs,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    WhenMatched whenMatched,
    WhenNotMatched whenNotMatched,
    boost::optional<BSONObj> letVariables,
    boost::optional<std::vector<BSONObj>> pipeline,
    std::set<FieldPath> mergeOnFields,
    boost::optional<ChunkVersion> collectionPlacementVersion,
    bool allowMergeOnNullishValues,
    MergeProcessor::AllowInsertWithUpdateBackupStrategies allowInsertWithUpdateBackupStrategies) {
    uassert(51189,
            fmt::format("Combination of {} modes 'whenMatched: {}' and 'whenNotMatched: {}' "
                        "is not supported",
                        kStageName,
                        idl::serialize(whenMatched),
                        idl::serialize(whenNotMatched)),
            isSupportedMergeMode(whenMatched, whenNotMatched));

    uassert(ErrorCodes::InvalidNamespace,
            fmt::format(
                "Invalid {} target namespace: '{}'", kStageName, outputNs.toStringForErrorMsg()),
            outputNs.isValid());

    uassert(ErrorCodes::OperationNotSupportedInTransaction,
            fmt::format("{} cannot be used in a transaction", kStageName),
            !expCtx->getOperationContext()->inMultiDocumentTransaction());

    // We should only do validation that depends on the client running the query when we are
    // actually going to run the query. Other lifecycle stages like query shape re-parsing can skip
    // it.
    if (expCtx->getMongoProcessInterface()->isExpectedToExecuteQueries()) {
        uassert(31319,
                fmt::format("Cannot {} to special collection: {}", kStageName, outputNs.coll()),
                !outputNs.isSystem() ||
                    (outputNs.isSystemStatsCollection() &&
                     isInternalClient(expCtx->getOperationContext()->getClient())));

        uassert(31320,
                fmt::format("Cannot {} to internal database: {}",
                            kStageName,
                            outputNs.dbName().toStringForErrorMsg()),
                !outputNs.isOnInternalDb() ||
                    isInternalClient(expCtx->getOperationContext()->getClient()));
    }

    if (whenMatched == WhenMatched::kPipeline) {
        // If unspecified, 'letVariables' defaults to {new: "$$ROOT"}.
        letVariables = letVariables.value_or(kDefaultPipelineLet);
        auto newElt = letVariables->getField("new"sv);
        uassert(51273,
                "'let' may not define a value for the reserved 'new' variable other than '$$ROOT'",
                !newElt || newElt.valueStringDataSafe() == "$$ROOT"sv);
        // If the 'new' variable is missing and this is a {whenNotMatched: "insert"} merge, then the
        // new document *must* be serialized with the update request. Add it to the let variables.
        if (!newElt && whenNotMatched == WhenNotMatched::kInsert) {
            letVariables = letVariables->addField(kDefaultPipelineLet.firstElement());
        }
    } else {
        // Ensure the 'let' argument cannot be used with any other merge modes.
        uassert(51199,
                fmt::format("Cannot use 'let' variables with 'whenMatched: {}' mode",
                            idl::serialize(whenMatched)),
                !letVariables);
    }

    return new DocumentSourceMerge(std::move(outputNs),
                                   expCtx,
                                   whenMatched,
                                   whenNotMatched,
                                   std::move(letVariables),
                                   std::move(pipeline),
                                   std::move(mergeOnFields),
                                   std::move(collectionPlacementVersion),
                                   allowMergeOnNullishValues,
                                   allowInsertWithUpdateBackupStrategies);
}

boost::intrusive_ptr<DocumentSource> DocumentSourceMerge::createFromBson(
    BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(51182,
            fmt::format(
                "{} only supports a string or object argument, not {}", kStageName, spec.type()),
            spec.type() == BSONType::string || spec.type() == BSONType::object);

    auto mergeSpec = parseMergeSpecAndResolveTargetNamespace(
        spec, expCtx->getNamespaceString().dbName(), expCtx->getSerializationContext());
    auto targetNss = mergeSpec.getTargetNss();
    auto whenMatched =
        mergeSpec.getWhenMatched() ? mergeSpec.getWhenMatched()->mode : kDefaultWhenMatched;
    auto whenNotMatched = mergeSpec.getWhenNotMatched().value_or(kDefaultWhenNotMatched);
    auto pipeline = mergeSpec.getWhenMatched() ? mergeSpec.getWhenMatched()->pipeline : boost::none;
    auto fieldPaths = convertToFieldPaths(mergeSpec.getOn());
    auto [mergeOnFields, collectionPlacementVersion, supportingUniqueIndex] =
        expCtx->getMongoProcessInterface()->ensureFieldsUniqueOrResolveDocumentKey(
            expCtx, std::move(fieldPaths), mergeSpec.getTargetCollectionVersion(), targetNss);

    bool allowMergeOnNullishValues = false;
    if (feature_flags::gFeatureFlagAllowMergeOnNullishValues
            .isEnabledUseLastLTSFCVWhenUninitialized(
                VersionContext::getDecoration(expCtx->getOperationContext()),
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        allowMergeOnNullishValues = mergeSpec.getAllowMergeOnNullishValues().value_or(
            supportingUniqueIndex == MongoProcessInterface::SupportingUniqueIndex::Full);
    }
    return DocumentSourceMerge::create(std::move(targetNss),
                                       expCtx,
                                       whenMatched,
                                       whenNotMatched,
                                       mergeSpec.getLet(),
                                       std::move(pipeline),
                                       std::move(mergeOnFields),
                                       collectionPlacementVersion,
                                       allowMergeOnNullishValues);
}

StageConstraints DocumentSourceMerge::constraints(PipelineSplitState pipeState) const {
    StageConstraints result{StreamType::kStreaming,
                            PositionRequirement::kLast,
                            HostTypeRequirement::kNone,
                            DiskUseRequirement::kWritesPersistentData,
                            FacetRequirement::kNotAllowed,
                            TransactionRequirement::kNotAllowed,
                            LookupRequirement::kNotAllowed,
                            UnionRequirement::kNotAllowed};
    if (pipeState == PipelineSplitState::kSplitForMerge) {
        result.mergeShardId = getMergeShardId();
    }
    return result;
}

boost::optional<DocumentSource::DistributedPlanLogic> DocumentSourceMerge::distributedPlanLogic(
    const DistributedPlanContext* ctx) {
    return getMergeShardId() ? DocumentSourceWriter::distributedPlanLogic(ctx) : boost::none;
}

Value DocumentSourceMerge::serialize(const query_shape::SerializationOptions& opts) const {
    DocumentSourceMergeSpec spec;
    spec.setTargetNss(getOutputNs());
    const auto& letVariables = _mergeProcessor->getLetVariables();
    spec.setLet([&]() -> boost::optional<BSONObj> {
        if (letVariables.empty()) {
            return boost::none;
        }

        BSONObjBuilder bob;
        for (const auto& letVar : letVariables) {
            bob << opts.serializeFieldPathFromString(letVar.name)
                << letVar.expression->serialize(opts);
        }
        return bob.obj();
    }());

    const auto& descriptor = _mergeProcessor->getMergeStrategyDescriptor();
    spec.setWhenMatched(MergeWhenMatchedPolicy{
        descriptor.mode.first, [&]() -> boost::optional<std::vector<BSONObj>> {
            const auto& pipeline = _mergeProcessor->getPipeline();
            if (!pipeline.has_value()) {
                return boost::none;
            }
            auto expCtxWithLetVariables = makeCopyFromExpressionContext(getExpCtx(), getOutputNs());
            if (spec.getLet()) {
                BSONObjBuilder cleanLetSpecBuilder;
                for (const auto& letVar : letVariables) {
                    cleanLetSpecBuilder.append(letVar.name, BSONObj{});
                }
                expCtxWithLetVariables->variables.seedVariablesWithLetParameters(
                    expCtxWithLetVariables.get(),
                    cleanLetSpecBuilder.obj(),
                    [](const Expression* expr) {
                        return expression::getDependencies(expr).hasNoRequirements();
                    });
            }
            return withErrorContext(
                [&]() {
                    return pipeline_factory::makePipeline(pipeline.value(),
                                                          expCtxWithLetVariables,
                                                          pipeline_factory::kOptionsMinimal)
                        ->serializeToBson(opts);
                },
                "Error parsing $merge.whenMatched pipeline"sv);
        }()});
    spec.setWhenNotMatched(descriptor.mode.second);
    spec.setOn([&]() {
        std::vector<std::string> mergeOnFields;
        for (const auto& path : *_mergeOnFields) {
            mergeOnFields.push_back(path.fullPath());
        }
        return mergeOnFields;
    }());
    // Do not serialize 'targetCollectionVersion' and 'allowMergeOnNullishValues attribute as it is
    // not part of the query shape.
    if (opts.isKeepingLiteralsUnchanged()) {
        spec.setTargetCollectionVersion(_mergeProcessor->getCollectionPlacementVersion());
        if (feature_flags::gFeatureFlagAllowMergeOnNullishValues
                .isEnabledUseLastLTSFCVWhenUninitialized(
                    VersionContext::getDecoration(getExpCtx()->getOperationContext()),
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
            spec.setAllowMergeOnNullishValues(_mergeProcessor->getAllowMergeOnNullishValues());
        }
    }
    return Value(Document{{getSourceName(), spec.toBSON(opts)}});
}

}  // namespace mongo
