/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"

#include <memory>
#include <set>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * This stage keeps track of applyOps oplog entries that represent transactions and "unwinds" them
 * whenever an oplog entry commits a transaction. When the stage observes an applyOps or commit
 * command that commits a transaction, it emits one document for each operation in the transaction
 * that matches the namespace filter. The applyOps entries themselves are removed from the stage's
 * output, but all other entries pass through unmodified. Note that the namespace filter applies
 * only to unwound transaction operations, not to any other entries.
 */
class DocumentSourceChangeStreamUnwindTransaction final
    : public DocumentSourceInternalChangeStreamStage {
public:
    static constexpr StringData kStageName = "$_internalChangeStreamUnwindTransaction"_sd;

    static boost::intrusive_ptr<DocumentSourceChangeStreamUnwindTransaction> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx);

    static boost::intrusive_ptr<DocumentSourceChangeStreamUnwindTransaction> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    DepsTracker::State getDependencies(DepsTracker* deps) const final;

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    DocumentSource::GetModPathsReturn getModifiedPaths() const final;

    Value doSerialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    StageConstraints constraints(PipelineSplitState pipeState) const final;

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    const char* getSourceName() const override {
        return DocumentSourceChangeStreamUnwindTransaction::kStageName.data();
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

private:
    friend boost::intrusive_ptr<exec::agg::Stage>
    documentSourceChangeStreamUnwindTransactionToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSource);

    DocumentSourceContainer::iterator doOptimizeAt(DocumentSourceContainer::iterator itr,
                                                   DocumentSourceContainer* container) final;

    DocumentSourceChangeStreamUnwindTransaction(
        BSONObj filter, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Resets the transaction entry filter saved in the '_filter' and '_expression' fields.
     */
    void rebuild(BSONObj filter);

    // All transaction entries are filtered through this expression. This extra filtering step is
    // necessary because a transaction can contain a mix of operations from different namespaces. A
    // change stream needs to filter by namespace to ensure it does not return operations outside
    // the namespace it watches. As an optimization, this filter can also have predicates from
    // user-specified $match stages, allowing for early filtering of events that we know would be
    // filtered later in the pipeline.
    BSONObj _filter;
    std::shared_ptr<MatchExpression> _expression;
};

}  // namespace mongo
