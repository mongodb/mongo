// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/collection_index_usage_tracker.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>
#include <utility>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(IndexStats);

/**
 * Provides a document source interface to retrieve index statistics for a given namespace.
 * Each document returned represents a single index and mongod instance.
 */
class DocumentSourceIndexStats final {
public:
    static constexpr std::string_view kStageName = "$indexStats"sv;

    class LiteParsed final : public LiteParsedDocumentSourceDefault<LiteParsed> {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& spec,
                                                 const LiteParserOptions& options) {
            return std::make_unique<LiteParsed>(spec, nss);
        }

        LiteParsed(const BSONElement& spec, NamespaceString nss)
            : LiteParsedDocumentSourceDefault(spec), _nss(std::move(nss)) {}

        bool isIndexStats() const final {
            return true;
        }

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
            return stdx::unordered_set<NamespaceString>();
        }

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const final {
            return {Privilege(ResourcePattern::forExactNamespace(_nss), ActionType::indexStats)};
        }

        bool isInitialSource() const final {
            return true;
        }

        std::unique_ptr<StageParams> getStageParams() const final {
            return std::make_unique<IndexStatsStageParams>(_originalBson);
        }

    private:
        const NamespaceString _nss;
    };

    /**
     * Returns the stage constraints used to override 'DocumentSourceQueue'.
     *
     * This stage must be executed on each and every shard. Trying to call
     * 'MongoProcessInterface::getIndexStats()' on a 'mongos' instance will result in
     * 'MONGO_UNREACHEABLE'.
     */
    static StageConstraints constraints() {
        StageConstraints constraints(DocumentSource::StreamType::kStreaming,
                                     DocumentSource::PositionRequirement::kFirst,
                                     DocumentSource::HostTypeRequirement::kTargetedShards,
                                     DocumentSource::DiskUseRequirement::kNoDiskUse,
                                     DocumentSource::FacetRequirement::kNotAllowed,
                                     DocumentSource::TransactionRequirement::kNotAllowed,
                                     DocumentSource::LookupRequirement::kAllowed,
                                     DocumentSource::UnionRequirement::kAllowed);
        constraints.setConstraintsForNoInputSources();
        return constraints;
    }

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);
};

}  // namespace mongo
