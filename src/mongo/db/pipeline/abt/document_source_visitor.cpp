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

#include "mongo/db/pipeline/abt/document_source_visitor.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <s2cellid.h>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/abt/agg_expression_visitor.h"
#include "mongo/db/pipeline/abt/algebrizer_context.h"
#include "mongo/db/pipeline/abt/collation_translation.h"
#include "mongo/db/pipeline/abt/field_map_builder.h"
#include "mongo/db/pipeline/abt/match_expression_visitor.h"
#include "mongo/db/pipeline/abt/transformer_visitor.h"
#include "mongo/db/pipeline/abt/utils.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_source_union_with.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/visitors/document_source_visitor_registry_mongod.h"
#include "mongo/db/pipeline/visitors/document_source_walker.h"
#include "mongo/db/pipeline/visitors/transformer_interface_walker.h"
#include "mongo/db/query/optimizer/algebra/polyvalue.h"
#include "mongo/db/query/optimizer/comparison_op.h"
#include "mongo/db/query/optimizer/containers.h"
#include "mongo/db/query/optimizer/syntax/expr.h"
#include "mongo/db/query/optimizer/syntax/path.h"
#include "mongo/db/query/optimizer/utils/path_utils.h"
#include "mongo/db/query/optimizer/utils/strong_alias.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

namespace mongo::optimizer {

void ABTDocumentSourceTranslationVisitorContext::pushLimitSkip(const int64_t limit,
                                                               const int64_t skip) {
    auto entry = algCtx.getNode();
    algCtx.setNode<LimitSkipNode>(
        std::move(entry._rootProjection), limit, skip, std::move(entry._node));
}

template <typename T>
void visit(ABTDocumentSourceTranslationVisitorContext*, const T& source) {
    uasserted(ErrorCodes::InternalErrorNotSupported,
              str::stream() << "Stage is not supported: " << source.getSourceName());
}

void visit(ABTDocumentSourceTranslationVisitorContext* visitorCtx,
           const DocumentSourceGroup& source) {
    const StringMap<boost::intrusive_ptr<Expression>>& idFields = source.getIdFields();
    uassert(6624201, "Empty idFields map", !idFields.empty());

    std::vector<FieldNameType> groupByFieldNames;
    for (const auto& [fieldName, expr] : idFields) {
        groupByFieldNames.push_back(FieldNameType{fieldName});
    }
    const bool isSingleIdField =
        groupByFieldNames.size() == 1 && groupByFieldNames.front() == "_id";

    // Sort in order to generate consistent plans.
    std::sort(groupByFieldNames.begin(), groupByFieldNames.end());

    ProjectionNameVector groupByProjNames;
    AlgebrizerContext& ctx = visitorCtx->algCtx;
    auto entry = ctx.getNode();
    for (const FieldNameType& fieldName : groupByFieldNames) {
        const ProjectionName groupByProjName = ctx.getNextId("groupByProj");
        groupByProjNames.push_back(groupByProjName);

        ABT groupByExpr = generateAggExpression(
            idFields.at(fieldName.value()).get(), entry._rootProjection, ctx.getPrefixId());

        ctx.setNode<EvaluationNode>(
            entry._rootProjection, groupByProjName, std::move(groupByExpr), std::move(entry._node));
        entry = ctx.getNode();
    }

    // Fields corresponding to each accumulator
    std::vector<FieldNameType> aggProjFieldNames;
    // Projection names corresponding to each high-level accumulator ($avg can be broken down
    // into sum and count.).
    ProjectionNameVector aggOutputProjNames;
    // Projection names corresponding to each low-level accumulator (no $avg).
    ProjectionNameVector aggLowLevelOutputProjNames;

    ABTVector aggregationProjections;
    const std::vector<AccumulationStatement>& accumulatedFields =
        source.getAccumulationStatements();

    // Used to keep track which $sum and $count projections to use to compute $avg.
    struct AvgProjNames {
        ProjectionName _output;
        ProjectionName _sum;
        ProjectionName _count;
    };
    std::vector<AvgProjNames> avgProjNames;

    for (const AccumulationStatement& stmt : accumulatedFields) {
        const FieldNameType fieldName{stmt.fieldName};
        aggProjFieldNames.push_back(fieldName);

        ProjectionName aggOutputProjName{ctx.getNextId("field_agg")};

        ABT aggInputExpr = generateAggExpression(
            stmt.expr.argument.get(), entry._rootProjection, ctx.getPrefixId());
        if (!aggInputExpr.is<Constant>() && !aggInputExpr.is<Variable>()) {
            // Generate nodes for complex projections, otherwise inline constants and variables
            // into the group.
            const ProjectionName aggInputProjName = ctx.getNextId("groupByInputProj");
            ctx.setNode<EvaluationNode>(entry._rootProjection,
                                        aggInputProjName,
                                        std::move(aggInputExpr),
                                        std::move(entry._node));
            entry = ctx.getNode();
            aggInputExpr = make<Variable>(aggInputProjName);
        }

        aggOutputProjNames.push_back(aggOutputProjName);
        if (stmt.makeAccumulator()->getOpName() == "$avg"_sd) {
            // Express $avg as sum / count.
            ProjectionName sumProjName{ctx.getNextId("field_sum_agg")};
            aggLowLevelOutputProjNames.push_back(sumProjName);
            ProjectionName countProjName{ctx.getNextId("field_count_agg")};
            aggLowLevelOutputProjNames.push_back(countProjName);
            avgProjNames.emplace_back(AvgProjNames{
                std::move(aggOutputProjName), std::move(sumProjName), std::move(countProjName)});

            aggregationProjections.emplace_back(make<FunctionCall>("$sum", makeSeq(aggInputExpr)));
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
            make<If>(
                make<BinaryOp>(Operations::Gt, make<Variable>(countProjName), Constant::int64(0)),
                make<BinaryOp>(Operations::Div,
                               make<Variable>(std::move(sumProjName)),
                               make<Variable>(countProjName)),
                Constant::nothing()),
            std::move(result));
    }

    ABT integrationPath = make<PathIdentity>();
    for (size_t i = 0; i < groupByFieldNames.size(); i++) {
        std::string fieldName = groupByFieldNames.at(i).value().toString();
        if (!isSingleIdField) {
            // Erase '_id.' prefix.
            fieldName = fieldName.substr(strlen("_id."));
        }

        maybeComposePath(
            integrationPath,
            make<PathField>(FieldNameType{std::move(fieldName)},
                            make<PathConstant>(make<Variable>(std::move(groupByProjNames.at(i))))));
    }
    if (!isSingleIdField) {
        integrationPath = make<PathField>("_id", std::move(integrationPath));
    }

    for (size_t i = 0; i < aggProjFieldNames.size(); i++) {
        maybeComposePath(
            integrationPath,
            make<PathField>(aggProjFieldNames.at(i),
                            make<PathConstant>(make<Variable>(aggOutputProjNames.at(i)))));
    }

    entry = ctx.getNode();
    const ProjectionName mergeProject{ctx.getNextId("agg_project")};
    ctx.setNode<EvaluationNode>(mergeProject,
                                mergeProject,
                                make<EvalPath>(std::move(integrationPath), Constant::emptyObject()),
                                std::move(result));
}

void visit(ABTDocumentSourceTranslationVisitorContext* visitorCtx,
           const DocumentSourceLimit& source) {
    visitorCtx->pushLimitSkip(source.getLimit(), 0);
}

void visit(ABTDocumentSourceTranslationVisitorContext* visitorCtx,
           const DocumentSourceLookUp& source) {
    // This is an **experimental** implementation of $lookup. To achieve fully compatible
    // implementation we need the following:
    //   1. Check and potentially fix array to array comparison.
    //   2. Add ability to generate unique values (sequential or otherwise) in order to
    //   eliminate reliance of _id. This can be achieved for example via a stateful function.
    //   Currently, after joining the unwound elements, we perform a de-duplication based on _id
    //   to determine which corresponding documents match.

    uassert(6624303,
            "$lookup needs to be SBE compatible",
            source.sbeCompatibility() != SbeCompatibility::notCompatible);

    std::string scanDefName = source.getFromNs().coll().toString();
    AlgebrizerContext& ctx = visitorCtx->algCtx;
    const ProjectionName& scanProjName = ctx.getNextId("scan");

    // This assumes that the inner collection exists. If this is not the case, we will produce an
    // invalid SBE plan.
    ABT pipelineABT = make<ScanNode>(scanProjName, scanDefName);

    const ProjectionName& localIdProjName = ctx.getNextId("localId");
    auto entry = ctx.getNode();
    ctx.setNode<EvaluationNode>(entry._rootProjection,
                                localIdProjName,
                                make<EvalPath>(make<PathGet>("_id", make<PathIdentity>()),
                                               make<Variable>(entry._rootProjection)),
                                std::move(entry._node));

    const auto& localPath = source.getLocalField();
    ABT localPathGet = translateFieldPath(
        *localPath,
        make<PathIdentity>(),
        [](FieldNameType fieldName, const bool isLastElement, ABT input) {
            return make<PathGet>(
                std::move(fieldName),
                isLastElement ? std::move(input)
                              : make<PathTraverse>(PathTraverse::kUnlimited, std::move(input)));
        });

    auto localPathProjName = ctx.getNextId("localPath");
    entry = ctx.getNode();
    ctx.setNode<EvaluationNode>(
        entry._rootProjection,
        localPathProjName,
        make<EvalPath>(std::move(localPathGet), make<Variable>(entry._rootProjection)),
        std::move(entry._node));

    auto localProjName = ctx.getNextId("local");
    entry = ctx.getNode();
    ctx.setNode<EvaluationNode>(entry._rootProjection,
                                localProjName,
                                make<BinaryOp>(Operations::FillEmpty,
                                               make<Variable>(std::move(localPathProjName)),
                                               Constant::null()),
                                std::move(entry._node));

    const auto& foreignPath = source.getForeignField();
    ABT foreignSimplePath = translateFieldPath(
        *foreignPath,
        make<PathCompare>(Operations::EqMember, make<Variable>(localProjName)),
        [](FieldNameType fieldName, const bool /*isLastElement*/, ABT input) {
            return make<PathGet>(std::move(fieldName),
                                 make<PathTraverse>(PathTraverse::kSingleLevel, std::move(input)));
        });

    // Retain only the top-level get into foreignSimplePath.
    ABT foreignPathCmp = make<PathIdentity>();
    std::swap(foreignPathCmp, foreignSimplePath.cast<PathGet>()->getPath());

    ProjectionName foreignProjName = ctx.getNextId("remotePath");
    pipelineABT = make<EvaluationNode>(
        foreignProjName,
        make<EvalPath>(std::move(foreignSimplePath), make<Variable>(scanProjName)),
        std::move(pipelineABT));

    entry = ctx.getNode();
    ctx.setNode<BinaryJoinNode>(
        std::move(entry._rootProjection),
        JoinType::Left,
        ProjectionNameSet{},
        make<EvalFilter>(std::move(foreignPathCmp), make<Variable>(std::move(foreignProjName))),
        std::move(entry._node),
        std::move(pipelineABT));

    entry = ctx.getNode();

    const ProjectionName& localFoldedProjName = ctx.getNextId("localFolded");
    const ProjectionName& foreignFoldedProjName = ctx.getNextId("foreignFolded");
    ABT groupByFoldNode = make<GroupByNode>(
        ProjectionNameVector{localIdProjName},
        ProjectionNameVector{localFoldedProjName, foreignFoldedProjName},
        makeSeq(make<FunctionCall>("$first", makeSeq(make<Variable>(entry._rootProjection))),
                make<FunctionCall>("$push", makeSeq(make<Variable>(scanProjName)))),
        std::move(entry._node));

    ABT resultPath = translateFieldPath(
        source.getAsField(),
        make<PathConstant>(make<Variable>(foreignFoldedProjName)),
        [](FieldNameType fieldName, const bool isLastElement, ABT input) {
            if (!isLastElement) {
                input = make<PathTraverse>(PathTraverse::kUnlimited, std::move(input));
            }
            return make<PathField>(std::move(fieldName), std::move(input));
        });

    const ProjectionName& resultProjName = ctx.getNextId("result");
    ctx.setNode<EvaluationNode>(
        resultProjName,
        resultProjName,
        make<EvalPath>(std::move(resultPath), make<Variable>(localFoldedProjName)),
        std::move(groupByFoldNode));
}

void visit(ABTDocumentSourceTranslationVisitorContext* visitorCtx,
           const DocumentSourceMatch& source) {
    AlgebrizerContext& ctx = visitorCtx->algCtx;
    auto entry = ctx.getNode();
    ABT matchExpr = generateMatchExpression(source.getMatchExpression(),
                                            true /*allowAggExpressions*/,
                                            entry._rootProjection,
                                            ctx.getPrefixId(),
                                            ctx.getQueryParameters());

    {
        // If we have a top-level composition, flatten it into a chain of separate FilterNodes. We
        // are adding the entire subtree to the context.
        auto result = decomposeToFilterNodes(entry._node,
                                             matchExpr,
                                             make<Variable>(entry._rootProjection),
                                             1 /*minDepth*/,
                                             visitorCtx->maxFilterDepth);
        ctx.setNode(entry._rootProjection, std::move(*result));
        entry = ctx.getNode();
    }
}

void visit(ABTDocumentSourceTranslationVisitorContext* visitorCtx,
           const DocumentSourceSingleDocumentTransformation& source) {
    AlgebrizerContext& ctx = visitorCtx->algCtx;
    const ProjectionName& rootProjName = ctx.getNode()._rootProjection;
    FieldMapBuilder builder(rootProjName, rootProjName == ctx.getScanProjName());
    ABTTransformerVisitor visitor(ctx, builder);
    TransformerInterfaceWalker walker(&visitor);
    walker.walk(&source.getTransformer());
    visitor.generateCombinedProjection();
}

void visit(ABTDocumentSourceTranslationVisitorContext* visitorCtx,
           const DocumentSourceSkip& source) {
    visitorCtx->pushLimitSkip(-1, source.getSkip());
}

void visit(ABTDocumentSourceTranslationVisitorContext* visitorCtx,
           const DocumentSourceSort& source) {
    AlgebrizerContext& ctx = visitorCtx->algCtx;
    generateCollationNode(ctx, source.getSortKeyPattern());

    if (source.getLimit().has_value()) {
        // We need to limit the result of the collation.
        visitorCtx->pushLimitSkip(source.getLimit().value(), 0);
    }
}

void visit(ABTDocumentSourceTranslationVisitorContext* visitorCtx,
           const DocumentSourceUnionWith& source) {
    AlgebrizerContext& ctx = visitorCtx->algCtx;
    auto entry = ctx.getNode();
    ProjectionName unionProjName = entry._rootProjection;

    const Pipeline& pipeline = source.getPipeline();

    NamespaceString involvedNss = pipeline.getContext()->ns;
    std::string scanDefName = involvedNss.coll().toString();
    const ProjectionName& scanProjName = ctx.getNextId("scan");

    // This assumes that the second collection in the $unionWith exists. If this is not the case, we
    // will produce an invalid SBE plan.
    ABT initialNode = make<ScanNode>(scanProjName, scanDefName);

    ABT pipelineABT = translatePipelineToABT(pipeline,
                                             scanProjName,
                                             std::move(initialNode),
                                             ctx.getPrefixId(),
                                             ctx.getQueryParameters());

    uassert(6624425, "Expected root node for union pipeline", pipelineABT.is<RootNode>());
    ABT pipelineABTWithoutRoot = pipelineABT.cast<RootNode>()->getChild();
    // Pull out the root projection(s) from the inner pipeline.
    const ProjectionNameVector& rootProjections =
        pipelineABT.cast<RootNode>()->getProjections().getVector();
    uassert(6624426,
            "Expected a single projection for inner union branch",
            rootProjections.size() == 1);

    // Add an evaluation node such that it shares a projection with the outer
    // pipeline. If the same projection name is already defined in the inner pipeline
    // then there's no need for the extra eval node.
    const ProjectionName& innerProjection = rootProjections[0];
    ProjectionName newRootProj = unionProjName;
    if (innerProjection != unionProjName) {
        ABT evalNodeInner = make<EvaluationNode>(
            unionProjName,
            make<EvalPath>(make<PathIdentity>(), make<Variable>(innerProjection)),
            std::move(pipelineABTWithoutRoot));
        ctx.setNode(std::move(newRootProj),
                    make<UnionNode>(ProjectionNameVector{std::move(unionProjName)},
                                    makeSeq(std::move(entry._node), std::move(evalNodeInner))));
    } else {
        ctx.setNode(
            std::move(newRootProj),
            make<UnionNode>(ProjectionNameVector{std::move(unionProjName)},
                            makeSeq(std::move(entry._node), std::move(pipelineABTWithoutRoot))));
    }
}

void visit(ABTDocumentSourceTranslationVisitorContext* visitorCtx,
           const DocumentSourceUnwind& source) {
    const FieldPath& unwindFieldPath = source.getUnwindPath();
    const bool preserveNullAndEmpty = source.preserveNullAndEmptyArrays();

    AlgebrizerContext& ctx = visitorCtx->algCtx;
    const ProjectionName pidProjName{ctx.getNextId("unwoundPid")};
    const ProjectionName unwoundProjName{ctx.getNextId("unwoundProj")};

    const auto generatePidGteZeroTest = [&pidProjName](ABT thenCond, ABT elseCond) {
        return make<If>(
            make<BinaryOp>(Operations::Gte, make<Variable>(pidProjName), Constant::int64(0)),
            std::move(thenCond),
            std::move(elseCond));
    };

    ABT embedPath = make<Variable>(unwoundProjName);
    if (preserveNullAndEmpty) {
        const ProjectionName unwindLambdaVarName{ctx.getNextId("unwoundLambdaVarName")};
        embedPath = make<PathLambda>(make<LambdaAbstraction>(
            unwindLambdaVarName,
            generatePidGteZeroTest(std::move(embedPath), make<Variable>(unwindLambdaVarName))));
    } else {
        embedPath = make<PathConstant>(std::move(embedPath));
    }
    embedPath = translateFieldPath(
        unwindFieldPath,
        std::move(embedPath),
        [](FieldNameType fieldName, const bool isLastElement, ABT input) {
            return make<PathField>(
                std::move(fieldName),
                isLastElement ? std::move(input)
                              : make<PathTraverse>(PathTraverse::kUnlimited, std::move(input)));
        });

    ABT unwoundPath =
        translateFieldPath(unwindFieldPath,
                           make<PathIdentity>(),
                           [](FieldNameType fieldName, const bool isLastElement, ABT input) {
                               return make<PathGet>(std::move(fieldName), std::move(input));
                           });

    auto entry = ctx.getNode();
    ctx.setNode<EvaluationNode>(
        entry._rootProjection,
        unwoundProjName,
        make<EvalPath>(std::move(unwoundPath), make<Variable>(entry._rootProjection)),
        std::move(entry._node));

    entry = ctx.getNode();
    ctx.setNode<UnwindNode>(std::move(entry._rootProjection),
                            unwoundProjName,
                            pidProjName,
                            preserveNullAndEmpty,
                            std::move(entry._node));

    entry = ctx.getNode();
    const ProjectionName embedProjName{ctx.getNextId("embedProj")};
    ctx.setNode<EvaluationNode>(
        embedProjName,
        embedProjName,
        make<EvalPath>(std::move(embedPath), make<Variable>(entry._rootProjection)),
        std::move(entry._node));

    if (source.indexPath().has_value()) {
        const FieldPath indexFieldPath = source.indexPath().value();
        if (indexFieldPath.getPathLength() > 0) {
            ABT indexPath = translateFieldPath(
                indexFieldPath,
                make<PathConstant>(
                    generatePidGteZeroTest(make<Variable>(pidProjName), Constant::null())),
                [](FieldNameType fieldName, const bool /*isLastElement*/, ABT input) {
                    return make<PathField>(std::move(fieldName), std::move(input));
                });

            entry = ctx.getNode();
            const ProjectionName embedPidProjName{ctx.getNextId("embedPidProj")};
            ctx.setNode<EvaluationNode>(
                embedPidProjName,
                embedPidProjName,
                make<EvalPath>(std::move(indexPath), make<Variable>(entry._rootProjection)),
                std::move(entry._node));
        }
    }
}

const ServiceContext::ConstructorActionRegisterer abtTranslationRegisterer{
    "ABTTranslationRegisterer", [](ServiceContext* service) {
        registerMongodVisitor<ABTDocumentSourceTranslationVisitorContext>(service);
    }};

ABT translatePipelineToABT(const Pipeline& pipeline,
                           ProjectionName scanProjName,
                           ABT initialNode,
                           PrefixId& prefixId,
                           QueryParameterMap& queryParameters,
                           size_t maxFilterDepth) {
    AlgebrizerContext ctx(prefixId, {scanProjName, std::move(initialNode)}, queryParameters);
    ABTDocumentSourceTranslationVisitorContext visitorCtx(ctx, maxFilterDepth);

    ServiceContext* serviceCtx = pipeline.getContext()->opCtx->getServiceContext();
    auto& reg = getDocumentSourceVisitorRegistry(serviceCtx);
    DocumentSourceWalker walker(reg, &visitorCtx);
    walker.walk(pipeline);

    auto entry = ctx.getNode();
    return make<RootNode>(ProjectionNameVector{std::move(entry._rootProjection)},
                          std::move(entry._node));
}

}  // namespace mongo::optimizer
