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

#pragma once

#include <string>
#include <utility>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/explain_interface.h"
#include "mongo/db/query/optimizer/node_defs.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"


namespace mongo::optimizer {

enum class ExplainVersion { V1, V2, V2Compact, V3, UserFacingExplain, Vmax };

/**
 * Given the RootNode of an ABT, determine whether the ABT represents an EOF plan. This function
 * checks for the following form:
 *
 * RootNode
 * |
 * EvaluationNode
 * |     <>: Nothing
 * LimitSkipNode
 * |     limit: 0, skip: 0
 * CoScanNode
 */
bool isEOFPlan(ABT::reference_type node);

/**
 * This structure holds any data that is required by the explain. It is self-sufficient and separate
 * because it must outlive the other optimizer state as it is used by the runtime plan executor.
 */
class ABTPrinter : public AbstractABTPrinter {
public:
    ABTPrinter(PlanAndProps planAndProps, ExplainVersion explainVersion, QueryParameterMap qpMap);

    BSONObj explainBSON() const final;
    std::string getPlanSummary() const final;
    BSONObj getQueryParameters() const final;

private:
    PlanAndProps _planAndProps;
    ExplainVersion _explainVersion;
    QueryParameterMap _queryParameters;
};

/**
 * Stringifies paths and expressions in an ABT for queryPlanner explain purposes. For example, the
 * following ABT:
 * EvalFilter []
 * |   Variable [p3]
 * PathTraverse [1] PathComposeM []
 * |   PathCompare [Gt] Const [nan]
 * PathCompare [Lt] Const [5]
 *
 * results in the following string:
 * "EvalFilter (Traverse [1] ComposeM (< Const [5]) (> Const [nan])) (Var [p3])"
 */
class StringifyPathsAndExprs {
public:
    static std::string stringify(ABT::reference_type node);
};

/**
 * This transport is used to generate the user-facing representation of the ABT which is shown in
 * the queryPlanner section of explain output. It assumes that the input is a physical ABT. This
 * currently only works for M2 plans under tryBonsai.
 */
class UserFacingExplain {
public:
    UserFacingExplain(const NodeToGroupPropsMap& nodeMap = {}) : _nodeMap(nodeMap) {}

    // Constants relevant to all stages.
    constexpr static StringData kStage = "stage"_sd;
    constexpr static StringData kNodeId = "planNodeId"_sd;
    constexpr static StringData kProj = "projections"_sd;
    constexpr static StringData kInput = "inputStage"_sd;

    // Specific to PhysicalScanNode.
    constexpr static StringData kScanName = "COLLSCAN"_sd;
    constexpr static StringData kDir = "direction"_sd;
    constexpr static StringData kForward = "forward"_sd;
    constexpr static StringData kBackward = "backward"_sd;
    constexpr static StringData kRandom = "random"_sd;

    // Specific to FilterNode.
    constexpr static StringData kFilterName = "FILTER"_sd;
    constexpr static StringData kFilter = "filter"_sd;

    // Specific to EvaluationNode.
    constexpr static StringData kEvalName = "EVALUATION"_sd;

    // Specific to RootNode.
    constexpr static StringData kRootName = "ROOT"_sd;

    // Specific to EOF.
    constexpr static StringData kEOF = "EOF"_sd;

    // The default noop case.
    template <typename T, typename... Ts>
    void walk(const T&, BSONObjBuilder* bob, Ts&&...) {
        // If we get here, that means we are trying to generate explain for an unsupported node. We
        // should never generate an unsupported node to explain to begin with.
        tasserted(8075606, "Trying to generate explain for an unsupported node.");
    }

    void walk(const RootNode& node, BSONObjBuilder* bob, const ABT& child, const ABT& /* refs */) {
        bob->append(kStage, kRootName);

        BSONArrayBuilder projs(bob->subarrayStart(kProj));
        for (const auto& projName : node.getProjections().getVector()) {
            projs.append(projName.value());
        }
        projs.doneFast();

        BSONObjBuilder inputBob(bob->subobjStart(kInput));
        generateExplain(child, &inputBob);
    }

    void walk(const FilterNode& node, BSONObjBuilder* bob, const ABT& child, const ABT& expr) {
        auto it = _nodeMap.find(&node);
        tassert(8075601, "Failed to find node properties", it != _nodeMap.end());
        const NodeProps& props = it->second;

        bob->append(kStage, kFilterName);
        bob->append(kNodeId, props._planNodeId);
        bob->append(kFilter, StringifyPathsAndExprs::stringify(expr));

        BSONObjBuilder inputBob(bob->subobjStart(kInput));
        generateExplain(child, &inputBob);
    }

    void walk(const EvaluationNode& node,
              BSONObjBuilder* bob,
              const ABT& child,
              const ABT& /* expr */) {
        auto it = _nodeMap.find(&node);
        tassert(8075602, "Failed to find node properties", it != _nodeMap.end());
        const NodeProps& props = it->second;

        bob->append(kStage, kEvalName);
        bob->append(kNodeId, props._planNodeId);

        BSONObjBuilder projectionsBob(bob->subobjStart(kProj));
        projectionsBob.append(node.getProjectionName().value(),
                              StringifyPathsAndExprs::stringify(node.getProjection()));
        projectionsBob.doneFast();

        BSONObjBuilder inputBob(bob->subobjStart(kInput));
        generateExplain(child, &inputBob);
    }

    void walk(const PhysicalScanNode& node, BSONObjBuilder* bob, const ABT& /* bind */) {
        auto it = _nodeMap.find(&node);
        tassert(8075603, "Failed to find node properties", it != _nodeMap.end());
        const NodeProps& props = it->second;

        bob->append(kStage, kScanName);
        bob->append(kNodeId, props._planNodeId);

        switch (node.getScanOrder()) {
            case ScanOrder::Forward:
                bob->append(kDir, kForward);
                break;
            case ScanOrder::Reverse:
                bob->append(kDir, kBackward);
                break;
            case ScanOrder::Random:
                bob->append(kDir, kRandom);
                break;
        }

        auto map = node.getFieldProjectionMap();
        std::map<FieldNameType, ProjectionName> ordered;
        if (const auto& projName = map._ridProjection) {
            ordered.emplace("<rid>", *projName);
        }
        if (const auto& projName = map._rootProjection) {
            ordered.emplace("<root>", *projName);
        }
        for (const auto& entry : map._fieldProjections) {
            ordered.insert(entry);
        }
        BSONObjBuilder fieldProjs(bob->subobjStart(kProj));
        for (const auto& [fieldName, projectionName] : ordered) {
            fieldProjs.append(projectionName.value(), fieldName.value());
        }
        fieldProjs.doneFast();
    }

    void generateExplain(const ABT::reference_type n, BSONObjBuilder* bob) {
        algebra::walk<false>(n, *this, bob);
    }

    BSONObj generateEOFPlan(const ABT::reference_type node) {
        BSONObjBuilder bob;

        auto it = _nodeMap.find(node.cast<Node>());
        tassert(8075605, "Failed to find node properties", it != _nodeMap.end());
        const NodeProps& props = it->second;

        bob.append(kStage, kEOF);
        bob.append(kNodeId, props._planNodeId);

        return bob.obj();
    }

    BSONObj explain(const ABT::reference_type node) {
        // Short circuit to return EOF stage if the collection is empty.
        if (isEOFPlan(node)) {
            return generateEOFPlan(node);
        }

        BSONObjBuilder bob;
        generateExplain(node, &bob);

        BSONObj result = bob.obj();

        // If at this point (after the walk) the explain BSON is empty, that means the ABT had no
        // nodes (if it had any unsupported nodes, we would have hit the MONGO_UNREACHABLE in the
        // default case above).
        tassert(8075604, "The ABT has no nodes.", !result.isEmpty());

        return result;
    }

private:
    const NodeToGroupPropsMap& _nodeMap;
};

class ExplainGenerator {
public:
    // Optionally display logical and physical properties using the memo.
    // whenever memo delegators are printed.
    static std::string explain(ABT::reference_type node,
                               bool displayProperties = false,
                               const NodeToGroupPropsMap& nodeMap = {});

    // Optionally display logical and physical properties using the memo.
    // whenever memo delegators are printed.
    static std::string explainV2(ABT::reference_type node,
                                 bool displayProperties = false,
                                 const NodeToGroupPropsMap& nodeMap = {});

    // Optionally display logical and physical properties using the memo.
    // whenever memo delegators are printed.
    static std::string explainV2Compact(ABT::reference_type node,
                                        bool displayProperties = false,
                                        const NodeToGroupPropsMap& nodeMap = {});

    static std::string explainNode(ABT::reference_type node);

    static std::pair<sbe::value::TypeTags, sbe::value::Value> explainBSON(
        ABT::reference_type node,
        bool displayProperties = false,
        const NodeToGroupPropsMap& nodeMap = {});

    static BSONObj explainBSONObj(ABT::reference_type node,
                                  bool displayProperties = false,
                                  const NodeToGroupPropsMap& nodeMap = {});

    static std::string explainBSONStr(ABT::reference_type node,
                                      bool displayProperties = false,
                                      const NodeToGroupPropsMap& nodeMap = {});
};

}  // namespace mongo::optimizer
