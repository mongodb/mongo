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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/change_stream_event_transform.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"

#include <memory>
#include <set>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

class DocumentSourceChangeStreamTransform final : public DocumentSourceInternalChangeStreamStage {
public:
    static constexpr StringData kStageName = "$_internalChangeStreamTransform"_sd;

    /**
     * Creates a new transformation stage from the given specification.
     */
    static boost::intrusive_ptr<DocumentSourceChangeStreamTransform> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const DocumentSourceChangeStreamSpec& spec);

    static boost::intrusive_ptr<DocumentSourceChangeStreamTransform> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    Document applyTransformation(const Document& input);

    DepsTracker::State getDependencies(DepsTracker* deps) const final;

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    DocumentSource::GetModPathsReturn getModifiedPaths() const final;

    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    /**
     * This function should never be called, since this DocumentSource has its own serialize method.
     */
    Value doSerialize(const SerializationOptions& opts) const final {
        MONGO_UNREACHABLE;
    }

    StageConstraints constraints(PipelineSplitState pipeState) const final;

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    const char* getSourceName() const final {
        return kStageName.data();
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

private:
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceChangeStreamTransformToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSource);

    // This constructor is private, callers should use the 'create()' method above.
    DocumentSourceChangeStreamTransform(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                        DocumentSourceChangeStreamSpec spec);

    DocumentSourceChangeStreamSpec _changeStreamSpec;

    // TODO SERVER-105521: Check if we can change from 'std::shared_ptr' to 'std::unique_ptr', and
    // std::move the transformer to the Stage.
    std::shared_ptr<ChangeStreamEventTransformer> _transformer;

    // Set to true if this transformation stage can be run on the collectionless namespace.
    bool _isIndependentOfAnyCollection;
};

}  // namespace mongo
