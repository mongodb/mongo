/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <boost/intrusive_ptr.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <cstddef>
#include <set>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/tee_buffer.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo {

class Document;
class ExpressionContext;
class Value;

/**
 * This stage acts as a proxy between a pipeline within a $facet stage and the buffer of incoming
 * documents held in a TeeBuffer stage. It will simply open an iterator on the TeeBuffer stage, and
 * answer calls to getNext() by advancing said iterator.
 */
class DocumentSourceTeeConsumer : public DocumentSource {
public:
    static boost::intrusive_ptr<DocumentSourceTeeConsumer> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        size_t facetId,
        const boost::intrusive_ptr<TeeBuffer>& bufferSource,
        StringData stageName);

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        return {StreamType::kStreaming,
                PositionRequirement::kNone,
                HostTypeRequirement::kNone,
                DiskUseRequirement::kNoDiskUse,
                FacetRequirement::kAllowed,
                TransactionRequirement::kAllowed,
                LookupRequirement::kAllowed,
                UnionRequirement::kAllowed};
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    /**
     * Returns SEE_NEXT, since it requires no fields, and changes nothing about the documents.
     */
    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        return DepsTracker::State::SEE_NEXT;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    const char* getSourceName() const override;

    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final override;

protected:
    GetNextResult doGetNext() final;
    void doDispose() final;

private:
    DocumentSourceTeeConsumer(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                              size_t facetId,
                              const boost::intrusive_ptr<TeeBuffer>& bufferSource,
                              StringData stageName);

    size_t _facetId;
    boost::intrusive_ptr<TeeBuffer> _bufferSource;

    // Specific name of the tee consumer.
    std::string _stageName;
};
}  // namespace mongo
