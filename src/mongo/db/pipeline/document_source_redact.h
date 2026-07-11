// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/redact_processor.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/expression_dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/modules.h"

#include <memory>
#include <set>
#include <string_view>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

DEFINE_LITE_PARSED_STAGE_DEFAULT_DERIVED(Redact);

class [[MONGO_MOD_NEEDS_REPLACEMENT]] DocumentSourceRedact final : public DocumentSource {
public:
    static constexpr std::string_view kStageName = "$redact"sv;
    std::string_view getSourceName() const final;
    boost::intrusive_ptr<DocumentSource> optimize();

    static const Id& id;

    Id getId() const override {
        return id;
    }

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        StageConstraints constraints{StreamType::kStreaming,
                                     PositionRequirement::kNone,
                                     HostTypeRequirement::kNone,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kAllowed,
                                     TransactionRequirement::kAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed,
                                     ChangeStreamRequirement::kAllowlist};
        constraints.outputDependsOnSingleInput = true;
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        return boost::none;
    }

    /**
     * Attempts to duplicate the redact-safe portion of a subsequent $match before the $redact
     * stage.
     */
    DocumentSourceContainer::iterator optimizeAt(DocumentSourceContainer::iterator itr,
                                                 DocumentSourceContainer* container);

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;

    const std::shared_ptr<RedactProcessor>& getRedactProcessor() const {
        return _redactProcessor;
    }

    boost::intrusive_ptr<Expression> getExpression() {
        return _redactProcessor->getExpression();
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {
        expression::addVariableRefs(_redactProcessor->getExpression().get(), refs);
    }

private:
    DocumentSourceRedact(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                         const boost::intrusive_ptr<Expression>& previsit,
                         Variables::Id currentId);

    std::shared_ptr<RedactProcessor> _redactProcessor;
};

}  // namespace mongo
