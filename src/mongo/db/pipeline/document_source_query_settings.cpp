// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_query_settings.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_internal_query_settings_debug_shape.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_query_settings_gen.h"
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <deque>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using namespace query_settings;

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(InternalListQuerySettings);

/**
 * '$_internalListQuerySettings' seeds the '$querySettings' desugar with the in-memory query shape
 * configurations. It is a thin wrapper around DocumentSourceQueue whose serialization is a constant
 * '{$_internalListQuerySettings: {}}'. That serialization is:
 *   - stable, so $querySettings's own query shape does not change with the number of settings, and
 *   - reparse-safe, so on a sharded cluster the configurations are re-read locally on whichever
 * node the pipeline is dispatched to (the config server, where the downstream $lookup runs). The
 *     query settings cluster parameter is present on every node, so reading it wherever the
 * pipeline lands is correct.
 */
class DocumentSourceInternalListQuerySettings final {
public:
    static constexpr std::string_view kStageName = "$_internalListQuerySettings"sv;

    class LiteParsed final : public LiteParsedDocumentSourceDefault<LiteParsed> {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& spec,
                                                 const LiteParserOptions& options) {
            return std::make_unique<LiteParsed>(spec);
        }

        LiteParsed(const BSONElement& spec) : LiteParsedDocumentSourceDefault(spec) {}

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
            return stdx::unordered_set<NamespaceString>();
        }

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const final {
            return {};
        }

        bool isInitialSource() const final {
            return true;
        }

        std::unique_ptr<StageParams> getStageParams() const final {
            return std::make_unique<InternalListQuerySettingsStageParams>(_originalBson);
        }
    };

    static StageConstraints constraints() {
        // 'kCollectionlessSourceRunOnceAnyNode' (rather than 'kReceivingHostOnly') lets the
        // pipeline be forwarded to the config server on a sharded cluster, where the downstream
        // $lookup into config.queryShapeRepresentativeQueries becomes a legal shard-role local
        // read.
        StageConstraints constraints{
            StageConstraints::StreamType::kStreaming,
            StageConstraints::PositionRequirement::kFirst,
            StageConstraints::HostTypeRequirement::kCollectionlessSourceRunOnceAnyNode,
            StageConstraints::DiskUseRequirement::kNoDiskUse,
            StageConstraints::FacetRequirement::kNotAllowed,
            StageConstraints::TransactionRequirement::kAllowed,
            StageConstraints::LookupRequirement::kAllowed,
            StageConstraints::UnionRequirement::kAllowed};
        constraints.isIndependentOfAnyCollection = true;
        constraints.setConstraintsForNoInputSources();
        return constraints;
    }

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        uassert(12915500,
                str::stream() << kStageName << " expects a nested empty object but found: " << elem,
                elem.type() == BSONType::object && elem.embeddedObject().isEmpty());

        // The read is deferred so it runs on whichever node ends up executing the stage.
        DocumentSourceQueue::DeferredQueue deferredConfigs{[expCtx]() {
            auto tenantId = expCtx->getNamespaceString().tenantId();
            auto* opCtx = expCtx->getOperationContext();
            auto configs = QuerySettingsService::get(opCtx)
                               .getAllQueryShapeConfigurations(tenantId)
                               .queryShapeConfigurations;
            std::deque<DocumentSource::GetNextResult> queue;
            for (auto&& config : configs) {
                queue.emplace_back(Document{config.toBSON()});
            }
            return queue;
        }};

        return make_intrusive<DocumentSourceQueue>(
            std::move(deferredConfigs),
            expCtx,
            /* stageNameOverride */ kStageName,
            /* serializeOverride */ Value(DOC(kStageName << Document())),
            /* constraintsOverride */ constraints());
    }
};

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(_internalListQuerySettings,
                                     DocumentSourceInternalListQuerySettings::LiteParsed::parse,
                                     AllowedWithApiStrict::kNeverInVersion1);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(_internalListQuerySettings,
                                                   DocumentSourceInternalListQuerySettings,
                                                   InternalListQuerySettingsStageParams);

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(querySettings,
                                     DocumentSourceQuerySettings::LiteParsed::parse,
                                     AllowedWithApiStrict::kNeverInVersion1);

REGISTER_DOCUMENT_SOURCE_CONTAINER_WITH_STAGE_PARAMS_DEFAULT(querySettings,
                                                             DocumentSourceQuerySettings,
                                                             QuerySettingsStageParams);

DocumentSourceContainer DocumentSourceQuerySettings::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(7746801,
            "$querySettings stage expects a document as argument",
            elem.type() == BSONType::object);

    // Resolve whether to include the debug query shape or not.
    bool showDebugQueryShape = DocumentSourceQuerySettingsSpec::parse(
                                   elem.embeddedObject(), IDLParserContext("$querySettings"))
                                   .getShowDebugQueryShape();

    const auto& repQueriesNss = NamespaceString::kQueryShapeRepresentativeQueriesNamespace;

    DocumentSourceContainer pipeline;

    // Seed the pipeline with the in-memory query shape configurations.
    const auto seedSpec = BSON(DocumentSourceInternalListQuerySettings::kStageName << BSONObj());
    pipeline.push_back(
        DocumentSourceInternalListQuerySettings::createFromBson(seedSpec.firstElement(), expCtx));

    // Join the matching representative query for each configuration into
    // '__backfilledRepresentativeQuery', distinct from 'representativeQuery' so the join does not
    // clobber that field: before FCV 8.3, representative queries live embedded in the
    // configuration alongside its settings; from 8.3 onward (once 'featureFlagPQSBackfill' is
    // enabled) they are backfilled into the dedicated collection instead, and on downgrade below
    // 8.3 they are moved back and the collection is dropped. The dedicated collection lives on the
    // config server; $lookup merges there for us.
    pipeline.push_back(DocumentSourceLookUp::createFromBson(
        BSON("$lookup" << BSON("from" << BSON("db" << repQueriesNss.dbName().toStringForResourceId()
                                                   << "coll" << repQueriesNss.coll())
                                      << "localField" << "queryShapeHash" << "foreignField"
                                      << "_id" << "as" << "__backfilledRepresentativeQuery"))
            .firstElement(),
        expCtx));

    // Flatten the 0-or-1 element $lookup array, keeping configurations that have no matching
    // backfilled representative query ('includeNullIfEmptyOrMissing').
    pipeline.push_back(DocumentSourceUnwind::create(expCtx,
                                                    "__backfilledRepresentativeQuery",
                                                    true /* includeNullIfEmptyOrMissing */,
                                                    boost::none));

    // Prefer the backfilled representative query. When there was no match, fall back to the
    // pre-8.3 representative query already embedded in the configuration; otherwise the field is
    // omitted entirely. Drop the scratch join field in the same stage.
    pipeline.push_back(DocumentSourceAddFields::create(
        BSON("representativeQuery"
             << BSON("$ifNull" << BSON_ARRAY("$__backfilledRepresentativeQuery.representativeQuery"
                                             << "$representativeQuery"))
             << "__backfilledRepresentativeQuery" << "$$REMOVE"),
        expCtx));

    // Optionally append the debug query shape computation.
    if (showDebugQueryShape) {
        pipeline.push_back(make_intrusive<DocumentSourceInternalQuerySettingsDebugShape>(expCtx));
    }

    return pipeline;
}
}  // namespace mongo
