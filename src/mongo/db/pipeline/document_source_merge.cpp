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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <iosfwd>
#include <map>
#include <tuple>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/bson/bsontypes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/pipeline/document_source_merge.h"
#include "mongo/db/pipeline/document_source_merge_gen.h"
#include "mongo/db/pipeline/document_source_merge_spec.h"
#include "mongo/db/pipeline/variable_validation.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_component.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {
using namespace fmt::literals;

MONGO_FAIL_POINT_DEFINE(hangWhileBuildingDocumentSourceMergeBatch);
REGISTER_DOCUMENT_SOURCE(merge,
                         DocumentSourceMerge::LiteParsed::parse,
                         DocumentSourceMerge::createFromBson,
                         AllowedWithApiStrict::kAlways);

namespace {

using WhenMatched = MergeStrategyDescriptor::WhenMatched;
using WhenNotMatched = MergeStrategyDescriptor::WhenNotMatched;

constexpr auto kStageName = DocumentSourceMerge::kStageName;
const auto kDefaultPipelineLet = BSON("new"
                                      << "$$ROOT");

/**
 * Checks if a pair of whenMatched/whenNotMatched merge modes is supported.
 */
bool isSupportedMergeMode(WhenMatched whenMatched, WhenNotMatched whenNotMatched) {
    return getMergeStrategyDescriptors().count({whenMatched, whenNotMatched}) > 0;
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
    if (spec.type() == BSONType::String) {
        targetNss = NamespaceStringUtil::deserialize(defaultDb, spec.valueStringData());
    } else {
        const auto tenantId = defaultDb.tenantId();
        const auto vts = tenantId
            ? boost::make_optional(auth::ValidatedTenancyScopeFactory::create(
                  *tenantId, auth::ValidatedTenancyScopeFactory::TrustedForInnerOpMsgRequestTag{}))
            : boost::none;
        mergeSpec = DocumentSourceMergeSpec::parse(
            IDLParserContext(kStageName, false /* apiStrict */, vts, tenantId, sc),
            spec.embeddedObject());
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
}  // namespace

std::unique_ptr<DocumentSourceMerge::LiteParsed> DocumentSourceMerge::LiteParsed::parse(
    const NamespaceString& nss, const BSONElement& spec) {
    uassert(ErrorCodes::TypeMismatch,
            "{} requires a string or object argument, but found {}"_format(kStageName,
                                                                           typeName(spec.type())),
            spec.type() == BSONType::String || spec.type() == BSONType::Object);

    auto mergeSpec = parseMergeSpecAndResolveTargetNamespace(spec, nss.dbName());
    auto targetNss = mergeSpec.getTargetNss();

    uassert(ErrorCodes::InvalidNamespace,
            "Invalid {} target namespace: '{}'"_format(kStageName, targetNss.toStringForErrorMsg()),
            targetNss.isValid());

    auto whenMatched =
        mergeSpec.getWhenMatched() ? mergeSpec.getWhenMatched()->mode : kDefaultWhenMatched;
    auto whenNotMatched = mergeSpec.getWhenNotMatched().value_or(kDefaultWhenNotMatched);

    uassert(51181,
            "Combination of {} modes 'whenMatched: {}' and 'whenNotMatched: {}' "
            "is not supported"_format(kStageName,
                                      MergeWhenMatchedMode_serializer(whenMatched),
                                      MergeWhenNotMatchedMode_serializer(whenNotMatched)),
            isSupportedMergeMode(whenMatched, whenNotMatched));
    boost::optional<LiteParsedPipeline> liteParsedPipeline;
    if (whenMatched == MergeWhenMatchedModeEnum::kPipeline) {
        auto pipeline = mergeSpec.getWhenMatched()->pipeline;
        invariant(pipeline);
        liteParsedPipeline = LiteParsedPipeline(nss, *pipeline);
    }
    return std::make_unique<DocumentSourceMerge::LiteParsed>(spec.fieldName(),
                                                             std::move(targetNss),
                                                             whenMatched,
                                                             whenNotMatched,
                                                             std::move(liteParsedPipeline));
}

PrivilegeVector DocumentSourceMerge::LiteParsed::requiredPrivileges(
    bool isMongos, bool bypassDocumentValidation) const {
    invariant(_foreignNss);
    auto actions =
        ActionSet{getMergeStrategyDescriptors().at({_whenMatched, _whenNotMatched}).actions};
    if (bypassDocumentValidation) {
        actions.addAction(ActionType::bypassDocumentValidation);
    }

    return {{ResourcePattern::forExactNamespace(*_foreignNss), actions}};
}

DocumentSourceMerge::DocumentSourceMerge(NamespaceString outputNs,
                                         const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         WhenMatched whenMatched,
                                         WhenNotMatched whenNotMatched,
                                         boost::optional<BSONObj> letVariables,
                                         boost::optional<std::vector<BSONObj>> pipeline,
                                         std::set<FieldPath> mergeOnFields,
                                         boost::optional<ChunkVersion> collectionPlacementVersion)
    : DocumentSourceWriter(kStageName.rawData(), std::move(outputNs), expCtx),
      _mergeOnFields(std::move(mergeOnFields)),
      _mergeOnFieldsIncludesId(_mergeOnFields.count("_id") == 1) {
    _mergeProcessor.emplace(expCtx,
                            whenMatched,
                            whenNotMatched,
                            std::move(letVariables),
                            std::move(pipeline),
                            std::move(collectionPlacementVersion));
}

boost::intrusive_ptr<DocumentSource> DocumentSourceMerge::create(
    NamespaceString outputNs,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    WhenMatched whenMatched,
    WhenNotMatched whenNotMatched,
    boost::optional<BSONObj> letVariables,
    boost::optional<std::vector<BSONObj>> pipeline,
    std::set<FieldPath> mergeOnFields,
    boost::optional<ChunkVersion> collectionPlacementVersion) {
    uassert(51189,
            "Combination of {} modes 'whenMatched: {}' and 'whenNotMatched: {}' "
            "is not supported"_format(kStageName,
                                      MergeWhenMatchedMode_serializer(whenMatched),
                                      MergeWhenNotMatchedMode_serializer(whenNotMatched)),
            isSupportedMergeMode(whenMatched, whenNotMatched));

    uassert(ErrorCodes::InvalidNamespace,
            "Invalid {} target namespace: '{}'"_format(kStageName, outputNs.toStringForErrorMsg()),
            outputNs.isValid());

    uassert(ErrorCodes::OperationNotSupportedInTransaction,
            "{} cannot be used in a transaction"_format(kStageName),
            !expCtx->opCtx->inMultiDocumentTransaction());

    uassert(
        31319,
        "Cannot {} to special collection: {}"_format(kStageName, outputNs.coll()),
        !outputNs.isSystem() ||
            (outputNs.isSystemStatsCollection() && isInternalClient(expCtx->opCtx->getClient())));

    uassert(31320,
            "Cannot {} to internal database: {}"_format(kStageName,
                                                        outputNs.dbName().toStringForErrorMsg()),
            !outputNs.isOnInternalDb() || isInternalClient(expCtx->opCtx->getClient()));

    if (whenMatched == WhenMatched::kPipeline) {
        // If unspecified, 'letVariables' defaults to {new: "$$ROOT"}.
        letVariables = letVariables.value_or(kDefaultPipelineLet);
        auto newElt = letVariables->getField("new"_sd);
        uassert(51273,
                "'let' may not define a value for the reserved 'new' variable other than '$$ROOT'",
                !newElt || newElt.valueStringDataSafe() == "$$ROOT"_sd);
        // If the 'new' variable is missing and this is a {whenNotMatched: "insert"} merge, then the
        // new document *must* be serialized with the update request. Add it to the let variables.
        if (!newElt && whenNotMatched == WhenNotMatched::kInsert) {
            letVariables = letVariables->addField(kDefaultPipelineLet.firstElement());
        }
    } else {
        // Ensure the 'let' argument cannot be used with any other merge modes.
        uassert(51199,
                "Cannot use 'let' variables with 'whenMatched: {}' mode"_format(
                    MergeWhenMatchedMode_serializer(whenMatched)),
                !letVariables);
    }

    return new DocumentSourceMerge(std::move(outputNs),
                                   expCtx,
                                   whenMatched,
                                   whenNotMatched,
                                   std::move(letVariables),
                                   std::move(pipeline),
                                   std::move(mergeOnFields),
                                   std::move(collectionPlacementVersion));
}

boost::intrusive_ptr<DocumentSource> DocumentSourceMerge::createFromBson(
    BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(51182,
            "{} only supports a string or object argument, not {}"_format(kStageName, spec.type()),
            spec.type() == BSONType::String || spec.type() == BSONType::Object);

    auto mergeSpec = parseMergeSpecAndResolveTargetNamespace(
        spec, expCtx->ns.dbName(), expCtx->serializationCtxt);
    auto targetNss = mergeSpec.getTargetNss();
    auto whenMatched =
        mergeSpec.getWhenMatched() ? mergeSpec.getWhenMatched()->mode : kDefaultWhenMatched;
    auto whenNotMatched = mergeSpec.getWhenNotMatched().value_or(kDefaultWhenNotMatched);
    auto pipeline = mergeSpec.getWhenMatched() ? mergeSpec.getWhenMatched()->pipeline : boost::none;
    auto fieldPaths = convertToFieldPaths(mergeSpec.getOn());
    auto [mergeOnFields, collectionPlacementVersion] =
        expCtx->mongoProcessInterface->ensureFieldsUniqueOrResolveDocumentKey(
            expCtx, std::move(fieldPaths), mergeSpec.getTargetCollectionVersion(), targetNss);

    return DocumentSourceMerge::create(std::move(targetNss),
                                       expCtx,
                                       whenMatched,
                                       whenNotMatched,
                                       mergeSpec.getLet(),
                                       std::move(pipeline),
                                       std::move(mergeOnFields),
                                       collectionPlacementVersion);
}

StageConstraints DocumentSourceMerge::constraints(Pipeline::SplitState pipeState) const {
    StageConstraints result{StreamType::kStreaming,
                            PositionRequirement::kLast,
                            HostTypeRequirement::kNone,
                            DiskUseRequirement::kWritesPersistentData,
                            FacetRequirement::kNotAllowed,
                            TransactionRequirement::kNotAllowed,
                            LookupRequirement::kNotAllowed,
                            UnionRequirement::kNotAllowed};
    if (pipeState == Pipeline::SplitState::kSplitForMerge) {
        result.mergeShardId = getMergeShardId();
    }
    return result;
}

boost::optional<DocumentSource::DistributedPlanLogic> DocumentSourceMerge::distributedPlanLogic() {
    return getMergeShardId() ? DocumentSourceWriter::distributedPlanLogic() : boost::none;
}

Value DocumentSourceMerge::serialize(const SerializationOptions& opts) const {
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
            auto expCtxWithLetVariables = pExpCtx->copyWith(getOutputNs());
            if (spec.getLet()) {
                BSONObjBuilder cleanLetSpecBuilder;
                for (const auto& letVar : letVariables) {
                    cleanLetSpecBuilder.append(letVar.name, BSONObj{});
                }
                expCtxWithLetVariables->variables.seedVariablesWithLetParameters(
                    expCtxWithLetVariables.get(), cleanLetSpecBuilder.obj());
            }
            return Pipeline::parse(pipeline.value(), expCtxWithLetVariables)->serializeToBson(opts);
        }()});
    spec.setWhenNotMatched(descriptor.mode.second);
    spec.setOn([&]() {
        std::vector<std::string> mergeOnFields;
        for (const auto& path : _mergeOnFields) {
            mergeOnFields.push_back(path.fullPath());
        }
        return mergeOnFields;
    }());
    // Do not serialize 'targetCollectionVersion' attribute as it is not part of the query shape.
    if (opts.literalPolicy == LiteralSerializationPolicy::kUnchanged) {
        spec.setTargetCollectionVersion(_mergeProcessor->getCollectionPlacementVersion());
    }
    return Value(Document{{getSourceName(), spec.toBSON(opts)}});
}

std::pair<DocumentSourceMerge::BatchObject, int> DocumentSourceMerge::makeBatchObject(
    Document doc) const {
    auto batchObject =
        _mergeProcessor->makeBatchObject(std::move(doc), _mergeOnFields, _mergeOnFieldsIncludesId);
    auto upsertType = _mergeProcessor->getMergeStrategyDescriptor().upsertType;

    tassert(6628901, "_writeSizeEstimator should be initialized", _writeSizeEstimator);
    int size = _writeSizeEstimator->estimateUpdateSizeBytes(batchObject, upsertType);
    return {std::move(batchObject), size};
}

void DocumentSourceMerge::flush(BatchedCommandRequest bcr, BatchedObjects batch) {
    try {
        DocumentSourceWriteBlock writeBlock(pExpCtx->opCtx);
        _mergeProcessor->flush(getOutputNs(), std::move(bcr), std::move(batch));
    } catch (const ExceptionFor<ErrorCodes::ImmutableField>& ex) {
        uassertStatusOKWithContext(ex.toStatus(),
                                   "$merge failed to update the matching document, did you "
                                   "attempt to modify the _id or the shard key?");
    } catch (const ExceptionFor<ErrorCodes::DuplicateKey>& ex) {
        // A DuplicateKey error could be due to a collision on the 'on' fields or on any other
        // unique index.
        auto dupKeyPattern = ex->getKeyPattern();
        bool dupKeyFromMatchingOnFields =
            (static_cast<size_t>(dupKeyPattern.nFields()) == _mergeOnFields.size()) &&
            std::all_of(_mergeOnFields.begin(), _mergeOnFields.end(), [&](auto onField) {
                return dupKeyPattern.hasField(onField.fullPath());
            });

        if (_mergeProcessor->getMergeStrategyDescriptor().mode ==
                MergeStrategyDescriptor::kFailInsertMode &&
            dupKeyFromMatchingOnFields) {
            uassertStatusOKWithContext(
                ex.toStatus(),
                "$merge with whenMatched: fail found an existing document with "
                "the same values for the 'on' fields");

        } else {
            uassertStatusOKWithContext(ex.toStatus(), "$merge failed due to a DuplicateKey error");
        }
    }
}

BatchedCommandRequest DocumentSourceMerge::makeBatchedWriteRequest() const {
    return _mergeProcessor->getMergeStrategyDescriptor().batchedCommandGenerator(pExpCtx,
                                                                                 getOutputNs());
}

void DocumentSourceMerge::waitWhileFailPointEnabled() {
    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangWhileBuildingDocumentSourceMergeBatch,
        pExpCtx->opCtx,
        "hangWhileBuildingDocumentSourceMergeBatch",
        []() {
            LOGV2(
                20900,
                "Hanging aggregation due to 'hangWhileBuildingDocumentSourceMergeBatch' failpoint");
        },
        getOutputNs());
}
}  // namespace mongo
