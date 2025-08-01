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
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/exclusion_projection_executor.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/single_document_transformation_processor.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/transformer_interface.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <list>
#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * This class is for DocumentSources that take in and return one document at a time, in a 1:1
 * transformation. It should only be used via an alias that passes the transformation logic through
 * a ParsedSingleDocumentTransformation. It is not a registered DocumentSource, and it cannot be
 * created from BSON.
 */
class DocumentSourceSingleDocumentTransformation final : public DocumentSource {
public:
    DocumentSourceSingleDocumentTransformation(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
        std::unique_ptr<TransformerInterface> parsedTransform,
        StringData name,
        bool independentOfAnyCollection);

    // virtuals from DocumentSource
    const char* getSourceName() const final;

    static const Id& id;

    Id getId() const override {
        return id;
    }

    boost::intrusive_ptr<DocumentSource> optimize() final;
    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final;
    DepsTracker::State getDependencies(DepsTracker* deps) const final;
    void addVariableRefs(std::set<Variables::Id>* refs) const final;
    GetModPathsReturn getModifiedPaths() const final;
    StageConstraints constraints(PipelineSplitState pipeState) const final;

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    TransformerInterface::TransformerType getTransformerType() const {
        return _transformationProcessor->getTransformer().getType();
    }

    const auto& getTransformer() const {
        return _transformationProcessor->getTransformer();
    }
    auto& getTransformer() {
        return _transformationProcessor->getTransformer();
    }

    SingleDocumentTransformationProcessor* getTransformationProcessor() {
        return _transformationProcessor.get();
    }

    /**
     * Extract computed projection(s) depending on the 'oldName' argument if the transformation is
     * of type inclusion projection or computed projection. Extraction is not allowed if the name of
     * the projection is in the 'reservedNames' set. The function returns a pair of <BSONObj, bool>.
     * The BSONObj contains the computed projections in which the 'oldName' is substituted for the
     * 'newName'. The boolean flag is true if the original projection has become empty after the
     * extraction and can be deleted by the caller.
     *
     * For transformation of type inclusion projection the computed projections are replaced with a
     * projected field or an identity projection depending on their position in the order of
     * additional fields.
     * For transformation of type computed projection the extracted computed projections are
     * removed.
     *
     * The function has no effect for exclusion projections, or if there are no computed
     * projections, or the computed expression depends on other fields than the oldName.
     */
    std::pair<BSONObj, bool> extractComputedProjections(StringData oldName,
                                                        StringData newName,
                                                        const std::set<StringData>& reservedNames) {
        return _transformationProcessor->getTransformer().extractComputedProjections(
            oldName, newName, reservedNames);
    }

    /**
     * If this transformation is a project, removes and returns a BSONObj representing the part of
     * this project that depends only on 'oldName'. Also returns a bool indicating whether this
     * entire project is extracted. In the extracted $project, 'oldName' is renamed to 'newName'.
     * 'oldName' should not be dotted.
     */
    std::pair<BSONObj, bool> extractProjectOnFieldAndRename(StringData oldName,
                                                            StringData newName) {
        return _transformationProcessor->getTransformer().extractProjectOnFieldAndRename(oldName,
                                                                                         newName);
    }

protected:
    DocumentSourceContainer::iterator doOptimizeAt(DocumentSourceContainer::iterator itr,
                                                   DocumentSourceContainer* container) final;

private:
    friend boost::intrusive_ptr<exec::agg::Stage>
    documentSourceSingleDocumentTransformationToStageFn(
        const boost::intrusive_ptr<DocumentSource>&);

    DocumentSourceContainer::iterator maybeCoalesce(
        DocumentSourceContainer::iterator itr,
        DocumentSourceContainer* container,
        DocumentSourceSingleDocumentTransformation* nextSingleDocTransform);

    // TODO SERVER-105521: Check if we can change from 'std::shared_ptr' to 'std::unique_ptr'.
    std::shared_ptr<SingleDocumentTransformationProcessor> _transformationProcessor;

    projection_executor::ExclusionNode& getExclusionNode();

    // Specific name of the transformation.
    std::string _name;

    // Set to true if this transformation stage can be run on the collectionless namespace.
    bool _isIndependentOfAnyCollection;

    // TODO SERVER-105521: Check if we can remove this and use just the '_transformationProcessor'.
    // Cached stage options in case this DocumentSource is disposed before serialized (e.g. explain
    // with a sort which will auto-dispose of the pipeline).
    Document _cachedStageOptions;
};

}  // namespace mongo
