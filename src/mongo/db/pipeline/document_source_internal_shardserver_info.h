// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/pipeline_split_state.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/tenant_id.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"

#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <boost/none.hpp>
#include <boost/none_t.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(InternalShardServerInfo);

/**
 * An internal stage available for testing. Gets the host and shard name for every shard server in
 * the cluster.
 */
class DocumentSourceInternalShardServerInfo final : public DocumentSource {
public:
    class LiteParsed : public LiteParsedDocumentSourceDefault<LiteParsed> {
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
            return {Privilege(ResourcePattern::forClusterResource(boost::none),
                              ActionSet{ActionType::internal})};
        }

        std::unique_ptr<StageParams> getStageParams() const final {
            return std::make_unique<InternalShardServerInfoStageParams>(_originalBson);
        }
    };

    static constexpr std::string_view kStageName = "$_internalShardServerInfo"sv;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement, const boost::intrusive_ptr<ExpressionContext>&);

    static boost::intrusive_ptr<DocumentSource> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourceInternalShardServerInfo(expCtx);
    }

    std::string_view getSourceName() const final {
        return kStageName;
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        return boost::none;
    }

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        StageConstraints constraints{StreamType::kStreaming,
                                     PositionRequirement::kFirst,
                                     HostTypeRequirement::kAllShardHosts,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kNotAllowed,
                                     LookupRequirement::kNotAllowed,
                                     UnionRequirement::kAllowed};
        constraints.isIndependentOfAnyCollection = true;
        constraints.setConstraintsForNoInputSources();
        return constraints;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

private:
    DocumentSourceInternalShardServerInfo(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSource(kStageName, expCtx) {}

    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;
};

}  // namespace mongo
