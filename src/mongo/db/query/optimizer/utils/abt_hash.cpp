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

#include "mongo/db/query/optimizer/utils/abt_hash.h"

#include "mongo/db/query/optimizer/node.h"
#include "mongo/db/query/optimizer/utils/utils.h"

namespace mongo::optimizer {

static size_t computeCollationHash(const properties::CollationRequirement& prop) {
    size_t collationHash = 17;
    for (const auto& entry : prop.getCollationSpec()) {
        updateHash(collationHash, std::hash<ProjectionName>()(entry.first));
        updateHash(collationHash, std::hash<CollationOp>()(entry.second));
    }
    return collationHash;
}

static size_t computeLimitSkipHash(const properties::LimitSkipRequirement& prop) {
    size_t limitSkipHash = 17;
    updateHash(limitSkipHash, std::hash<int64_t>()(prop.getLimit()));
    updateHash(limitSkipHash, std::hash<int64_t>()(prop.getSkip()));
    return limitSkipHash;
}

static size_t computePropertyProjectionsHash(const ProjectionNameVector& projections) {
    size_t resultHash = 17;
    for (const ProjectionName& projection : projections) {
        updateHashUnordered(resultHash, std::hash<ProjectionName>()(projection));
    }
    return resultHash;
}

static size_t computeProjectionRequirementHash(const properties::ProjectionRequirement& prop) {
    return computePropertyProjectionsHash(prop.getProjections().getVector());
}

static size_t computeDistributionHash(const properties::DistributionRequirement& prop) {
    size_t resultHash = 17;
    const auto& distribAndProjections = prop.getDistributionAndProjections();
    updateHash(resultHash, std::hash<DistributionType>()(distribAndProjections._type));
    updateHash(resultHash, computePropertyProjectionsHash(distribAndProjections._projectionNames));
    return resultHash;
}

static void updateBoundHash(size_t& result, const BoundRequirement& bound) {
    updateHash(result, std::hash<bool>()(bound.isInclusive()));
    updateHash(result, ABTHashGenerator::generate(bound.getBound()));
};

template <class T>
class IntervalHasher {
public:
    size_t computeHash(const IntervalRequirement& req) {
        size_t result = 17;
        updateBoundHash(result, req.getLowBound());
        updateBoundHash(result, req.getHighBound());
        return 17;
    }

    size_t computeHash(const CompoundIntervalRequirement& req) {
        size_t result = 19;
        for (const auto& interval : req) {
            updateHash(result, computeHash(interval));
        }
        return result;
    }

    size_t transport(const typename T::Atom& node) {
        return computeHash(node.getExpr());
    }

    size_t transport(const typename T::Conjunction& node, const std::vector<size_t> childResults) {
        size_t result = 31;
        for (const size_t childResult : childResults) {
            updateHash(result, childResult);
        }
        return result;
    }

    size_t transport(const typename T::Disjunction& node, const std::vector<size_t> childResults) {
        size_t result = 29;
        for (const size_t childResult : childResults) {
            updateHash(result, childResult);
        }
        return result;
    }

    size_t compute(const typename T::Node& intervals) {
        return algebra::transport<false>(intervals, *this);
    }
};

static size_t computePartialSchemaReqHash(const PartialSchemaRequirements& reqMap) {
    size_t result = 17;

    IntervalHasher<IntervalReqExpr> intervalHasher;
    for (const auto& [key, req] : reqMap) {
        updateHash(result, std::hash<ProjectionName>()(key._projectionName));
        updateHash(result, ABTHashGenerator::generate(key._path));
        updateHash(result, std::hash<ProjectionName>()(req.getBoundProjectionName()));
        updateHash(result, intervalHasher.compute(req.getIntervals()));
    }
    return result;
}

/**
 * Hasher for ABT nodes. Used in conjunction with memo.
 */
class ABTHashTransporter {
public:
    /**
     * Nodes
     */
    template <typename T, typename... Ts>
    size_t transport(const T& /*node*/, Ts&&...) {
        // Physical nodes do not currently need to implement hash.
        static_assert(!canBeLogicalNode<T>(), "Logical node must implement its hash.");
        uasserted(6624142, "must implement custom hash");
    }

    size_t transport(const References& references, std::vector<size_t> inResults) {
        return computeHashSeq<1>(computeVectorHash(inResults));
    }

    size_t transport(const ExpressionBinder& binders, std::vector<size_t> inResults) {
        return computeHashSeq<2>(computeVectorHash(binders.names()), computeVectorHash(inResults));
    }

    size_t transport(const ScanNode& node, size_t bindResult) {
        return computeHashSeq<3>(std::hash<std::string>()(node.getScanDefName()), bindResult);
    }

    size_t transport(const ValueScanNode& node, size_t bindResult) {
        return computeHashSeq<46>(std::hash<size_t>()(node.getArraySize()),
                                  ABTHashGenerator::generate(node.getValueArray()),
                                  bindResult);
    }

    size_t transport(const MemoLogicalDelegatorNode& node) {
        return computeHashSeq<4>(std::hash<GroupIdType>()(node.getGroupId()));
    }

    size_t transport(const FilterNode& node, size_t childResult, size_t filterResult) {
        return computeHashSeq<5>(filterResult, childResult);
    }

    size_t transport(const EvaluationNode& node, size_t childResult, size_t projectionResult) {
        return computeHashSeq<6>(projectionResult, childResult);
    }

    size_t transport(const SargableNode& node,
                     size_t childResult,
                     size_t /*bindResult*/,
                     size_t /*refResult*/) {
        // Specifically not hashing the candidate indexes and ScanParams. Those are derivative of
        // the requirements, and can have temp projection names.
        return computeHashSeq<44>(computePartialSchemaReqHash(node.getReqMap()),
                                  std::hash<IndexReqTarget>()(node.getTarget()),
                                  childResult);
    }

    size_t transport(const RIDIntersectNode& node,
                     size_t leftChildResult,
                     size_t rightChildResult) {
        // Specifically always including children.
        return computeHashSeq<45>(std::hash<ProjectionName>()(node.getScanProjectionName()),
                                  std::hash<bool>()(node.hasLeftIntervals()),
                                  std::hash<bool>()(node.hasRightIntervals()),
                                  leftChildResult,
                                  rightChildResult);
    }

    size_t transport(const BinaryJoinNode& node,
                     size_t leftChildResult,
                     size_t rightChildResult,
                     size_t filterResult) {
        // Specifically always including children.
        return computeHashSeq<7>(filterResult, leftChildResult, rightChildResult);
    }

    size_t transport(const UnionNode& node,
                     std::vector<size_t> childResults,
                     size_t bindResult,
                     size_t refsResult) {
        // Specifically always including children.
        return computeHashSeq<9>(bindResult, refsResult, computeVectorHash(childResults));
    }

    size_t transport(const GroupByNode& node,
                     size_t childResult,
                     size_t bindAggResult,
                     size_t refsAggResult,
                     size_t bindGbResult,
                     size_t refsGbResult) {
        return computeHashSeq<10>(bindAggResult,
                                  refsAggResult,
                                  bindGbResult,
                                  refsGbResult,
                                  std::hash<GroupNodeType>()(node.getType()),
                                  childResult);
    }

    size_t transport(const UnwindNode& node,
                     size_t childResult,
                     size_t bindResult,
                     size_t refsResult) {
        return computeHashSeq<11>(
            std::hash<bool>()(node.getRetainNonArrays()), bindResult, refsResult, childResult);
    }

    size_t transport(const CollationNode& node, size_t childResult, size_t /*refsResult*/) {
        return computeHashSeq<13>(computeCollationHash(node.getProperty()), childResult);
    }

    size_t transport(const LimitSkipNode& node, size_t childResult) {
        return computeHashSeq<14>(computeLimitSkipHash(node.getProperty()), childResult);
    }

    size_t transport(const ExchangeNode& node, size_t childResult, size_t /*refsResult*/) {
        return computeHashSeq<43>(computeDistributionHash(node.getProperty()), childResult);
    }

    size_t transport(const RootNode& node, size_t childResult, size_t /*refsResult*/) {
        return computeHashSeq<15>(computeProjectionRequirementHash(node.getProperty()),
                                  childResult);
    }

    /**
     * Expressions
     */
    size_t transport(const Blackhole& expr) {
        return computeHashSeq<16>();
    }

    size_t transport(const Constant& expr) {
        auto [tag, val] = expr.get();
        return computeHashSeq<17>(sbe::value::hashValue(tag, val));
    }

    size_t transport(const Variable& expr) {
        return computeHashSeq<18>(std::hash<std::string>()(expr.name()));
    }

    size_t transport(const UnaryOp& expr, size_t inResult) {
        return computeHashSeq<19>(std::hash<Operations>()(expr.op()), inResult);
    }

    size_t transport(const BinaryOp& expr, size_t leftResult, size_t rightResult) {
        return computeHashSeq<20>(std::hash<Operations>()(expr.op()), leftResult, rightResult);
    }

    size_t transport(const If& expr, size_t condResult, size_t thenResult, size_t elseResult) {
        return computeHashSeq<21>(condResult, thenResult, elseResult);
    }

    size_t transport(const Let& expr, size_t bindResult, size_t exprResult) {
        return computeHashSeq<22>(std::hash<std::string>()(expr.varName()), bindResult, exprResult);
    }

    size_t transport(const LambdaAbstraction& expr, size_t inResult) {
        return computeHashSeq<23>(std::hash<std::string>()(expr.varName()), inResult);
    }

    size_t transport(const LambdaApplication& expr, size_t lambdaResult, size_t argumentResult) {
        return computeHashSeq<24>(lambdaResult, argumentResult);
    }

    size_t transport(const FunctionCall& expr, std::vector<size_t> argResults) {
        return computeHashSeq<25>(std::hash<std::string>()(expr.name()),
                                  computeVectorHash(argResults));
    }

    size_t transport(const EvalPath& expr, size_t pathResult, size_t inputResult) {
        return computeHashSeq<26>(pathResult, inputResult);
    }

    size_t transport(const EvalFilter& expr, size_t pathResult, size_t inputResult) {
        return computeHashSeq<27>(pathResult, inputResult);
    }

    size_t transport(const Source& expr) {
        return computeHashSeq<28>();
    }

    /**
     * Paths
     */
    size_t transport(const PathConstant& path, size_t inResult) {
        return computeHashSeq<29>(inResult);
    }

    size_t transport(const PathLambda& path, size_t inResult) {
        return computeHashSeq<30>(inResult);
    }

    size_t transport(const PathIdentity& path) {
        return computeHashSeq<31>();
    }

    size_t transport(const PathDefault& path, size_t inResult) {
        return computeHashSeq<32>(inResult);
    }

    size_t transport(const PathCompare& path, size_t valueResult) {
        return computeHashSeq<33>(std::hash<Operations>()(path.op()), valueResult);
    }

    size_t transport(const PathDrop& path) {
        size_t namesHash = 17;
        for (const std::string& name : path.getNames()) {
            updateHash(namesHash, std::hash<std::string>()(name));
        }
        return computeHashSeq<34>(namesHash);
    }

    size_t transport(const PathKeep& path) {
        size_t namesHash = 17;
        for (const std::string& name : path.getNames()) {
            updateHash(namesHash, std::hash<std::string>()(name));
        }
        return computeHashSeq<35>(namesHash);
    }

    size_t transport(const PathObj& path) {
        return computeHashSeq<36>();
    }

    size_t transport(const PathArr& path) {
        return computeHashSeq<37>();
    }

    size_t transport(const PathTraverse& path, size_t inResult) {
        return computeHashSeq<38>(inResult, std::hash<size_t>()(path.getMaxDepth()));
    }

    size_t transport(const PathField& path, size_t inResult) {
        return computeHashSeq<39>(std::hash<std::string>()(path.name()), inResult);
    }

    size_t transport(const PathGet& path, size_t inResult) {
        return computeHashSeq<40>(std::hash<std::string>()(path.name()), inResult);
    }

    size_t transport(const PathComposeM& path, size_t leftResult, size_t rightResult) {
        return computeHashSeq<41>(leftResult, rightResult);
    }

    size_t transport(const PathComposeA& path, size_t leftResult, size_t rightResult) {
        return computeHashSeq<42>(leftResult, rightResult);
    }

    size_t generate(const ABT& node) {
        return algebra::transport<false>(node, *this);
    }

    size_t generate(const ABT::reference_type& nodeRef) {
        return algebra::transport<false>(nodeRef, *this);
    }
};

size_t ABTHashGenerator::generate(const ABT& node) {
    ABTHashTransporter gen;
    return gen.generate(node);
}

size_t ABTHashGenerator::generate(const ABT::reference_type& nodeRef) {
    ABTHashTransporter gen;
    return gen.generate(nodeRef);
}

class PhysPropsHasher {
public:
    size_t operator()(const properties::PhysProperty&,
                      const properties::CollationRequirement& prop) {
        return computeHashSeq<1>(computeCollationHash(prop));
    }

    size_t operator()(const properties::PhysProperty&,
                      const properties::LimitSkipRequirement& prop) {
        return computeHashSeq<2>(computeLimitSkipHash(prop));
    }

    size_t operator()(const properties::PhysProperty&,
                      const properties::ProjectionRequirement& prop) {
        return computeHashSeq<3>(computeProjectionRequirementHash(prop));
    }

    size_t operator()(const properties::PhysProperty&,
                      const properties::DistributionRequirement& prop) {
        return computeHashSeq<4>(computeDistributionHash(prop));
    }

    size_t operator()(const properties::PhysProperty&,
                      const properties::IndexingRequirement& prop) {
        return computeHashSeq<5>(std::hash<IndexReqTarget>()(prop.getIndexReqTarget()),
                                 std::hash<bool>()(prop.getDedupRID()));
    }

    size_t operator()(const properties::PhysProperty&, const properties::RepetitionEstimate& prop) {
        return computeHashSeq<6>(std::hash<CEType>()(prop.getEstimate()));
    }

    size_t operator()(const properties::PhysProperty&, const properties::LimitEstimate& prop) {
        return computeHashSeq<7>(std::hash<CEType>()(prop.getEstimate()));
    }

    static size_t computeHash(const properties::PhysProps& props) {
        PhysPropsHasher visitor;
        size_t result = 17;
        for (const auto& prop : props) {
            updateHashUnordered(result, prop.second.visit(visitor));
        }
        return result;
    }
};

size_t ABTHashGenerator::generateForPhysProps(const properties::PhysProps& props) {
    return PhysPropsHasher::computeHash(props);
}

}  // namespace mongo::optimizer
