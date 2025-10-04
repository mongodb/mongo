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
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"

#include <set>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * This stage detects change stream topology changes in the form of 'kNewShardDetectedOpType' events
 * and forwards them directly to the executor via an exception. Using an exception bypasses the rest
 * of the pipeline, ensuring that the event cannot be filtered out or modified by user-specified
 * stages and that it will ultimately be available to the mongoS.
 *
 * The mongoS must see all 'kNewShardDetectedOpType' events, so that it knows when it needs to open
 * cursors on newly active shards. These events are generated when a chunk is migrated to a shard
 * that previously may not have held any data for the collection being watched, and they contain the
 * information necessary for the mongoS to include the new shard in the merged change stream.
 */
class DocumentSourceChangeStreamCheckTopologyChange final
    : public DocumentSourceInternalChangeStreamStage {
public:
    static constexpr StringData kStageName = "$_internalChangeStreamCheckTopologyChange"_sd;

    static boost::intrusive_ptr<DocumentSourceChangeStreamCheckTopologyChange> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    static boost::intrusive_ptr<DocumentSourceChangeStreamCheckTopologyChange> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourceChangeStreamCheckTopologyChange(expCtx);
    }

    const char* getSourceName() const final {
        return kStageName.data();
    }

    StageConstraints constraints(PipelineSplitState pipeState) const final;

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    Value doSerialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    static const Id& id;

    Id getId() const override {
        return id;
    }

private:
    DocumentSourceChangeStreamCheckTopologyChange(
        const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceInternalChangeStreamStage(kStageName, expCtx) {}
};

}  // namespace mongo
