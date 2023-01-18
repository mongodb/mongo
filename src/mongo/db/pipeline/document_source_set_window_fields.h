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

#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_set_window_fields_gen.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/memory_usage_tracker.h"
#include "mongo/db/pipeline/window_function/partition_iterator.h"
#include "mongo/db/pipeline/window_function/window_bounds.h"
#include "mongo/db/pipeline/window_function/window_function_exec.h"
#include "mongo/db/pipeline/window_function/window_function_expression.h"

namespace mongo {

class WindowFunctionExec;

struct WindowFunctionStatement {
    std::string fieldName;  // top-level fieldname, not a path
    boost::intrusive_ptr<window_function::Expression> expr;

    WindowFunctionStatement(std::string fieldName,
                            boost::intrusive_ptr<window_function::Expression> expr)
        : fieldName(std::move(fieldName)), expr(std::move(expr)) {}

    static WindowFunctionStatement parse(BSONElement elem,
                                         const boost::optional<SortPattern>& sortBy,
                                         ExpressionContext* expCtx);

    void addDependencies(DepsTracker* deps) const {
        if (expr) {
            expr->addDependencies(deps);
        }

        const FieldPath path(fieldName);

        // We do this because acting on "a.b" where a is an object also depends on "a" not being
        // changed (e.g. to a non-object).
        for (size_t i = 0; i < path.getPathLength() - 1; i++) {
            deps->fields.insert(path.getSubpath(i).toString());
        }
    }

    void serialize(MutableDocument& outputFields,
                   boost::optional<ExplainOptions::Verbosity> explain) const;
};

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
    const boost::optional<SortPattern>& sortBy,
    std::vector<WindowFunctionStatement> outputFields);

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
        const boost::optional<SortPattern>& sortBy,
        std::vector<WindowFunctionStatement> outputFields,
        size_t maxMemoryBytes)
        : DocumentSource(kStageName, expCtx),
          _partitionBy(partitionBy),
          _sortBy(std::move(sortBy)),
          _outputFields(std::move(outputFields)),
          _memoryTracker{expCtx->allowDiskUse, maxMemoryBytes},
          _iterator(expCtx.get(), pSource, &_memoryTracker, std::move(partitionBy), _sortBy){};

    GetModPathsReturn getModifiedPaths() const final {
        OrderedPathSet outputPaths;
        for (auto&& outputField : _outputFields) {
            outputPaths.insert(outputField.fieldName);
        }

        return {DocumentSource::GetModPathsReturn::Type::kFiniteSet, std::move(outputPaths), {}};
    }


    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        return StageConstraints(StreamType::kBlocking,
                                PositionRequirement::kNone,
                                HostTypeRequirement::kNone,
                                DiskUseRequirement::kWritesTmpData,
                                FacetRequirement::kAllowed,
                                TransactionRequirement::kAllowed,
                                LookupRequirement::kAllowed,
                                UnionRequirement::kAllowed);
    }

    const char* getSourceName() const {
        return kStageName.rawData();
    };

    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        if (_sortBy) {
            _sortBy->addDependencies(deps);
        }

        if (_partitionBy && (*_partitionBy)) {
            (*_partitionBy)->addDependencies(deps);
        }

        for (auto&& outputField : _outputFields) {
            outputField.addDependencies(deps);
        }

        return DepsTracker::State::SEE_NEXT;
    }

    Pipeline::SourceContainer::iterator doOptimizeAt(Pipeline::SourceContainer::iterator itr,
                                                     Pipeline::SourceContainer* container) final;

    boost::optional<DistributedPlanLogic> distributedPlanLogic() {
        // Force to run on the merging half for now.
        return DistributedPlanLogic{nullptr, this, boost::none};
    }

    boost::intrusive_ptr<DocumentSource> optimize() final;

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain) const;

    DocumentSource::GetNextResult doGetNext();

    void setSource(DocumentSource* source) final {
        pSource = source;
        _iterator.setSource(source);
    }

    bool usedDisk() final {
        return _iterator.usedDisk();
    };

private:
    void initialize();

    boost::optional<boost::intrusive_ptr<Expression>> _partitionBy;
    boost::optional<SortPattern> _sortBy;
    std::vector<WindowFunctionStatement> _outputFields;
    MemoryUsageTracker _memoryTracker;
    PartitionIterator _iterator;
    StringMap<std::unique_ptr<WindowFunctionExec>> _executableOutputs;
    bool _init = false;
    bool _eof = false;
};

}  // namespace mongo
