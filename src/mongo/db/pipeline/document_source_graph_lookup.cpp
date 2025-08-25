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

// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/db/pipeline/document_source_graph_lookup.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/pipeline/sort_reorder_helpers.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/stage_memory_limit_knobs/knobs.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"

#include <memory>

#include <absl/container/node_hash_map.h>
#include <absl/container/node_hash_set.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

namespace {

// Parses $graphLookup 'from' field. The 'from' field must be a string.
NamespaceString parseGraphLookupFromAndResolveNamespace(const BSONElement& elem,
                                                        const DatabaseName& defaultDb) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "$graphLookup 'from' field must be a string, but found "
                          << typeName(elem.type()),
            elem.type() == BSONType::string);

    NamespaceString fromNss(NamespaceStringUtil::deserialize(defaultDb, elem.valueStringData()));
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "invalid $graphLookup namespace: " << fromNss.toStringForErrorMsg(),
            fromNss.isValid());
    return fromNss;
}

}  // namespace

using boost::intrusive_ptr;

std::unique_ptr<DocumentSourceGraphLookUp::LiteParsed> DocumentSourceGraphLookUp::LiteParsed::parse(
    const NamespaceString& nss, const BSONElement& spec, const LiteParserOptions& options) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "the $graphLookup stage specification must be an object, but found "
                          << typeName(spec.type()),
            spec.type() == BSONType::object);

    auto specObj = spec.Obj();
    auto fromElement = specObj["from"];
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "missing 'from' option to $graphLookup stage specification: "
                          << specObj,
            fromElement);

    return std::make_unique<LiteParsed>(
        spec.fieldName(), parseGraphLookupFromAndResolveNamespace(fromElement, nss.dbName()));
}

REGISTER_DOCUMENT_SOURCE(graphLookup,
                         DocumentSourceGraphLookUp::LiteParsed::parse,
                         DocumentSourceGraphLookUp::createFromBson,
                         AllowedWithApiStrict::kAlways);
ALLOCATE_DOCUMENT_SOURCE_ID(graphLookup, DocumentSourceGraphLookUp::id)

const char* DocumentSourceGraphLookUp::getSourceName() const {
    return kStageName.data();
}

boost::optional<ShardId> DocumentSourceGraphLookUp::computeMergeShardId() const {
    // Note that we can only check sharding state when we're on router as we may be holding
    // locks on mongod (which would inhibit looking up sharding state in the catalog cache).
    if (getExpCtx()->getInRouter()) {
        // Only nominate a merging shard if the outer collection is unsharded, or if the
        // pipeline is running as a collectionless aggregate (e.g. $documents is substituted for
        // the "from" field).
        if (getExpCtx()->getNamespaceString().isCollectionlessAggregateNS() ||
            !getExpCtx()->getMongoProcessInterface()->isSharded(
                getExpCtx()->getOperationContext(), getExpCtx()->getNamespaceString())) {
            return getExpCtx()->getMongoProcessInterface()->determineSpecificMergeShard(
                getExpCtx()->getOperationContext(), getFromNs());
        }
    } else if (const auto shardingState = ShardingState::get(getExpCtx()->getOperationContext());
               shardingState->enabled()) {
        // This path can be taken on a replica set where the sharding state is not yet
        // initialized and the node does not have a shard id. Note that the sharding state being
        // disabled may mean we are on a secondary of a shard server node that hasn't yet
        // initialized its sharding state. Since this choice is only for performance, that is
        // acceptable.
        return shardingState->shardId();
    }
    return boost::none;
}

bool DocumentSourceGraphLookUp::foreignShardedGraphLookupAllowed() const {
    const auto fcvSnapshot = serverGlobalParams.mutableFCV.acquireFCVSnapshot();
    return !getExpCtx()->getOperationContext()->inMultiDocumentTransaction() ||
        gFeatureFlagAllowAdditionalParticipants.isEnabled(fcvSnapshot);
}

boost::optional<DocumentSource::DistributedPlanLogic>
DocumentSourceGraphLookUp::distributedPlanLogic() {
    // If $graphLookup into a sharded foreign collection is allowed, top-level $graphLookup
    // stages can run in parallel on the shards.
    if (foreignShardedGraphLookupAllowed() && getExpCtx()->getSubPipelineDepth() == 0) {
        if (getMergeShardId()) {
            return DistributedPlanLogic{nullptr, this, boost::none};
        }
        return boost::none;
    }

    // {shardsStage, mergingStage, sortPattern}
    return DistributedPlanLogic{nullptr, this, boost::none};
}

DocumentSource::GetModPathsReturn DocumentSourceGraphLookUp::getModifiedPaths() const {
    OrderedPathSet modifiedPaths{getAsField().fullPath()};
    if (_unwind) {
        auto pathsModifiedByUnwind = _unwind.value()->getModifiedPaths();
        invariant(pathsModifiedByUnwind.type == GetModPathsReturn::Type::kFiniteSet);
        modifiedPaths.insert(pathsModifiedByUnwind.paths.begin(),
                             pathsModifiedByUnwind.paths.end());
    }
    return {GetModPathsReturn::Type::kFiniteSet, std::move(modifiedPaths), {}};
}

StageConstraints DocumentSourceGraphLookUp::constraints(PipelineSplitState pipeState) const {
    // $graphLookup can execute on a mongos or a shard, so its host type requirement is
    // 'kNone'. If it needs to execute on a specific merging shard, it can request this
    // later.
    StageConstraints constraints(StreamType::kStreaming,
                                 PositionRequirement::kNone,
                                 HostTypeRequirement::kNone,
                                 DiskUseRequirement::kNoDiskUse,
                                 FacetRequirement::kAllowed,
                                 TransactionRequirement::kAllowed,
                                 LookupRequirement::kAllowed,
                                 UnionRequirement::kAllowed);

    constraints.canSwapWithMatch = true;
    constraints.canSwapWithSkippingOrLimitingStage = !_unwind;

    // If this $graphLookup is on the merging half of the pipeline and the inner collection
    // isn't sharded (that is, it is either unsplittable or untracked), then we should merge
    // on the shard which owns the inner collection.
    if (pipeState == PipelineSplitState::kSplitForMerge) {
        constraints.mergeShardId = getMergeShardId();
    }

    return constraints;
}

DocumentSourceContainer::iterator DocumentSourceGraphLookUp::doOptimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    invariant(*itr == this);

    if (std::next(itr) == container->end()) {
        return container->end();
    }

    // If we are not already handling an $unwind stage internally, we can combine with the
    // following $unwind stage.
    auto nextUnwind = dynamic_cast<DocumentSourceUnwind*>((*std::next(itr)).get());
    if (nextUnwind && !_unwind && nextUnwind->getUnwindPath() == getAsField().fullPath()) {
        _unwind = std::move(nextUnwind);
        container->erase(std::next(itr));
        return itr;
    }

    // If the following stage is $sort and there is no internal $unwind, consider pushing it
    // ahead of $graphLookup.
    if (!_unwind) {
        itr = tryReorderingWithSort(itr, container);
        if (*itr != this) {
            return itr;
        }
    }

    return std::next(itr);
}

void DocumentSourceGraphLookUp::serializeToArray(std::vector<Value>& array,
                                                 const SerializationOptions& opts) const {
    // Do not include tenantId in serialized 'from' namespace.
    auto fromValue = getExpCtx()->getNamespaceString().isEqualDb(getFromNs())
        ? Value(opts.serializeIdentifier(getFromNs().coll()))
        : Value(Document{{"db",
                          opts.serializeIdentifier(
                              getFromNs().dbName().serializeWithoutTenantPrefix_UNSAFE())},
                         {"coll", opts.serializeIdentifier(getFromNs().coll())}});

    // Serialize default options.
    MutableDocument spec(DOC("from"
                             << fromValue << "as" << opts.serializeFieldPath(getAsField())
                             << "connectToField" << opts.serializeFieldPath(getConnectToField())
                             << "connectFromField" << opts.serializeFieldPath(getConnectFromField())
                             << "startWith" << getStartWithField()->serialize(opts)));

    // depthField is optional; serialize it if it was specified.
    if (getDepthField()) {
        spec["depthField"] = Value(opts.serializeFieldPath(*getDepthField()));
    }

    if (getMaxDepth()) {
        spec["maxDepth"] = Value(opts.serializeLiteral(*getMaxDepth()));
    }

    if (getAdditionalFilter()) {
        if (opts.isSerializingForQueryStats()) {
            auto matchExpr =
                uassertStatusOK(MatchExpressionParser::parse(*getAdditionalFilter(), getExpCtx()));
            spec["restrictSearchWithMatch"] = Value(matchExpr->serialize(opts));
        } else {
            spec["restrictSearchWithMatch"] = Value(*getAdditionalFilter());
        }
    }

    // If we are explaining, include an absorbed $unwind inside the $graphLookup
    // specification.
    if (_unwind && opts.isSerializingForExplain()) {
        const boost::optional<FieldPath> indexPath = (*_unwind)->indexPath();
        spec["unwinding"] =
            Value(DOC("preserveNullAndEmptyArrays"
                      << opts.serializeLiteral((*_unwind)->preserveNullAndEmptyArrays())
                      << "includeArrayIndex"
                      << (indexPath ? Value(opts.serializeFieldPath(*indexPath)) : Value())));
    }

    MutableDocument out;
    out[getSourceName()] = spec.freezeToValue();

    array.push_back(out.freezeToValue());

    // If we are not explaining, the output of this method must be parseable, so serialize
    // our $unwind into a separate stage.
    if (_unwind && !opts.isSerializingForExplain()) {
        (*_unwind)->serializeToArray(array, opts);
    }
}

void DocumentSourceGraphLookUp::detachSourceFromOperationContext() {
    _fromExpCtx->setOperationContext(nullptr);
}

void DocumentSourceGraphLookUp::reattachSourceToOperationContext(OperationContext* opCtx) {
    _fromExpCtx->setOperationContext(opCtx);
}

bool DocumentSourceGraphLookUp::validateSourceOperationContext(
    const OperationContext* opCtx) const {
    return getExpCtx()->getOperationContext() == opCtx &&
        _fromExpCtx->getOperationContext() == opCtx;
}

DocumentSourceGraphLookUp::DocumentSourceGraphLookUp(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    GraphLookUpParams params,
    boost::optional<boost::intrusive_ptr<DocumentSourceUnwind>> unwindSrc)
    : DocumentSource(kStageName, expCtx),
      _params(std::move(params)),
      _unwind(unwindSrc),
      _variables(expCtx->variables),
      _variablesParseState(expCtx->variablesParseState.copyWith(_variables.useIdGenerator())) {
    if (!getFromNs().isOnInternalDb()) {
        serviceOpCounters(expCtx->getOperationContext()).gotNestedAggregate();
    }

    const auto& resolvedNamespace = getExpCtx()->getResolvedNamespace(getFromNs());
    _fromExpCtx = makeCopyForSubPipelineFromExpressionContext(
        getExpCtx(), resolvedNamespace.ns, resolvedNamespace.uuid);
    _fromExpCtx->setInLookup(true);

    // We append an additional BSONObj to '_fromPipeline' as a placeholder for the $match
    // stage we'll eventually construct from the input document.
    _fromPipeline.reserve(resolvedNamespace.pipeline.size() + 1);
    _fromPipeline = resolvedNamespace.pipeline;
    _fromPipeline.push_back(BSON("$match" << BSONObj()));
}

DocumentSourceGraphLookUp::DocumentSourceGraphLookUp(
    const DocumentSourceGraphLookUp& original,
    const boost::intrusive_ptr<ExpressionContext>& newExpCtx)
    : DocumentSource(kStageName, newExpCtx),
      _params(original._params),
      _fromExpCtx(makeCopyFromExpressionContext(
          original._fromExpCtx,
          original.getExpCtx()->getResolvedNamespace(getFromNs()).ns,
          original.getExpCtx()->getResolvedNamespace(getFromNs()).uuid)),
      _fromPipeline(original._fromPipeline),
      _variables(original._variables),
      _variablesParseState(original._variablesParseState.copyWith(_variables.useIdGenerator())) {
    if (original._unwind) {
        _unwind =
            static_cast<DocumentSourceUnwind*>(original._unwind.value()->clone(getExpCtx()).get());
    }
}

intrusive_ptr<DocumentSourceGraphLookUp> DocumentSourceGraphLookUp::create(
    const intrusive_ptr<ExpressionContext>& expCtx,
    NamespaceString fromNs,
    std::string asField,
    std::string connectFromField,
    std::string connectToField,
    intrusive_ptr<Expression> startWith,
    boost::optional<BSONObj> additionalFilter,
    boost::optional<FieldPath> depthField,
    boost::optional<long long> maxDepth,
    boost::optional<boost::intrusive_ptr<DocumentSourceUnwind>> unwindSrc) {
    intrusive_ptr<DocumentSourceGraphLookUp> source(
        new DocumentSourceGraphLookUp(expCtx,
                                      GraphLookUpParams(std::move(fromNs),
                                                        std::move(asField),
                                                        std::move(connectFromField),
                                                        std::move(connectToField),
                                                        std::move(startWith),
                                                        additionalFilter,
                                                        depthField,
                                                        maxDepth),

                                      unwindSrc));
    return source;
}

intrusive_ptr<DocumentSource> DocumentSourceGraphLookUp::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    NamespaceString from;
    std::string as;
    boost::intrusive_ptr<Expression> startWith;
    std::string connectFromField;
    std::string connectToField;
    boost::optional<FieldPath> depthField;
    boost::optional<long long> maxDepth;
    boost::optional<BSONObj> additionalFilter;

    VariablesParseState vps = expCtx->variablesParseState;

    for (auto&& argument : elem.Obj()) {
        const auto argName = argument.fieldNameStringData();

        if (argName == "startWith") {
            startWith = Expression::parseOperand(expCtx.get(), argument, vps);
            continue;
        } else if (argName == "maxDepth") {
            uassert(40100,
                    str::stream() << "maxDepth must be numeric, found type: "
                                  << typeName(argument.type()),
                    argument.isNumber());
            maxDepth = argument.safeNumberLong();
            uassert(40101,
                    str::stream() << "maxDepth requires a nonnegative argument, found: "
                                  << *maxDepth,
                    *maxDepth >= 0);
            uassert(40102,
                    str::stream() << "maxDepth could not be represented as a long long: "
                                  << *maxDepth,
                    *maxDepth == argument.number());
            continue;
        } else if (argName == "restrictSearchWithMatch") {
            uassert(40185,
                    str::stream() << "restrictSearchWithMatch must be an object, found "
                                  << typeName(argument.type()),
                    argument.type() == BSONType::object);

            // We don't need to keep ahold of the MatchExpression, but we do need to ensure
            // that the specified object is parseable and does not contain extensions.
            uassertStatusOKWithContext(
                MatchExpressionParser::parse(argument.embeddedObject(), expCtx),
                "Failed to parse 'restrictSearchWithMatch' option to $graphLookup");

            additionalFilter = argument.embeddedObject().getOwned();
            continue;
        }

        if (argName == "from" || argName == "as" || argName == "connectFromField" ||
            argName == "depthField" || argName == "connectToField") {
            // All remaining arguments to $graphLookup are expected to be strings.
            uassert(40103,
                    str::stream() << "expected string as argument for " << argName
                                  << ", found: " << typeName(argument.type()),
                    argument.type() == BSONType::string);
        }

        if (argName == "from") {
            from = parseGraphLookupFromAndResolveNamespace(argument,
                                                           expCtx->getNamespaceString().dbName());
        } else if (argName == "as") {
            as = argument.String();
        } else if (argName == "connectFromField") {
            connectFromField = argument.String();
        } else if (argName == "connectToField") {
            connectToField = argument.String();
        } else if (argName == "depthField") {
            depthField = boost::optional<FieldPath>(FieldPath(argument.String()));
        } else {
            uasserted(40104,
                      str::stream()
                          << "Unknown argument to $graphLookup: " << argument.fieldName());
        }
    }

    const bool isMissingRequiredField = from.isEmpty() || as.empty() || !startWith ||
        connectFromField.empty() || connectToField.empty();

    uassert(40105,
            str::stream() << "$graphLookup requires 'from', 'as', 'startWith', 'connectFromField', "
                          << "and 'connectToField' to be specified.",
            !isMissingRequiredField);

    intrusive_ptr<DocumentSourceGraphLookUp> newSource(
        new DocumentSourceGraphLookUp(expCtx,
                                      GraphLookUpParams(std::move(from),
                                                        std::move(as),
                                                        std::move(connectFromField),
                                                        std::move(connectToField),
                                                        std::move(startWith),
                                                        additionalFilter,
                                                        depthField,
                                                        maxDepth),
                                      boost::none));

    return newSource;
}

boost::intrusive_ptr<DocumentSource> DocumentSourceGraphLookUp::clone(
    const boost::intrusive_ptr<ExpressionContext>& newExpCtx) const {
    return make_intrusive<DocumentSourceGraphLookUp>(*this, newExpCtx);
}

void DocumentSourceGraphLookUp::addInvolvedCollections(
    stdx::unordered_set<NamespaceString>* collectionNames) const {
    collectionNames->insert(_fromExpCtx->getNamespaceString());
    auto introspectionPipeline = Pipeline::parse(_fromPipeline, _fromExpCtx);
    for (auto&& stage : introspectionPipeline->getSources()) {
        stage->addInvolvedCollections(collectionNames);
    }
}

}  // namespace mongo
