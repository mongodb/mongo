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

#include "mongo/db/query/optimizer/utils/utils.h"

#include <absl/container/node_hash_map.h>
#include <absl/container/node_hash_set.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include <algorithm>
#include <initializer_list>
#include <istream>

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/optimizer/algebra/operator.h"
#include "mongo/db/query/optimizer/algebra/polyvalue.h"
#include "mongo/db/query/optimizer/index_bounds.h"
#include "mongo/db/query/optimizer/syntax/expr.h"
#include "mongo/db/query/optimizer/syntax/path.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"
#include "mongo/db/query/optimizer/utils/interval_utils.h"
#include "mongo/db/query/optimizer/utils/path_utils.h"
#include "mongo/util/assert_util.h"


namespace mongo::optimizer {

ABT makeBalancedBooleanOpTree(Operations logicOp, std::vector<ABT> leaves) {
    auto builder = [=](ABT lhs, ABT rhs) {
        return make<BinaryOp>(logicOp, std::move(lhs), std::move(rhs));
    };
    return makeBalancedTreeImpl(builder, leaves, 0, leaves.size());
}

void restrictProjections(ProjectionNameVector projNames, const CEType ce, PhysPlanBuilder& input) {
    input.make<UnionNode>(ce, std::move(projNames), makeSeq(std::move(input._node)));
}

std::string encodeIndexKeyName(const size_t indexField) {
    std::ostringstream os;
    os << kIndexKeyPrefix << " " << indexField;
    return os.str();
}

size_t decodeIndexKeyName(const std::string& fieldName) {
    std::istringstream is(fieldName);

    std::string prefix;
    is >> prefix;
    uassert(6624151, "Invalid index key prefix", prefix == kIndexKeyPrefix);

    int key;
    is >> key;
    return key;
}

/**
 * Transport that checks if we have a primitive interval which may contain null.
 */
class PartialSchemaReqMayContainNullTransport {
public:
    bool transport(const IntervalReqExpr::Atom& node, const ConstFoldFn& constFold) {
        return mayContainNull(node, constFold);
    }

    bool transport(const IntervalReqExpr::Conjunction& node,
                   const ConstFoldFn& constFold,
                   std::vector<bool> childResults) {
        return std::all_of(
            childResults.cbegin(), childResults.cend(), [](const bool v) { return v; });
    }

    bool transport(const IntervalReqExpr::Disjunction& node,
                   const ConstFoldFn& constFold,
                   std::vector<bool> childResults) {
        return std::any_of(
            childResults.cbegin(), childResults.cend(), [](const bool v) { return v; });
    }

    bool check(const IntervalReqExpr::Node& intervals, const ConstFoldFn& constFold) {
        return algebra::transport<false>(intervals, *this, constFold);
    }
};

bool checkMaybeHasNull(const IntervalReqExpr::Node& intervals, const ConstFoldFn& constFold) {
    return PartialSchemaReqMayContainNullTransport{}.check(intervals, constFold);
}

/**
 * Identifies all Eq or EqMember nodes in the childResults. If there is more than one,
 * they are consolidated into one larger EqMember node with a new array containing
 * all of the individual node's value.
 */
static void consolidateEqDisjunctions(ABTVector& childResults) {
    ABTVector newResults;
    const auto [eqMembersTag, eqMembersVal] = sbe::value::makeNewArray();
    sbe::value::ValueGuard guard{eqMembersTag, eqMembersVal};
    const auto eqMembersArray = sbe::value::getArrayView(eqMembersVal);

    for (size_t index = 0; index < childResults.size(); index++) {
        ABT& child = childResults.at(index);
        if (const auto pathCompare = child.cast<PathCompare>();
            pathCompare != nullptr && pathCompare->getVal().is<Constant>()) {
            switch (pathCompare->op()) {
                case Operations::EqMember: {
                    // This is unreachable since the PSR is assumed to be in DNF form when converted
                    // to a filter node. This means that any EqMember from translation would have
                    // first been decomposed into individual EQ predicates under a disjunction.
                    tasserted(7987902, "Unexpected eqMember");
                }
                case Operations::Eq: {
                    // Copy this node's value into the new eqMembersArray.
                    const auto [tag, val] = pathCompare->getVal().cast<Constant>()->get();
                    const auto [newTag, newVal] = sbe::value::copyValue(tag, val);
                    eqMembersArray->push_back(newTag, newVal);
                    break;
                }
                default:
                    newResults.emplace_back(std::move(child));
                    continue;
            }
        } else {
            newResults.emplace_back(std::move(child));
        }
    }

    // Add a node to the newResults with the combined Eq/EqMember values under one EqMember; if
    // there is only one value, add an Eq node with that value.
    if (eqMembersArray->size() > 1) {
        guard.reset();
        newResults.emplace_back(
            make<PathCompare>(Operations::EqMember, make<Constant>(eqMembersTag, eqMembersVal)));
    } else if (eqMembersArray->size() == 1) {
        const auto [eqConstantTag, eqConstantVal] =
            sbe::value::copyValue(eqMembersArray->getAt(0).first, eqMembersArray->getAt(0).second);
        newResults.emplace_back(
            make<PathCompare>(Operations::Eq, make<Constant>(eqConstantTag, eqConstantVal)));
    }

    std::swap(childResults, newResults);
}

class PartialSchemaReqLowerTransport {
public:
    PartialSchemaReqLowerTransport(const bool hasBoundProjName,
                                   const PathToIntervalFn& pathToInterval)
        : _hasBoundProjName(hasBoundProjName),
          _intervalArr(getInterval(pathToInterval, make<PathArr>())),
          _intervalObj(getInterval(pathToInterval, make<PathObj>())) {}

    ABT transport(const IntervalReqExpr::Atom& node) {
        const auto& interval = node.getExpr();
        const auto& lowBound = interval.getLowBound();
        const auto& highBound = interval.getHighBound();

        if (interval.isEquality()) {
            if (auto constPtr = lowBound.getBound().cast<Constant>()) {
                if (constPtr->isNull()) {
                    return make<PathComposeA>(make<PathDefault>(Constant::boolean(true)),
                                              make<PathCompare>(Operations::Eq, Constant::null()));
                }
                return make<PathCompare>(Operations::Eq, lowBound.getBound());
            } else {
                uassert(6624164,
                        "Cannot lower variable index bound with bound projection",
                        !_hasBoundProjName);
                return make<PathCompare>(Operations::Eq, lowBound.getBound());
            }
        }

        if (interval == _intervalArr) {
            return make<PathArr>();
        }
        if (interval == _intervalObj) {
            return make<PathObj>();
        }

        ABT result = make<PathIdentity>();
        if (!lowBound.isMinusInf()) {
            maybeComposePath(
                result,
                make<PathCompare>(lowBound.isInclusive() ? Operations::Gte : Operations::Gt,
                                  lowBound.getBound()));
        }
        if (!highBound.isPlusInf()) {
            maybeComposePath(
                result,
                make<PathCompare>(highBound.isInclusive() ? Operations::Lte : Operations::Lt,
                                  highBound.getBound()));
        }
        return result;
    }

    ABT transport(const IntervalReqExpr::Conjunction& node, ABTVector childResults) {
        // Construct a balanced multiplicative composition tree.
        maybeComposePaths<PathComposeM>(childResults);
        return std::move(childResults.front());
    }

    ABT transport(const IntervalReqExpr::Disjunction& node, ABTVector childResults) {
        if (childResults.size() > 1) {
            // Consolidate Eq and EqMember disjunctions, then construct a balanced additive
            // composition tree.
            consolidateEqDisjunctions(childResults);
            maybeComposePaths<PathComposeA>(childResults);
        }
        return std::move(childResults.front());
    }

    ABT lower(const IntervalReqExpr::Node& intervals) {
        return algebra::transport<false>(intervals, *this);
    }

private:
    /**
     * Convenience for looking up the singular interval that corresponds to either PathArr or
     * PathObj.
     */
    static boost::optional<IntervalRequirement> getInterval(const PathToIntervalFn& pathToInterval,
                                                            const ABT& path) {
        if (pathToInterval) {
            if (auto intervalExpr = pathToInterval(path)) {
                if (auto interval = IntervalReqExpr::getSingularDNF(*intervalExpr)) {
                    return *interval;
                }
            }
        }
        return boost::none;
    }

    const bool _hasBoundProjName;
    const boost::optional<IntervalRequirement> _intervalArr;
    const boost::optional<IntervalRequirement> _intervalObj;
};

}  // namespace mongo::optimizer
