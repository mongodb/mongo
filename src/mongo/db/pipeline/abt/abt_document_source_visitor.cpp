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

#include "mongo/db/pipeline/abt/abt_document_source_visitor.h"
#include "mongo/db/exec/add_fields_projection_executor.h"
#include "mongo/db/exec/exclusion_projection_executor.h"
#include "mongo/db/exec/inclusion_projection_executor.h"
#include "mongo/db/pipeline/abt/agg_expression_visitor.h"
#include "mongo/db/pipeline/abt/field_map_builder.h"
#include "mongo/db/pipeline/abt/match_expression_visitor.h"
#include "mongo/db/pipeline/abt/utils.h"
#include "mongo/db/pipeline/document_source_bucket_auto.h"
#include "mongo/db/pipeline/document_source_coll_stats.h"
#include "mongo/db/pipeline/document_source_current_op.h"
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/document_source_exchange.h"
#include "mongo/db/pipeline/document_source_facet.h"
#include "mongo/db/pipeline/document_source_geo_near.h"
#include "mongo/db/pipeline/document_source_geo_near_cursor.h"
#include "mongo/db/pipeline/document_source_graph_lookup.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_index_stats.h"
#include "mongo/db/pipeline/document_source_internal_inhibit_optimization.h"
#include "mongo/db/pipeline/document_source_internal_shard_filter.h"
#include "mongo/db/pipeline/document_source_internal_split_pipeline.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_list_cached_and_active_users.h"
#include "mongo/db/pipeline/document_source_list_local_sessions.h"
#include "mongo/db/pipeline/document_source_list_sessions.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_merge.h"
#include "mongo/db/pipeline/document_source_operation_metrics.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/db/pipeline/document_source_plan_cache_stats.h"
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/db/pipeline/document_source_redact.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/document_source_sample.h"
#include "mongo/db/pipeline/document_source_sample_from_random_cursor.h"
#include "mongo/db/pipeline/document_source_sequential_document_cache.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_source_tee_consumer.h"
#include "mongo/db/pipeline/document_source_union_with.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/visitors/document_source_walker.h"
#include "mongo/db/pipeline/visitors/transformer_interface_walker.h"
#include "mongo/s/query/document_source_merge_cursors.h"
#include "mongo/util/string_map.h"

namespace mongo::optimizer {

class DSAlgebrizerContext {
public:
    struct NodeWithRootProjection {
        NodeWithRootProjection(ProjectionName rootProjection, ABT node)
            : _rootProjection(std::move(rootProjection)), _node(std::move(node)) {}

        ProjectionName _rootProjection;
        ABT _node;
    };

    DSAlgebrizerContext(PrefixId& prefixId, NodeWithRootProjection node)
        : _node(std::move(node)), _scanProjName(_node._rootProjection), _prefixId(prefixId) {
        assertNodeSort(_node._node);
    }

    template <typename T, typename... Args>
    inline auto setNode(ProjectionName rootProjection, Args&&... args) {
        setNode(std::move(rootProjection), std::move(ABT::make<T>(std::forward<Args>(args)...)));
    }

    void setNode(ProjectionName rootProjection, ABT node) {
        assertNodeSort(node);
        _node._node = std::move(node);
        _node._rootProjection = std::move(rootProjection);
    }

    NodeWithRootProjection& getNode() {
        return _node;
    }

    std::string getNextId(const std::string& key) {
        return _prefixId.getNextId(key);
    }

    PrefixId& getPrefixId() {
        return _prefixId;
    }

    const ProjectionName& getScanProjName() const {
        return _scanProjName;
    }

private:
    NodeWithRootProjection _node;
    ProjectionName _scanProjName;

    // We don't own this.
    PrefixId& _prefixId;
};

class ABTTransformerVisitor : public TransformerInterfaceConstVisitor {
public:
    ABTTransformerVisitor(DSAlgebrizerContext& ctx, FieldMapBuilder& builder)
        : _ctx(ctx), _builder(builder) {}

    void visit(const projection_executor::AddFieldsProjectionExecutor* transformer) override {
        visitInclusionNode(transformer->getRoot(), true /*isAddingFields*/);
    }

    void visit(const projection_executor::ExclusionProjectionExecutor* transformer) override {
        visitExclusionNode(*transformer->getRoot());
    }

    void visit(const projection_executor::InclusionProjectionExecutor* transformer) override {
        visitInclusionNode(*transformer->getRoot(), false /*isAddingFields*/);
    }

    void visit(const GroupFromFirstDocumentTransformation* transformer) override {
        // TODO: Is this internal-only?
        unsupportedTransformer(transformer);
    }

    void visit(const ReplaceRootTransformation* transformer) override {
        auto entry = _ctx.getNode();
        const std::string& projName = _ctx.getNextId("newRoot");
        ABT expr = generateAggExpression(
            transformer->getExpression().get(), entry._rootProjection, projName);

        _ctx.setNode<EvaluationNode>(projName, projName, std::move(expr), std::move(entry._node));
    }

    void generateCombinedProjection() const {
        auto result = _builder.generateABT();
        if (!result) {
            return;
        }

        auto entry = _ctx.getNode();
        const ProjectionName projName = _ctx.getNextId("combinedProjection");
        _ctx.setNode<EvaluationNode>(
            projName, projName, std::move(*result), std::move(entry._node));
    }

private:
    void unsupportedTransformer(const TransformerInterface* transformer) const {
        uasserted(ErrorCodes::InternalErrorNotSupported,
                  str::stream() << "Transformer is not supported (code: "
                                << static_cast<int>(transformer->getType()) << ")");
    }

    void processProjectedPaths(const projection_executor::InclusionNode& node) {
        std::set<std::string> preservedPaths;
        node.reportProjectedPaths(&preservedPaths);

        for (const std::string& preservedPathStr : preservedPaths) {
            _builder.integrateFieldPath(FieldPath(preservedPathStr),
                                        [](const bool isLastElement, FieldMapEntry& entry) {
                                            entry._hasLeadingObj = true;
                                            entry._hasKeep = true;
                                        });
        }
    }

    void processComputedPaths(const projection_executor::InclusionNode& node,
                              const std::string& rootProjection,
                              const bool isAddingFields) {
        std::set<std::string> computedPaths;
        StringMap<std::string> renamedPaths;
        node.reportComputedPaths(&computedPaths, &renamedPaths);

        // Handle path renames: essentially single element FieldPath expression.
        for (const auto& renamedPathEntry : renamedPaths) {
            ABT path = translateFieldPath(
                FieldPath(renamedPathEntry.second),
                make<PathIdentity>(),
                [](const std::string& fieldName, const bool isLastElement, ABT input) {
                    return make<PathGet>(fieldName,
                                         isLastElement ? std::move(input)
                                                       : make<PathTraverse>(std::move(input)));
                });

            auto entry = _ctx.getNode();
            const std::string& renamedProjName = _ctx.getNextId("projRenamedPath");
            _ctx.setNode<EvaluationNode>(
                entry._rootProjection,
                renamedProjName,
                make<EvalPath>(std::move(path), make<Variable>(entry._rootProjection)),
                std::move(entry._node));

            _builder.integrateFieldPath(FieldPath(renamedPathEntry.first),
                                        [&renamedProjName, &isAddingFields](
                                            const bool isLastElement, FieldMapEntry& entry) {
                                            if (!isAddingFields) {
                                                entry._hasKeep = true;
                                            }
                                            if (isLastElement) {
                                                entry._constVarName = renamedProjName;
                                                entry._hasTrailingDefault = true;
                                            }
                                        });
        }

        // Handle general expression projection.
        for (const std::string& computedPathStr : computedPaths) {
            const FieldPath computedPath(computedPathStr);

            auto entry = _ctx.getNode();
            const std::string& getProjName = _ctx.getNextId("projGetPath");
            ABT getExpr = generateAggExpression(
                node.getExpressionForPath(computedPath).get(), rootProjection, getProjName);

            _ctx.setNode<EvaluationNode>(std::move(entry._rootProjection),
                                         getProjName,
                                         std::move(getExpr),
                                         std::move(entry._node));

            _builder.integrateFieldPath(
                computedPath,
                [&getProjName, &isAddingFields](const bool isLastElement, FieldMapEntry& entry) {
                    if (!isAddingFields) {
                        entry._hasKeep = true;
                    }
                    if (isLastElement) {
                        entry._constVarName = getProjName;
                        entry._hasTrailingDefault = true;
                    }
                });
        }
    }

    void visitInclusionNode(const projection_executor::InclusionNode& node,
                            const bool isAddingFields) {
        auto entry = _ctx.getNode();
        const std::string rootProjection = entry._rootProjection;

        processProjectedPaths(node);
        processComputedPaths(node, rootProjection, isAddingFields);
    }

    void visitExclusionNode(const projection_executor::ExclusionNode& node) {
        std::set<std::string> preservedPaths;
        node.reportProjectedPaths(&preservedPaths);

        for (const std::string& preservedPathStr : preservedPaths) {
            _builder.integrateFieldPath(FieldPath(preservedPathStr),
                                        [](const bool isLastElement, FieldMapEntry& entry) {
                                            if (isLastElement) {
                                                entry._hasDrop = true;
                                            }
                                        });
        }
    }

    DSAlgebrizerContext& _ctx;
    FieldMapBuilder& _builder;
};

class ABTDocumentSourceVisitor : public DocumentSourceConstVisitor {
public:
    ABTDocumentSourceVisitor(DSAlgebrizerContext& ctx, const Metadata& metadata)
        : _ctx(ctx), _metadata(metadata) {}

    void visit(const DocumentSourceBucketAuto* source) override {
        unsupportedStage(source);
    }

    void visit(const DocumentSourceCollStats* source) override {
        unsupportedStage(source);
    }

    void visit(const DocumentSourceCurrentOp* source) override {
        unsupportedStage(source);
    }

    void visit(const DocumentSourceCursor* source) override {
        unsupportedStage(source);
    }

    void visit(const DocumentSourceExchange* source) override {
        unsupportedStage(source);
    }

    void visit(const DocumentSourceFacet* source) override {
        unsupportedStage(source);
    }

    void visit(const DocumentSourceGeoNear* source) override {
        unsupportedStage(source);
    }

    void visit(const DocumentSourceGeoNearCursor* source) override {
        unsupportedStage(source);
    }

    void visit(const DocumentSourceGraphLookUp* source) override {
        unsupportedStage(source);
    }

    void visit(const DocumentSourceGroup* source) override {
        const StringMap<boost::intrusive_ptr<Expression>>& idFields = source->getIdFields();
        uassert(6624201, "Empty idFields map", !idFields.empty());

        std::vector<FieldNameType> groupByFieldNames;
        for (const auto& [fieldName, expr] : idFields) {
            groupByFieldNames.push_back(fieldName);
        }
        // Sort in order to generate consistent plans.
        std::sort(groupByFieldNames.begin(), groupByFieldNames.end());

        ProjectionNameVector groupByProjNames;
        auto entry = _ctx.getNode();
        for (const FieldNameType& fieldName : groupByFieldNames) {
            const ProjectionName groupByProjName = _ctx.getNextId("groupByProj");
            groupByProjNames.push_back(groupByProjName);

            ABT groupByExpr = generateAggExpression(
                idFields.at(fieldName).get(), entry._rootProjection, groupByProjName);

            _ctx.setNode<EvaluationNode>(entry._rootProjection,
                                         groupByProjName,
                                         std::move(groupByExpr),
                                         std::move(entry._node));
            entry = _ctx.getNode();
        }

        // Fields corresponding to each accumulator
        ProjectionNameVector aggProjFieldNames;
        // Projection names corresponding to each high-level accumulator ($avg can be broken down
        // into sum and count.).
        ProjectionNameVector aggOutputProjNames;
        // Projection names corresponding to each low-level accumulator (no $avg).
        ProjectionNameVector aggLowLevelOutputProjNames;

        ABTVector aggregationProjections;
        const std::vector<AccumulationStatement>& accumulatedFields =
            source->getAccumulatedFields();

        // Used to keep track which $sum and $count projections to use to compute $avg.
        struct AvgProjNames {
            ProjectionName _output;
            ProjectionName _sum;
            ProjectionName _count;
        };
        std::vector<AvgProjNames> avgProjNames;

        for (const AccumulationStatement& stmt : accumulatedFields) {
            const FieldNameType& fieldName = stmt.fieldName;
            aggProjFieldNames.push_back(fieldName);

            ProjectionName aggOutputProjName = _ctx.getNextId(fieldName + "_agg");

            ABT aggInputExpr = generateAggExpression(
                stmt.expr.argument.get(), entry._rootProjection, aggOutputProjName);
            if (!aggInputExpr.is<Constant>() && !aggInputExpr.is<Variable>()) {
                // Generate nodes for complex projections, otherwise inline constants and variables
                // into the group.
                const ProjectionName aggInputProjName = _ctx.getNextId("groupByInputProj");
                _ctx.setNode<EvaluationNode>(entry._rootProjection,
                                             aggInputProjName,
                                             std::move(aggInputExpr),
                                             std::move(entry._node));
                entry = _ctx.getNode();
                aggInputExpr = make<Variable>(aggInputProjName);
            }

            aggOutputProjNames.push_back(aggOutputProjName);
            if (stmt.makeAccumulator()->getOpName() == "$avg"_sd) {
                // Express $avg as sum / count.
                ProjectionName sumProjName = _ctx.getNextId(fieldName + "_sum_agg");
                aggLowLevelOutputProjNames.push_back(sumProjName);
                ProjectionName countProjName = _ctx.getNextId(fieldName + "_count_agg");
                aggLowLevelOutputProjNames.push_back(countProjName);
                avgProjNames.emplace_back(AvgProjNames{std::move(aggOutputProjName),
                                                       std::move(sumProjName),
                                                       std::move(countProjName)});

                aggregationProjections.emplace_back(
                    make<FunctionCall>("$sum", makeSeq(aggInputExpr)));
                aggregationProjections.emplace_back(
                    make<FunctionCall>("$sum", makeSeq(Constant::int64(1))));
            } else {
                aggLowLevelOutputProjNames.push_back(std::move(aggOutputProjName));
                aggregationProjections.emplace_back(make<FunctionCall>(
                    stmt.makeAccumulator()->getOpName(), makeSeq(std::move(aggInputExpr))));
            }
        }

        ABT result = make<GroupByNode>(ProjectionNameVector{groupByProjNames},
                                       aggLowLevelOutputProjNames,
                                       aggregationProjections,
                                       std::move(entry._node));

        for (auto&& [outputProjName, sumProjName, countProjName] : avgProjNames) {
            result = make<EvaluationNode>(
                std::move(outputProjName),
                make<If>(make<BinaryOp>(
                             Operations::Gt, make<Variable>(countProjName), Constant::int64(0)),
                         make<BinaryOp>(Operations::Div,
                                        make<Variable>(std::move(sumProjName)),
                                        make<Variable>(countProjName)),
                         Constant::nothing()),
                std::move(result));
        }

        ABT integrationPath = make<PathIdentity>();
        for (size_t i = 0; i < groupByFieldNames.size(); i++) {
            maybeComposePath(integrationPath,
                             make<PathField>(std::move(groupByFieldNames.at(i)),
                                             make<PathConstant>(make<Variable>(
                                                 std::move(groupByProjNames.at(i))))));
        }
        for (size_t i = 0; i < aggProjFieldNames.size(); i++) {
            maybeComposePath(
                integrationPath,
                make<PathField>(aggProjFieldNames.at(i),
                                make<PathConstant>(make<Variable>(aggOutputProjNames.at(i)))));
        }

        entry = _ctx.getNode();
        const std::string& mergeProject = _ctx.getNextId("agg_project");
        _ctx.setNode<EvaluationNode>(
            mergeProject,
            mergeProject,
            make<EvalPath>(std::move(integrationPath), Constant::emptyObject()),
            std::move(result));
    }

    void visit(const DocumentSourceIndexStats* source) override {
        unsupportedStage(source);
    }

    void visit(const DocumentSourceInternalInhibitOptimization* source) override {
        // Can be ignored.
    }

    void visit(const DocumentSourceInternalShardFilter* source) override {
        unsupportedStage(source);
    }

    void visit(const DocumentSourceInternalSplitPipeline* source) override {
        unsupportedStage(source);
    }

    void visit(const DocumentSourceLimit* source) override {
        pushLimitSkip(source->getLimit(), 0);
    }

    void visit(const DocumentSourceListCachedAndActiveUsers* source) override {
        unsupportedStage(source);
    }

    void visit(const DocumentSourceListLocalSessions* source) override {
        unsupportedStage(source);
    }

    void visit(const DocumentSourceListSessions* source) override {
        unsupportedStage(source);
    }

    void visit(const DocumentSourceLookUp* source) override {
        // This is an **experimental** implementation of $lookup. To achieve fully compatible
        // implementation we need the following:
        //   1. Add support for unwind to emit not just the array elements, but in addition the
        //   array itself. Such unwinding needs to occur on the inner side in order to match the
        //   left side both against the elements and the array itself. The inner side would perform
        //   a regular unwind.
        //   2. Add support for left outer join. Currently we only results when there is a match.
        //   3. Add ability to generate unique values (sequential or otherwise) in order to
        //   eliminate reliance of _id. This can be achieved for example via a stateful function.
        //   Currently, after joining the unwound elements, we perform a de-duplication based on _id
        //   to determine which corresponding documents match.

        uassert(6624303, "$lookup needs to be SBE compatible", source->sbeCompatible());

        std::string scanDefName = source->getFromNs().coll().toString();
        const ProjectionName& scanProjName = _ctx.getNextId("scan");

        PrefixId prefixId;
        ABT pipelineABT = _metadata._scanDefs.at(scanDefName).exists()
            ? make<ScanNode>(scanProjName, scanDefName)
            : make<ValueScanNode>(ProjectionNameVector{scanProjName});

        const ProjectionName& localIdProjName = _ctx.getNextId("localId");
        auto entry = _ctx.getNode();
        _ctx.setNode<EvaluationNode>(entry._rootProjection,
                                     localIdProjName,
                                     make<EvalPath>(make<PathGet>("_id", make<PathIdentity>()),
                                                    make<Variable>(entry._rootProjection)),
                                     std::move(entry._node));

        const auto& localPath = source->getLocalField();
        ProjectionName localProjName = entry._rootProjection;
        for (size_t index = 0; index < localPath->getPathLength(); index++) {
            ABT path = make<EvalPath>(
                make<PathGet>(localPath->getFieldName(index).toString(), make<PathIdentity>()),
                make<Variable>(std::move(localProjName)));

            localProjName = _ctx.getNextId("local");
            entry = _ctx.getNode();
            _ctx.setNode<EvaluationNode>(
                entry._rootProjection, localProjName, std::move(path), std::move(entry._node));

            entry = _ctx.getNode();
            _ctx.setNode<UnwindNode>(std::move(entry._rootProjection),
                                     localProjName,
                                     _ctx.getNextId("unwoundPidLocal"),
                                     true /*retainNonArrays*/,
                                     std::move(entry._node));
        }

        const ProjectionName& foreignIdProjName = _ctx.getNextId("foreignId");
        pipelineABT =
            make<EvaluationNode>(foreignIdProjName,
                                 make<EvalPath>(make<PathGet>("_id", make<PathIdentity>()),
                                                make<Variable>(scanProjName)),
                                 std::move(pipelineABT));

        const auto& foreignPath = source->getForeignField();
        ProjectionName foreignProjName = scanProjName;
        for (size_t index = 0; index < foreignPath->getPathLength(); index++) {
            ABT path = make<EvalPath>(
                make<PathGet>(foreignPath->getFieldName(index).toString(), make<PathIdentity>()),
                make<Variable>(foreignProjName));

            foreignProjName = _ctx.getNextId("foreign");
            pipelineABT =
                make<EvaluationNode>(foreignProjName, std::move(path), std::move(pipelineABT));

            // For the last field on the path we need to also emit arrays as values themselves (we
            // need a new unwind option).
            pipelineABT = make<UnwindNode>(foreignProjName,
                                           _ctx.getNextId("unwoundPidForeign"),
                                           true /*retainNonArrays*/,
                                           std::move(pipelineABT));
        }

        const auto comparisonExprFn = [](const ProjectionName& projName) {
            return make<If>(make<FunctionCall>("exists", makeSeq(make<Variable>(projName))),
                            make<Variable>(projName),
                            Constant::null());
        };

        entry = _ctx.getNode();
        // TODO: use LeftOuter join when we support it.
        _ctx.setNode<BinaryJoinNode>(std::move(entry._rootProjection),
                                     JoinType::Inner,
                                     ProjectionNameSet{},
                                     make<BinaryOp>(Operations::Eq,
                                                    comparisonExprFn(localProjName),
                                                    comparisonExprFn(foreignProjName)),
                                     std::move(entry._node),
                                     std::move(pipelineABT));

        const ProjectionName& localRootProjId = _ctx.getNextId("localRoot");
        const ProjectionName& foreignRootProjId = _ctx.getNextId("foreignRoot");
        entry = _ctx.getNode();
        ABT groupByDedupNode = make<GroupByNode>(
            ProjectionNameVector{localIdProjName, foreignIdProjName},
            ProjectionNameVector{localRootProjId, foreignRootProjId},
            makeSeq(make<FunctionCall>("$first", makeSeq(make<Variable>(entry._rootProjection))),
                    make<FunctionCall>("$first", makeSeq(make<Variable>(scanProjName)))),
            std::move(entry._node));

        const ProjectionName& localFoldedProjName = _ctx.getNextId("localFolded");
        const ProjectionName& foreignFoldedProjName = _ctx.getNextId("foreignFolded");
        ABT groupByFoldNode = make<GroupByNode>(
            ProjectionNameVector{localIdProjName},
            ProjectionNameVector{localFoldedProjName, foreignFoldedProjName},
            makeSeq(make<FunctionCall>("$first", makeSeq(make<Variable>(localRootProjId))),
                    make<FunctionCall>("$push", makeSeq(make<Variable>(foreignRootProjId)))),
            std::move(groupByDedupNode));

        ABT resultPath = translateFieldPath(
            source->getAsField(),
            make<PathConstant>(make<Variable>(foreignFoldedProjName)),
            [](const std::string& fieldName, const bool isLastElement, ABT input) {
                if (!isLastElement) {
                    input = make<PathTraverse>(std::move(input));
                }
                return make<PathField>(fieldName, std::move(input));
            });

        const ProjectionName& resultProjName = _ctx.getNextId("result");
        _ctx.setNode<EvaluationNode>(
            resultProjName,
            resultProjName,
            make<EvalPath>(std::move(resultPath), make<Variable>(localFoldedProjName)),
            std::move(groupByFoldNode));
    }

    void visit(const DocumentSourceMatch* source) override {
        auto entry = _ctx.getNode();
        ABT matchExpr = generateMatchExpression(source->getMatchExpression(),
                                                true /*allowAggExpressions*/,
                                                entry._rootProjection,
                                                _ctx.getNextId("matchExpression"));

        // If we have a top-level composition, create separate filters.
        const auto& composition = collectComposed(matchExpr);
        for (const auto& path : composition) {
            _ctx.setNode<FilterNode>(entry._rootProjection,
                                     make<EvalFilter>(path, make<Variable>(entry._rootProjection)),
                                     std::move(entry._node));
            entry = _ctx.getNode();
        }
    }

    void visit(const DocumentSourceMerge* source) override {
        unsupportedStage(source);
    }

    void visit(const DocumentSourceMergeCursors* source) override {
        unsupportedStage(source);
    }

    void visit(const DocumentSourceOperationMetrics* source) override {
        unsupportedStage(source);
    }

    void visit(const DocumentSourceOut* source) override {
        unsupportedStage(source);
    }

    void visit(const DocumentSourcePlanCacheStats* source) override {
        unsupportedStage(source);
    }

    void visit(const DocumentSourceQueue* source) override {
        unsupportedStage(source);
    }

    void visit(const DocumentSourceRedact* source) override {
        unsupportedStage(source);
    }

    void visit(const DocumentSourceSample* source) override {
        unsupportedStage(source);
    }

    void visit(const DocumentSourceSampleFromRandomCursor* source) override {
        unsupportedStage(source);
    }

    void visit(const DocumentSourceSequentialDocumentCache* source) override {
        unsupportedStage(source);
    }

    void visit(const DocumentSourceSingleDocumentTransformation* source) override {
        const ProjectionName& rootProjName = _ctx.getNode()._rootProjection;
        FieldMapBuilder builder(rootProjName, rootProjName == _ctx.getScanProjName());
        ABTTransformerVisitor visitor(_ctx, builder);
        TransformerInterfaceWalker walker(&visitor);
        walker.walk(&source->getTransformer());
        visitor.generateCombinedProjection();
    }

    void visit(const DocumentSourceSkip* source) override {
        pushLimitSkip(-1, source->getSkip());
    }

    void visit(const DocumentSourceSort* source) override {
        ProjectionCollationSpec collationSpec;
        const SortPattern& pattern = source->getSortKeyPattern();
        for (size_t i = 0; i < pattern.size(); i++) {
            const SortPattern::SortPatternPart& part = pattern[i];
            if (!part.fieldPath.has_value()) {
                // TODO: consider metadata expression.
                continue;
            }

            const std::string& sortProjName = _ctx.getNextId("sort");
            collationSpec.emplace_back(
                sortProjName, part.isAscending ? CollationOp::Ascending : CollationOp::Descending);

            const FieldPath& fieldPath = part.fieldPath.value();
            ABT sortPath = make<PathIdentity>();
            for (size_t j = 0; j < fieldPath.getPathLength(); j++) {
                sortPath = make<PathGet>(fieldPath.getFieldName(j).toString(), std::move(sortPath));
            }

            auto entry = _ctx.getNode();
            _ctx.setNode<EvaluationNode>(
                entry._rootProjection,
                sortProjName,
                make<EvalPath>(std::move(sortPath), make<Variable>(entry._rootProjection)),
                std::move(entry._node));
        }

        if (!collationSpec.empty()) {
            auto entry = _ctx.getNode();
            _ctx.setNode<CollationNode>(std::move(entry._rootProjection),
                                        properties::CollationRequirement(std::move(collationSpec)),
                                        std::move(entry._node));
        }

        if (source->getLimit().has_value()) {
            // We need to limit the result of the collation.
            pushLimitSkip(source->getLimit().value(), 0);
        }
    }

    void visit(const DocumentSourceTeeConsumer* source) override {
        unsupportedStage(source);
    }

    void visit(const DocumentSourceUnionWith* source) override {
        auto entry = _ctx.getNode();
        ProjectionName unionProjName = entry._rootProjection;

        const Pipeline& pipeline = source->getPipeline();

        NamespaceString involvedNss = pipeline.getContext()->ns;
        std::string scanDefName = involvedNss.coll().toString();
        const ProjectionName& scanProjName = _ctx.getNextId("scan");

        PrefixId prefixId;
        ABT initialNode = _metadata._scanDefs.at(scanDefName).exists()
            ? make<ScanNode>(scanProjName, scanDefName)
            : make<ValueScanNode>(ProjectionNameVector{scanProjName});
        ABT pipelineABT = translatePipelineToABT(
            _metadata, pipeline, scanProjName, std::move(initialNode), prefixId);

        uassert(6624425, "Expected root node for union pipeline", pipelineABT.is<RootNode>());
        ABT pipelineABTWithoutRoot = pipelineABT.cast<RootNode>()->getChild();
        // Pull out the root projection(s) from the inner pipeline.
        const ProjectionNameVector& rootProjections =
            pipelineABT.cast<RootNode>()->getProperty().getProjections().getVector();
        uassert(6624426,
                "Expected a single projection for inner union branch",
                rootProjections.size() == 1);

        // Add an evaluation node such that it shares a projection with the outer pipeline. If the
        // same projection name is already defined in the inner pipeline then there's no need for
        // the extra eval node.
        const ProjectionName& innerProjection = rootProjections[0];
        ProjectionName newRootProj = unionProjName;
        if (innerProjection != unionProjName) {
            ABT evalNodeInner = make<EvaluationNode>(
                unionProjName,
                make<EvalPath>(make<PathIdentity>(), make<Variable>(innerProjection)),
                std::move(pipelineABTWithoutRoot));
            _ctx.setNode(
                std::move(newRootProj),
                make<UnionNode>(ProjectionNameVector{std::move(unionProjName)},
                                makeSeq(std::move(entry._node), std::move(evalNodeInner))));
        } else {
            _ctx.setNode(std::move(newRootProj),
                         make<UnionNode>(
                             ProjectionNameVector{std::move(unionProjName)},
                             makeSeq(std::move(entry._node), std::move(pipelineABTWithoutRoot))));
        }
    }

    void visit(const DocumentSourceUnwind* source) override {
        const FieldPath& unwindFieldPath = source->getUnwindPath();
        const bool preserveNullAndEmpty = source->preserveNullAndEmptyArrays();

        const std::string pidProjName = _ctx.getNextId("unwoundPid");
        const std::string unwoundProjName = _ctx.getNextId("unwoundProj");

        const auto generatePidGteZeroTest = [&pidProjName](ABT thenCond, ABT elseCond) {
            return make<If>(
                make<BinaryOp>(Operations::Gte, make<Variable>(pidProjName), Constant::int64(0)),
                std::move(thenCond),
                std::move(elseCond));
        };

        ABT embedPath = make<Variable>(unwoundProjName);
        if (preserveNullAndEmpty) {
            const std::string unwindLambdaVarName = _ctx.getNextId("unwoundLambdaVarName");
            embedPath = make<PathLambda>(make<LambdaAbstraction>(
                unwindLambdaVarName,
                generatePidGteZeroTest(std::move(embedPath), make<Variable>(unwindLambdaVarName))));
        } else {
            embedPath = make<PathConstant>(std::move(embedPath));
        }
        embedPath = translateFieldPath(
            unwindFieldPath,
            std::move(embedPath),
            [](const std::string& fieldName, const bool isLastElement, ABT input) {
                return make<PathField>(fieldName,
                                       isLastElement ? std::move(input)
                                                     : make<PathTraverse>(std::move(input)));
            });

        ABT unwoundPath = translateFieldPath(
            unwindFieldPath,
            make<PathIdentity>(),
            [](const std::string& fieldName, const bool isLastElement, ABT input) {
                return make<PathGet>(fieldName, std::move(input));
            });

        auto entry = _ctx.getNode();
        _ctx.setNode<EvaluationNode>(
            entry._rootProjection,
            unwoundProjName,
            make<EvalPath>(std::move(unwoundPath), make<Variable>(entry._rootProjection)),
            std::move(entry._node));

        entry = _ctx.getNode();
        _ctx.setNode<UnwindNode>(std::move(entry._rootProjection),
                                 unwoundProjName,
                                 pidProjName,
                                 preserveNullAndEmpty,
                                 std::move(entry._node));

        entry = _ctx.getNode();
        const std::string embedProjName = _ctx.getNextId("embedProj");
        _ctx.setNode<EvaluationNode>(
            embedProjName,
            embedProjName,
            make<EvalPath>(std::move(embedPath), make<Variable>(entry._rootProjection)),
            std::move(entry._node));

        if (source->indexPath().has_value()) {
            const FieldPath indexFieldPath = source->indexPath().get();
            if (indexFieldPath.getPathLength() > 0) {
                ABT indexPath = translateFieldPath(
                    indexFieldPath,
                    make<PathConstant>(
                        generatePidGteZeroTest(make<Variable>(pidProjName), Constant::null())),
                    [](const std::string& fieldName, const bool isLastElement, ABT input) {
                        return make<PathField>(fieldName, std::move(input));
                    });

                entry = _ctx.getNode();
                const std::string embedPidProjName = _ctx.getNextId("embedPidProj");
                _ctx.setNode<EvaluationNode>(
                    embedPidProjName,
                    embedPidProjName,
                    make<EvalPath>(std::move(indexPath), make<Variable>(entry._rootProjection)),
                    std::move(entry._node));
            }
        }
    }

private:
    void unsupportedStage(const DocumentSource* source) const {
        uasserted(ErrorCodes::InternalErrorNotSupported,
                  str::stream() << "Stage is not supported: " << source->getSourceName());
    }

    void pushLimitSkip(const int64_t limit, const int64_t skip) {
        auto entry = _ctx.getNode();
        _ctx.setNode<LimitSkipNode>(std::move(entry._rootProjection),
                                    properties::LimitSkipRequirement(limit, skip),
                                    std::move(entry._node));
    }

    DSAlgebrizerContext& _ctx;
    const Metadata& _metadata;
};

ABT translatePipelineToABT(const Metadata& metadata,
                           const Pipeline& pipeline,
                           ProjectionName scanProjName,
                           ABT initialNode,
                           PrefixId& prefixId) {
    DSAlgebrizerContext ctx(prefixId, {scanProjName, std::move(initialNode)});
    ABTDocumentSourceVisitor visitor(ctx, metadata);

    DocumentSourceWalker walker(nullptr /*preVisitor*/, &visitor);
    walker.walk(pipeline);

    auto entry = ctx.getNode();
    return make<RootNode>(
        properties::ProjectionRequirement{ProjectionNameVector{std::move(entry._rootProjection)}},
        std::move(entry._node));
}

}  // namespace mongo::optimizer
