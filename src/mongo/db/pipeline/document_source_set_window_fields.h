/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_set_window_fields_gen.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/pipeline/window_function/window_bounds.h"
#include "mongo/db/pipeline/window_function/window_function_exec.h"
#include "mongo/db/pipeline/window_function/window_function_statement.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/compiler/dependency_analysis/expression_dependencies.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/string_map.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * $setWindowFields is an alias: it desugars to some combination of projection, sorting,
 * and $_internalSetWindowFields.
 */
namespace document_source_set_window_fields {
constexpr StringData kStageName = "$setWindowFields"_sd;

std::list<boost::intrusive_ptr<DocumentSource>> createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

std::list<boost::intrusive_ptr<DocumentSource>> create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::optional<boost::intrusive_ptr<Expression>> partitionBy,
    boost::optional<SortPattern> sortBy,
    std::vector<WindowFunctionStatement> outputFields,
    SbeCompatibility sbeCompatibility);

}  // namespace document_source_set_window_fields

class DocumentSourceInternalSetWindowFields final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$_internalSetWindowFields"_sd;

    /**
     * Parses 'elem' into a $setWindowFields stage, or throws a AssertionException if 'elem' was an
     * invalid specification.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    DocumentSourceInternalSetWindowFields(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        boost::optional<boost::intrusive_ptr<Expression>> partitionBy,
        boost::optional<SortPattern> sortBy,
        std::vector<WindowFunctionStatement> outputFields,
        SbeCompatibility sbeCompatibility)
        : DocumentSource(kStageName, expCtx),
          _partitionBy(partitionBy),
          _sortBy(std::move(sortBy)),
          _outputFields(std::move(outputFields)),
          _sbeCompatibility(sbeCompatibility) {};

    GetModPathsReturn getModifiedPaths() const final {
        OrderedPathSet outputPaths;
        for (auto&& outputField : _outputFields) {
            outputPaths.insert(outputField.fieldName);
        }

        return {DocumentSource::GetModPathsReturn::Type::kFiniteSet, std::move(outputPaths), {}};
    }

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        return StageConstraints(StreamType::kBlocking,
                                PositionRequirement::kNone,
                                HostTypeRequirement::kNone,
                                DiskUseRequirement::kWritesTmpData,
                                FacetRequirement::kAllowed,
                                TransactionRequirement::kAllowed,
                                LookupRequirement::kAllowed,
                                UnionRequirement::kAllowed);
    }

    const char* getSourceName() const override {
        return kStageName.data();
    };

    static const Id& id;

    Id getId() const override {
        return id;
    }

    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        if (_sortBy) {
            _sortBy->addDependencies(deps);
        }

        if (_partitionBy && (*_partitionBy)) {
            expression::addDependencies((*_partitionBy).get(), deps);
        }

        for (auto&& outputField : _outputFields) {
            outputField.addDependencies(deps);
        }

        return DepsTracker::State::SEE_NEXT;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {
        if (_partitionBy && (*_partitionBy)) {
            expression::addVariableRefs((*_partitionBy).get(), refs);
        }

        for (auto&& outputField : _outputFields) {
            outputField.addVariableRefs(refs);
        }
    }

    DocumentSourceContainer::iterator doOptimizeAt(DocumentSourceContainer::iterator itr,
                                                   DocumentSourceContainer* container) final;

    boost::optional<DistributedPlanLogic> distributedPlanLogic() override {
        // Force to run on the merging half for now.
        return DistributedPlanLogic{nullptr, this, boost::none};
    }

    boost::intrusive_ptr<DocumentSource> optimize() final;

    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    SbeCompatibility sbeCompatibility() const {
        return _sbeCompatibility;
    }

    boost::optional<boost::intrusive_ptr<Expression>> getPartitionBy() const {
        return _partitionBy;
    }

    boost::optional<SortPattern> getSortBy() const {
        return _sortBy;
    }

    const std::vector<WindowFunctionStatement>& getOutputFields() const {
        return _outputFields;
    }


private:
    boost::optional<boost::intrusive_ptr<Expression>> _partitionBy;
    boost::optional<SortPattern> _sortBy;
    std::vector<WindowFunctionStatement> _outputFields;

    SbeCompatibility _sbeCompatibility = SbeCompatibility::noRequirements;
};

}  // namespace mongo
