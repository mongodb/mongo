// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"

#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(ListCatalog);

/**
 * Provides a document source interface to retrieve catalog metadata for collections and views.
 * Each document returned represents either:
 * - a single collection with its indexes; or
 * - a view instance.
 */
class DocumentSourceListCatalog final : public DocumentSource {
public:
    static constexpr std::string_view kStageName = "$listCatalog"sv;

    class LiteParsed final : public LiteParsedDocumentSourceDefault<LiteParsed> {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& spec,
                                                 const LiteParserOptions& options) {
            return std::make_unique<LiteParsed>(spec, nss);
        }

        LiteParsed(const BSONElement& spec, NamespaceString ns)
            : LiteParsedDocumentSourceDefault(spec), _ns(std::move(ns)) {}

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
            return stdx::unordered_set<NamespaceString>();
        }


        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const final;

        bool isInitialSource() const final {
            return true;
        }

        std::unique_ptr<StageParams> getStageParams() const final {
            return std::make_unique<ListCatalogStageParams>(_originalBson);
        }

    private:
        NamespaceString _ns;
    };

    // virtuals from DocumentSource
    std::string_view getSourceName() const final;
    Value serialize(const query_shape::SerializationOptions& opts = {}) const final;

    static const Id& id;

    Id getId() const override {
        return id;
    }

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kFirst,
                                     HostTypeRequirement::kTargetedShards,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kNotAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed);

        constraints.isIndependentOfAnyCollection =
            getExpCtx()->getNamespaceString().isCollectionlessAggregateNS();
        constraints.setConstraintsForNoInputSources();
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        return boost::none;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    static DocumentSourceContainer createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    DocumentSourceListCatalog(const boost::intrusive_ptr<ExpressionContext>& pExpCtx);
};

}  // namespace mongo
