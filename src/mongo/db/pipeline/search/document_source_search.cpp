// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/search/document_source_search.h"

#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/pipeline/resolved_namespace.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/db/pipeline/search/document_source_internal_search_mongot_remote.h"
#include "mongo/db/pipeline/search/lite_parsed_search.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/pipeline/skip_and_limit.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/search/manage_search_index_request_gen.h"
#include "mongo/db/query/search/mongot_cursor.h"
#include "mongo/db/query/search/search_index_view_validation.h"
#include "mongo/platform/compiler.h"

#include <string_view>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

using boost::intrusive_ptr;
using std::list;

namespace {
/** Helper written in a particular redundant way to work around a GCC false-positive warning. */
std::string_view removePrefixWorkaround(std::string_view key, std::string_view pre) {
    MONGO_COMPILER_DIAGNOSTIC_PUSH
    MONGO_COMPILER_DIAGNOSTIC_IGNORED_TRANSITIONAL("-Warray-bounds")
    if (!key.starts_with(pre))
        return key;
    key.remove_prefix(pre.size());
    MONGO_COMPILER_DIAGNOSTIC_POP
    return key;
}

Rarely _samplerSearchBeta;

std::unique_ptr<LiteParsedDocumentSource> parseSearchBeta(const NamespaceString& nss,
                                                          const BSONElement& spec,
                                                          const LiteParserOptions& options) {
    if (_samplerSearchBeta.tick()) {
        LOGV2_WARNING(12165200, "$searchBeta is deprecated. Use $search instead.");
    }

    // $searchBeta is a deprecated alias for $search. Re-dispatch through the $search parser.
    BSONObjBuilder searchBuilder;
    searchBuilder.appendAs(spec, DocumentSourceSearch::kStageName);
    auto liteParsed = LiteParsedDocumentSource::parse(nss, searchBuilder.obj(), options);
    liteParsed->makeOwned();
    return liteParsed;
}
}  // namespace

// Register the legacy parser as a fallback. This parser will be used when
// featureFlagSearchExtension is disabled or when the search extension has not been
// loaded. Errors if the router sent the flag as true but the extension is not loaded.
REGISTER_LITE_PARSED_DOCUMENT_SOURCE_FALLBACK(
    search,
    [](const NamespaceString& nss,
       const BSONElement& spec,
       const LiteParserOptions& options) -> std::unique_ptr<LiteParsedDocumentSource> {
        tassert(12230700,
                "Cannot invoke fallback $search parser: featureFlagSearchExtension=true "
                "requires extension to be loaded",
                !search_helpers::isExtensionFlagEnabledByRouter(
                    options, feature_flags::gFeatureFlagSearchExtension));
        return SearchLiteParsed::parse(nss, spec, options);
    },
    AllowedWithApiStrict::kNeverInVersion1,
    &feature_flags::gFeatureFlagSearchExtension);

// $searchBeta is supported as a deprecated alias for $search for compatibility with applications
// that used search during its beta period.
REGISTER_LITE_PARSED_DOCUMENT_SOURCE(searchBeta,
                                     parseSearchBeta,
                                     AllowedWithApiStrict::kNeverInVersion1);

// This allocates SearchStageStageParams::id, which is shared by $search, $searchBeta,
// $searchMeta, and $vectorSearch.
REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(search, DocumentSourceSearch, SearchStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(search, DocumentSourceSearch::id);

std::string_view DocumentSourceSearch::getSourceName() const {
    return kStageName;
}

Value DocumentSourceSearch::serialize(const query_shape::SerializationOptions& opts) const {
    // If we aren't serializing for query stats or explain, serialize the full spec.
    // If we are in a router, serialize the full spec.
    // Otherwise, just serialize the mongotQuery.
    if ((!opts.isSerializingForQueryStats() && !opts.isSerializingForExplain()) ||
        getExpCtx()->getInRouter()) {
        return Value(Document{{getSourceName(), _spec.toBSON()}});
    }
    return Value(DOC(getSourceName() << opts.serializeLiteral(_spec.getMongotQuery())));
}

intrusive_ptr<DocumentSource> DocumentSourceSearch::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {
    mongot_cursor::throwIfNotRunningWithMongotHostConfigured(expCtx);

    uassert(ErrorCodes::FailedToParse,
            str::stream() << "$search value must be an object. Found: " << typeName(elem.type()),
            elem.type() == BSONType::object);
    auto specObj = elem.embeddedObject();

    search_helpers::validateViewNotSetByUser(expCtx, specObj);

    // If kMongotQueryFieldName is present, this is the case that we re-create the
    // DocumentSource from a serialized DocumentSourceSearch that was originally parsed on a
    // router.
    // We need to make sure that the mongotQuery BSONObj in the InternalSearchMongotRemoteSpec is
    // owned so that it persists safely to GetMores. Since the IDL type is object_owned, using the
    // parseOwned() function will make sure it's owned. Manually constructing the spec does _not_
    // ensure the owned is owned, which is why we call specObj.getOwned().
    InternalSearchMongotRemoteSpec spec =
        specObj.hasField(InternalSearchMongotRemoteSpec::kMongotQueryFieldName)
        ? InternalSearchMongotRemoteSpec::parseOwned(specObj.getOwned(),
                                                     IDLParserContext(kStageName))
        : InternalSearchMongotRemoteSpec(specObj.getOwned());

    // If there is no view on the spec, check the expression context for one. getViewFromExpCtx will
    // return boost::none if there is no view there either.
    if (!spec.getView()) {
        spec.setView(search_helpers::getViewFromExpCtx(expCtx));
    }

    if (auto view = spec.getView()) {
        search_helpers::validateMongotIndexedViewsFF(expCtx, view->getEffectivePipeline());
        search_index_view_validation::validate(*view);
    }

    return make_intrusive<DocumentSourceSearch>(expCtx, std::move(spec));
}

std::list<intrusive_ptr<DocumentSource>> DocumentSourceSearch::desugar() {
    auto executor = uassertStatusOK(
        executor::getMongotTaskExecutor(getExpCtx()->getOperationContext()->getServiceContext()));
    std::list<intrusive_ptr<DocumentSource>> desugaredPipeline;
    // 'getBoolField' returns false if the field is not present.
    bool storedSource = _spec.getMongotQuery().getBoolField(mongot_cursor::kReturnStoredSourceArg);

    auto spec =
        InternalSearchMongotRemoteSpec::parseOwned(_spec.toBSON(), IDLParserContext(kStageName));
    // Pass the limit in when there is no idLookup stage, and use the limit for mongotDocsRequested.
    // TODO: SERVER-76591 Remove special limit in favor of regular sharded limit optimization.
    spec.setMongotDocsRequested(spec.getLimit());
    if (!storedSource) {
        spec.setLimit(boost::none);
    }
    // Remove mergingPipeline info since it is not useful for
    // DocumentSourceInternalSearchMongotRemote.
    spec.setMergingPipeline(boost::none);

    auto mongoTRemoteStage = make_intrusive<DocumentSourceInternalSearchMongotRemote>(
        std::move(spec), getExpCtx(), executor);
    desugaredPipeline.push_back(mongoTRemoteStage);

    search_helpers::promoteStoredSourceOrAddIdLookup(
        getExpCtx(),
        desugaredPipeline,
        storedSource,
        static_cast<boost::optional<long long>>(_spec.getLimit()),
        _spec.getView());

    return desugaredPipeline;
}

StageConstraints DocumentSourceSearch::constraints(PipelineSplitState pipeState) const {
    auto constraints = DocumentSourceInternalSearchMongotRemote::getSearchDefaultConstraints();
    return constraints;
}
bool checkRequiresSearchSequenceToken(DocumentSourceContainer::iterator itr,
                                      DocumentSourceContainer* container) {
    DepsTracker deps;
    while (itr != container->end()) {
        auto nextStage = itr->get();
        nextStage->getDependencies(&deps);
        ++itr;
    }
    return deps.getNeedsMetadata(DocumentMetadataFields::kSearchSequenceToken);
}

DocumentSourceContainer::iterator DocumentSourceSearch::optimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    // In the case where the query has an extractable limit, we send that limit to mongot as a guide
    // for the number of documents mongot should return (rather than the default batchsize).
    // Move past the current stage ($search).
    auto stageItr = std::next(itr);
    // Only attempt to get the limit or requiresSearchSequenceToken from the query if there are
    // further stages in the pipeline.
    if (stageItr != container->end()) {
        // Calculate the extracted limit without modifying the rest of the pipeline.
        boost::optional<long long> limit = getUserLimit(stageItr, container);
        if (limit.has_value()) {
            _spec.setLimit((int64_t)*limit);
        }
        if (!_spec.getRequiresSearchSequenceToken()) {
            _spec.setRequiresSearchSequenceToken(checkRequiresSearchSequenceToken(itr, container));
        }
    }

    // If this $search stage was parsed from a spec sent from a router, the current pipeline may be
    // missing a reference to $$SEARCH_META that is only in the merging pipeline, so we shouldn't
    // compute requiresSearchMetaCursor based on the current pipeline.
    if (!_spec.getMetadataMergeProtocolVersion().has_value()) {
        // Determine whether the pipeline references the $$SEARCH_META variable. We won't insert a
        // $setVariableFromSubPipeline stage until we split the pipeline (see
        // distributedPlanLogic()), but at that point we don't have access to the full pipeline to
        // know whether we need it.
        _spec.setRequiresSearchMetaCursor(
            std::any_of(std::next(itr), container->end(), [](const auto& itr) {
                return search_helpers::hasReferenceToSearchMeta(*itr);
            }));
    }

    return std::next(itr);
}

void DocumentSourceSearch::validateSortSpec(boost::optional<BSONObj> sortSpec) {
    if (sortSpec) {
        // Verify that sortSpec do not contain dots after '$searchSortValues', as we expect it
        // to only contain top-level fields (no nested objects).
        for (auto&& k : *sortSpec) {
            auto key = k.fieldNameStringData();
            key = removePrefixWorkaround(key, mongot_cursor::kSearchSortValuesFieldPrefix);
            tassert(7320404,
                    fmt::format("planShardedSearch returned sortSpec with key containing a dot: {}",
                                key),
                    key.find('.', 0) == std::string::npos);
        }
    }
}

boost::optional<DocumentSource::DistributedPlanLogic> DocumentSourceSearch::distributedPlanLogic(
    const DistributedPlanContext* ctx) {
    // If 'searchReturnEofImmediately' is set, we return this stage as is because we don't expect to
    // return any results. More precisely, we wish to avoid calling 'planShardedSearch' when no
    // mongot is set up.
    if (MONGO_unlikely(searchReturnEofImmediately.shouldFail())) {
        return DistributedPlanLogic{this, nullptr, boost::none};
    }
    if (!_spec.getMetadataMergeProtocolVersion().has_value()) {
        // Issue a planShardedSearch call to mongot if we have not yet done so, and validate the
        // sortSpec in response. Note that this is done for unsharded collections as well as sharded
        // ones because the two collection types are treated the same in the context of shard
        // targeting.
        search_helpers::planShardedSearch(getExpCtx(), &_spec);
        validateSortSpec(_spec.getSortSpec());
    }

    // Construct the DistributedPlanLogic for sharded planning based on the information returned
    // from mongot.
    DistributedPlanLogic logic;
    logic.shardsStage = this;
    if (_spec.getMergingPipeline() && _spec.getRequiresSearchMetaCursor()) {
        logic.mergingStages = {DocumentSourceSetVariableFromSubPipeline::create(
            getExpCtx(),
            pipeline_factory::makePipeline(
                *_spec.getMergingPipeline(), getExpCtx(), pipeline_factory::kOptionsMinimal),
            Variables::kSearchMetaId)};
    }

    logic.mergeSortPattern = _spec.getSortSpec().has_value() ? _spec.getSortSpec()->getOwned()
                                                             : mongot_cursor::kSortSpec;

    logic.needsSplit = false;
    logic.canMovePast = canMovePastDuringSplit;

    return logic;
}

DepsTracker::State DocumentSourceSearch::getDependencies(DepsTracker* deps) const {
    // This stage doesn't currently support tracking field dependencies since mongot is
    // responsible for determining what fields to return. We do need to track metadata
    // dependencies though, so downstream stages know they are allowed to access "searchScore"
    // metadata.
    // TODO SERVER-101100 Implement logic for dependency analysis.

    deps->setMetadataAvailable(DocumentMetadataFields::kSearchScore);
    if (hasScoreDetails()) {
        deps->setMetadataAvailable(DocumentMetadataFields::kSearchScoreDetails);
    }

    if (hasSearchRootDocumentId()) {
        deps->setMetadataAvailable(DocumentMetadataFields::kSearchRootDocumentId);
    }

    return DepsTracker::State::NOT_SUPPORTED;
}

bool DocumentSourceSearch::canMovePastDuringSplit(const DocumentSource& ds) {
    return search_helpers::canMovePastDuringSplit(ds);
}

}  // namespace mongo
