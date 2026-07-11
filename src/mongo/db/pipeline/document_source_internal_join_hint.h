// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/optimizer/join/hint.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/modules.h"

#include <set>
#include <string_view>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

DEFINE_LITE_PARSED_STAGE_NO_TIMESERIES_DERIVED(InternalJoinHint);

/**
 * An internal stage available for testing. This allows us to specify a join order (or some
 * constraints on what the order/method/plan shape/enumeration strategy should be).
 *
 * See join_ordering::EnumerationStrategy for what is possible.
 */
class DocumentSourceInternalJoinHint final : public DocumentSource {
public:
    static constexpr std::string_view kStageName = "$_internalJoinHint"sv;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement, const boost::intrusive_ptr<ExpressionContext>&);

    DocumentSourceInternalJoinHint(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                   join_ordering::EnumerationStrategy&& strategy)
        : DocumentSource(kStageName, expCtx), _joinStrategy(std::move(strategy)) {}

    std::string_view getSourceName() const final {
        return kStageName;
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        auto constraints = StageConstraints{StreamType::kBlocking,
                                            PositionRequirement::kFirst,
                                            HostTypeRequirement::kReceivingHostOnly,
                                            DiskUseRequirement::kNoDiskUse,
                                            FacetRequirement::kNotAllowed,
                                            TransactionRequirement::kNotAllowed,
                                            LookupRequirement::kNotAllowed,
                                            UnionRequirement::kNotAllowed,
                                            ChangeStreamRequirement::kDenylist};
        constraints.canRunOnTimeseries = false;
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        return boost::none;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    join_ordering::EnumerationStrategy getStrategy() const {
        return _joinStrategy;
    }

private:
    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;

    join_ordering::EnumerationStrategy _joinStrategy;
};

}  // namespace mongo
