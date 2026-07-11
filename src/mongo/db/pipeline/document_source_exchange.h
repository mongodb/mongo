// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/exchange_stage.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/shard_role/resource_yielder.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <set>
#include <string_view>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

class DocumentSourceExchange final : public DocumentSource {
public:
    static constexpr std::string_view kStageName = "$_internalExchange"sv;

    /**
     * Create an Exchange consumer. 'resourceYielder' is so the exchange may temporarily yield
     * resources (such as the Session) while waiting for other threads to do
     * work. 'resourceYielder' may be nullptr if there are no resources which need to be given up
     * while waiting.
     */
    DocumentSourceExchange(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                           boost::intrusive_ptr<exec::agg::Exchange> exchange,
                           size_t consumerId,
                           const std::shared_ptr<ResourceYielder>& yielder);

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kNone,
                                     HostTypeRequirement::kNone,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kAllowed,
                                     TransactionRequirement::kAllowed,
                                     LookupRequirement::kNotAllowed,
                                     UnionRequirement::kNotAllowed);
        // Given that the exchange stage accumulates results from all workers, the overall logical
        // number of documents remains the same.
        constraints.preservesCardinality = true;
        constraints.outputDependsOnSingleInput = true;
        // Exchange consumers read from a buffer filled by the inner pipeline rather than a
        // preceding pSource.
        constraints.requiresInputDocSource = false;
        constraints.consumesLogicalCollectionData = false;
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        return boost::none;
    }

    std::string_view getSourceName() const final;

    static const Id& id;

    Id getId() const override {
        return id;
    }

    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;

    size_t getConsumers() const {
        return _exchange->getConsumers();
    }

    auto getExchange() const {
        return _exchange;
    }


    auto getConsumerId() const {
        return _consumerId;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {
        // Any correlation analysis should have happened before this stage was created.
        MONGO_UNREACHABLE;
    }

private:
    friend exec::agg::StagePtr documentSourceExchangeToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSource);

    boost::intrusive_ptr<exec::agg::Exchange> _exchange;
    const size_t _consumerId;

    // While waiting for another thread to make room in its buffer, we may want to yield certain
    // resources (such as the Session). Through this interface we can do that.
    std::shared_ptr<ResourceYielder> _resourceYielder;
};

}  // namespace mongo
