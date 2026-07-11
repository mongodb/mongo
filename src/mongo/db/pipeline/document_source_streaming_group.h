// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_group_base.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

DEFINE_LITE_PARSED_STAGE_DEFAULT_DERIVED(StreamingGroup);

/**
 * This class represents streaming group implementation that can only be used when at least one of
 * _id fields is monotonic. It stores and output groups in batches. All groups in the batch has
 * the same value of monotonic id fields.
 *
 * For example, if the inputs are sorted by "x", we could use a batched streaming algorithm to
 * perform the grouping for {$group: {_id: {x: "$x", y: "$y"}}}.
 *
 * Groups are processes in batches. One batch corresponds to a set of groups when each monotonic
 * id field have the same value. Non-monotonic fields can have different values, so we still may
 * have multiple groups and even spill to disk, but we still consume significantly less memory
 * than general hash based group.
 * When a document with a different value in at least one group id field is encountered, it is
 * cached in '_firstDocumentOfNextBatch', current groups are finalized and returned in
 * subsequent getNext() called and when the current batch is depleted, memory is freed and the
 * process starts again.
 *
 * TODO SERVER-71437 Implement an optimization for a special case where all group fields are
 * monotonic
 * - we don't need any hashing in this case.
 */
class DocumentSourceStreamingGroup final : public DocumentSourceGroupBase {
public:
    static constexpr std::string_view kStageName{"$_internalStreamingGroup"};

    std::string_view getSourceName() const final;

    static const Id& id;

    Id getId() const override {
        return id;
    }

    /**
     * Convenience method for creating a new $_internalStreamingGroup stage. If maxMemoryUsageBytes
     * is boost::none, then it will actually use the value of
     * internalDocumentSourceGroupMaxMemoryBytes.
     */
    static boost::intrusive_ptr<DocumentSourceStreamingGroup> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const boost::intrusive_ptr<Expression>& groupByExpression,
        std::vector<size_t> monotonicExpressionIndexes,
        std::vector<AccumulationStatement> accumulationStatements,
        boost::optional<int64_t> maxMemoryUsageBytes = boost::none);

    /**
     * Parses 'elem' into a $_internalStreamingGroup stage, or throws a AssertionException if 'elem'
     * was an invalid specification.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);
    static boost::intrusive_ptr<DocumentSource> createFromBsonWithMaxMemoryUsage(
        BSONElement elem,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        boost::optional<int64_t> maxMemoryUsageBytes);

protected:
    bool isSpecFieldReserved(std::string_view fieldName) final;
    void serializeAdditionalFields(MutableDocument& out,
                                   const query_shape::SerializationOptions& opts = {}) const final;

private:
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceStreamingGroupToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSource);

    static constexpr std::string_view kMonotonicIdFieldsSpecField{"$monotonicIdFields"};

    explicit DocumentSourceStreamingGroup(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        boost::optional<int64_t> maxMemoryUsageBytes = boost::none);

    std::vector<size_t> _monotonicExpressionIndexes;
};

}  // namespace mongo
