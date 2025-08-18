/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/change_stream_constants.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"

#include <set>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * An internal stage used as part of the V2 change streams infrastructure, to listen for events that
 * describe topology changes to the cluster. This stage is responsible for opening and closing
 * remote cursors on these shards as needed.
 */
class DocumentSourceChangeStreamHandleTopologyChangeV2 final
    : public DocumentSourceInternalChangeStreamStage,
      public exec::agg::Stage {
public:
    static constexpr StringData kStageName =
        change_stream_constants::stage_names::kHandleTopologyChangeV2;

    static boost::intrusive_ptr<DocumentSourceChangeStreamHandleTopologyChangeV2> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Creates a new stage for V2 change stream readers which will establish a new cursor and add it
     * to the cursors being merged by 'mergeCursorsStage' whenever a new shard is detected by a
     * change stream.
     */
    static boost::intrusive_ptr<DocumentSourceChangeStreamHandleTopologyChangeV2> create(
        const boost::intrusive_ptr<ExpressionContext>&);

    const char* getSourceName() const final {
        return kStageName.data();
    }

    Value doSerialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    StageConstraints constraints(PipelineSplitState pipeState) const final;

    GetModPathsReturn getModifiedPaths() const final {
        // This stage neither modifies nor renames any field.
        return {GetModPathsReturn::Type::kFiniteSet, OrderedPathSet{}, {}};
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return DistributedPlanLogic{nullptr, this, change_stream_constants::kSortSpec};
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    static const Id& id;

    Id getId() const override {
        return id;
    }

private:
    DocumentSourceChangeStreamHandleTopologyChangeV2(
        const boost::intrusive_ptr<ExpressionContext>&);

    /**
     * This method currently returns all input documents as they are. The actual handling of
     * topology changes will be implemented in follow-up tickets.
     */
    GetNextResult doGetNext() final;
};
}  // namespace mongo
