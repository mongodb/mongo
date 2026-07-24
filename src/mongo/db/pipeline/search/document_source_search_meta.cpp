// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/search/document_source_search_meta.h"

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/search/lite_parsed_search.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/search/mongot_cursor.h"
#include "mongo/db/query/search/search_index_view_validation.h"
#include "mongo/db/query/search/search_task_executors.h"

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

using boost::intrusive_ptr;
using std::list;

namespace {
/**
 * Helper to go through the list and clone each stage. Very similar to Pipeline::clone, but doesn't
 * necessitate use of the Pipeline type, which has auto-dispose behaviors which can cause problems.
 */
auto cloneEachOne(std::list<boost::intrusive_ptr<DocumentSource>> stages, const auto& expCtx) {
    std::list<boost::intrusive_ptr<DocumentSource>> cloned;
    for (const auto& stage : stages) {
        cloned.push_back(stage->clone(expCtx));
    }
    return cloned;
}
}  // namespace

// Register the legacy parser as a fallback. This parser will be used when
// featureFlagSearchExtension is disabled or when the search extension has not been
// loaded. Errors if the router sent the flag as true but the extension is not loaded.
REGISTER_LITE_PARSED_DOCUMENT_SOURCE_FALLBACK(
    searchMeta,
    [](const NamespaceString& nss,
       const BSONElement& spec,
       const LiteParserOptions& options) -> std::unique_ptr<LiteParsedDocumentSource> {
        tassert(12230701,
                "Cannot invoke fallback $searchMeta parser: featureFlagSearchExtension=true "
                "requires extension to be loaded",
                !search_helpers::isExtensionFlagEnabledByRouter(
                    options, feature_flags::gFeatureFlagSearchExtension));
        return SearchMetaLiteParsed::parse(nss, spec, options);
    },
    AllowedWithApiStrict::kNeverInVersion1,
    &feature_flags::gFeatureFlagSearchExtension);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(searchMeta,
                                                   DocumentSourceSearchMeta,
                                                   SearchMetaStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(searchMeta, DocumentSourceSearchMeta::id);

boost::optional<DocumentSource::DistributedPlanLogic>
DocumentSourceSearchMeta::distributedPlanLogic(const DistributedPlanContext* ctx) {
    DistributedPlanLogic logic;
    logic.shardsStage = this;
    tassert(6448011, "Expected merging pipeline to be set already", _mergingPipeline);
    // Please note that it's important for the merging stages to be copied so that we don't share a
    // pointer to them. If we share a pointer, it can lead to a bug where this $searchMeta stage is
    // serialized and sent to a remote shard, which causes "_mergingPipeline" to go out of scope and
    // dispose() each stage. That screws up the other pointers to these stages who now have disposed
    // DocumentSources which are expected to immediately return EOF.
    logic.mergingStages = cloneEachOne(_mergingPipeline->getSources(), getExpCtx());
    return logic;
}

namespace {
InternalSearchMongotRemoteSpec prepareInternalSearchMetaMongotSpec(
    const BSONObj& spec, const intrusive_ptr<ExpressionContext>& expCtx) {
    if (spec.hasField(InternalSearchMongotRemoteSpec::kMongotQueryFieldName)) {
        // The existence of this field name indicates that this spec was already serialized from a
        // mongos process. Parse out of the IDL spec format, rather than just expecting only the
        // mongot query (as a user would provide).
        auto params = InternalSearchMongotRemoteSpec::parseOwned(
            spec.getOwned(), IDLParserContext(DocumentSourceSearchMeta::kStageName));
        LOGV2_DEBUG(8569405,
                    4,
                    "Parsing as $internalSearchMongotRemote",
                    "params"_attr = redact(params.toBSON()));

        // Get the view from the expCtx if it doesn't already exist on the spec. getViewFromExpCtx
        // will return boost::none if no view exists there either.
        if (!params.getView()) {
            params.setView(search_helpers::getViewFromExpCtx(expCtx));
        }

        if (auto view = params.getView()) {
            search_helpers::validateMongotIndexedViewsFF(expCtx, view->getEffectivePipeline());
            search_index_view_validation::validate(*view);
        }

        return params;
    }

    // See note in DocumentSourceSearch::createFromBson() about making sure mongotQuery is owned
    // within the mongot remote spec.
    InternalSearchMongotRemoteSpec internalSpec(spec.getOwned());

    if (expCtx->getIsParsingViewDefinition()) {
        // $searchMeta is possible to be parsed from the user visible stage.  In this case, just
        // return the mongot query itself parsed into IDL.
        return internalSpec;
    }

    // If 'searchReturnEofImmediately' is set, we return this stage as is because we don't expect to
    // return any results. More precisely, we wish to avoid calling 'planShardedSearch' when no
    // mongot is set up.
    if (expCtx->getMongoProcessInterface()->isExpectedToExecuteQueries() &&
        expCtx->getMongoProcessInterface()->inShardedEnvironment(expCtx->getOperationContext()) &&
        !MONGO_unlikely(searchReturnEofImmediately.shouldFail())) {
        // This query is executing in a sharded environment. We need to consult a mongot to
        // construct such a merging pipeline for us to use later. Send a planShardedSearch command
        // to mongot to get the relevant planning information, including the metadata merging
        // pipeline and the optional merge sort spec.
        search_helpers::planShardedSearch(expCtx, &internalSpec);
    } else {
        // This is an unsharded environment or there is no mongot. If the case is the former, this
        // is only called from user pipelines during desugaring of $search/$searchMeta, so the
        // `specObj` should be the search query itself. If 'searchReturnEofImmediately' is set, we
        // return this stage as is because we don't expect to return any results. More precisely, we
        // wish to avoid calling 'planShardedSearch' when no mongot is set up.
    }

    return internalSpec;
}
}  // namespace

Value DocumentSourceSearchMeta::serialize(const query_shape::SerializationOptions& opts) const {
    // For query stats, serialize the mongot query as a single anonymized object.
    if (opts.isSerializingForQueryStats()) {
        return Value(Document{{getSourceName(), opts.serializeLiteral(getSearchQuery())}});
    }
    return DocumentSourceInternalSearchMongotRemote::serialize(opts);
}

std::list<intrusive_ptr<DocumentSource>> DocumentSourceSearchMeta::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {
    mongot_cursor::throwIfNotRunningWithMongotHostConfigured(expCtx);

    uassert(ErrorCodes::FailedToParse,
            str::stream() << "$searchMeta value must be an object. Found: "
                          << typeName(elem.type()),
            elem.type() == BSONType::object);

    auto specObj = elem.embeddedObject();
    auto executor = uassertStatusOK(
        executor::getMongotTaskExecutor(expCtx->getOperationContext()->getServiceContext()));
    auto internalRemoteSpec = prepareInternalSearchMetaMongotSpec(specObj, expCtx);

    return {
        make_intrusive<DocumentSourceSearchMeta>(std::move(internalRemoteSpec), expCtx, executor)};
}

}  // namespace mongo
