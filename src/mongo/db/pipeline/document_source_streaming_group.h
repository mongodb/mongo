/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <memory>
#include <utility>

#include "mongo/db/pipeline/document_source_group_base.h"

namespace mongo {

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
 * have multiple groups and even spill to disk, but we still consume significanty less memory
 * than general hash based group.
 * When a document with a different value in at least one group id field is encountered, it is
 * cached in '_firstDocumentOfNextBatch', current groups are finalized and returned in
 * subsequent getNext() called and when the current batch is depleted, memory is freeed and the
 * process starts again.
 *
 * TODO SERVER-71437 Implement an optimization for a special case where all group fields are
 * monotonic
 * - we don't need any hashing in this case.
 */
class DocumentSourceStreamingGroup final : public DocumentSourceGroupBase {
public:
    static constexpr StringData kStageName = "$_internalStreamingGroup"_sd;

    const char* getSourceName() const final;

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
        boost::optional<size_t> maxMemoryUsageBytes = boost::none);

    /**
     * Parses 'elem' into a $_internalStreamingGroup stage, or throws a AssertionException if 'elem'
     * was an invalid specification.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);
    static boost::intrusive_ptr<DocumentSource> createFromBsonWithMaxMemoryUsage(
        BSONElement elem,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        boost::optional<size_t> maxMemoryUsageBytes);

protected:
    GetNextResult doGetNext() final;

    bool isSpecFieldReserved(StringData fieldName) final;
    void serializeAdditionalFields(MutableDocument& out,
                                   boost::optional<ExplainOptions::Verbosity> explain) const final;

private:
    static constexpr StringData kMonotonicIdFieldsSpecField = "$monotonicIdFields"_sd;

    explicit DocumentSourceStreamingGroup(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        boost::optional<size_t> maxMemoryUsageBytes = boost::none);


    GetNextResult getNextDocument();

    GetNextResult readyNextBatch();
    /**
     * Readies next batch after all children are initialized. See readyNextBatch() for
     * more details.
     */
    GetNextResult readyNextBatchInner(GetNextResult input);

    bool isBatchFinished(const Value& id);

    template <typename IdValueGetter>
    bool checkForBatchEndAndUpdateLastIdValues(const IdValueGetter& idValueGetter);

    std::vector<size_t> _monotonicExpressionIndexes;
    std::vector<Value> _lastMonotonicIdFieldValues;

    boost::optional<Document> _firstDocumentOfNextBatch;

    bool _sourceDepleted;
};

}  // namespace mongo
