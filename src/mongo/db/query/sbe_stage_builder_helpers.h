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

#include <absl/container/inlined_vector.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "boost/container/detail/std_fwd.hpp"
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/ordering.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/exec/sbe/abt/abt_lower_defs.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/match_path.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/hash_agg.h"
#include "mongo/db/exec/sbe/stages/makeobj.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/optimizer/comparison_op.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/projection_ast.h"
#include "mongo/db/query/sbe_stage_builder_sbexpr.h"
#include "mongo/db/query/sbe_stage_builder_state.h"
#include "mongo/db/query/stage_types.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/field_set.h"
#include "mongo/util/string_map.h"

namespace mongo::projection_ast {
class Projection;
}

namespace mongo::stage_builder {

class PlanStageSlots;
struct Environment;
struct PlanStageStaticData;

std::unique_ptr<sbe::EExpression> makeUnaryOp(sbe::EPrimUnary::Op unaryOp,
                                              std::unique_ptr<sbe::EExpression> operand);

/**
 * Wrap expression into logical negation.
 */
std::unique_ptr<sbe::EExpression> makeNot(std::unique_ptr<sbe::EExpression> e);

std::unique_ptr<sbe::EExpression> makeBinaryOp(sbe::EPrimBinary::Op binaryOp,
                                               std::unique_ptr<sbe::EExpression> lhs,
                                               std::unique_ptr<sbe::EExpression> rhs);

std::unique_ptr<sbe::EExpression> makeBinaryOpWithCollation(sbe::EPrimBinary::Op binaryOp,
                                                            std::unique_ptr<sbe::EExpression> lhs,
                                                            std::unique_ptr<sbe::EExpression> rhs,
                                                            StageBuilderState& state);

/**
 * Generates an EExpression that checks if the input expression is null or missing.
 */
std::unique_ptr<sbe::EExpression> generateNullOrMissing(const sbe::EVariable& var);

std::unique_ptr<sbe::EExpression> generateNullOrMissing(sbe::FrameId frameId,
                                                        sbe::value::SlotId slotId);

std::unique_ptr<sbe::EExpression> generateNullOrMissing(std::unique_ptr<sbe::EExpression> arg);

/**
 * Generates an EExpression that checks if the input expression is a non-numeric type _assuming
 * that_ it has already been verified to be neither null nor missing.
 */
std::unique_ptr<sbe::EExpression> generateNonNumericCheck(const sbe::EVariable& var);

/**
 * Generates an EExpression that checks if the input expression is the value NumberLong(-2^64).
 */
std::unique_ptr<sbe::EExpression> generateLongLongMinCheck(const sbe::EVariable& var);

/**
 * Generates an EExpression that checks if the input expression is NaN _assuming that_ it has
 * already been verified to be numeric.
 */
std::unique_ptr<sbe::EExpression> generateNaNCheck(const sbe::EVariable& var);

/**
 * Generates an EExpression that checks if the input expression is a numeric Infinity.
 */
std::unique_ptr<sbe::EExpression> generateInfinityCheck(const sbe::EVariable& var);

/**
 * Generates an EExpression that checks if the input expression is a non-positive number (i.e. <= 0)
 * _assuming that_ it has already been verified to be numeric.
 */
std::unique_ptr<sbe::EExpression> generateNonPositiveCheck(const sbe::EVariable& var);

/**
 * Generates an EExpression that checks if the input expression is a positive number (i.e. > 0)
 * _assuming that_ it has already been verified to be numeric.
 */
std::unique_ptr<sbe::EExpression> generatePositiveCheck(const sbe::EVariable& var);

/**
 * Generates an EExpression that checks if the input expression is a negative (i.e., < 0) number
 * _assuming that_ it has already been verified to be numeric.
 */
std::unique_ptr<sbe::EExpression> generateNegativeCheck(const sbe::EVariable& var);

/**
 * Generates an EExpression that checks if the input expression is _not_ an object, _assuming that_
 * it has already been verified to be neither null nor missing.
 */
std::unique_ptr<sbe::EExpression> generateNonObjectCheck(const sbe::EVariable& var);

/**
 * Generates an EExpression that checks if the input expression is not a string, _assuming that
 * it has already been verified to be neither null nor missing.
 */
std::unique_ptr<sbe::EExpression> generateNonStringCheck(const sbe::EVariable& var);

/**
 * Generates an EExpression that checks whether the input expression is null, missing, or
 * unable to be converted to the type NumberInt32.
 */
std::unique_ptr<sbe::EExpression> generateNullishOrNotRepresentableInt32Check(
    const sbe::EVariable& var);

std::unique_ptr<sbe::EExpression> generateNonTimestampCheck(const sbe::EVariable& var);

/**
 * A pair representing a 1) true/false condition and 2) the value that should be returned if that
 * condition evaluates to true.
 */
using CaseValuePair =
    std::pair<std::unique_ptr<sbe::EExpression>, std::unique_ptr<sbe::EExpression>>;

/**
 * Convert a list of CaseValuePairs into a chain of EIf expressions, with the final else case
 * evaluating to the 'defaultValue' EExpression.
 */
template <typename... Ts>
std::unique_ptr<sbe::EExpression> buildMultiBranchConditional(Ts... cases);

template <typename... Ts>
std::unique_ptr<sbe::EExpression> buildMultiBranchConditional(CaseValuePair headCase, Ts... rest) {
    return sbe::makeE<sbe::EIf>(std::move(headCase.first),
                                std::move(headCase.second),
                                buildMultiBranchConditional(std::move(rest)...));
}

template <>
std::unique_ptr<sbe::EExpression> buildMultiBranchConditional(
    std::unique_ptr<sbe::EExpression> defaultCase);

/**
 * Converts a std::vector of CaseValuePairs into a chain of EIf expressions in the same manner as
 * the 'buildMultiBranchConditional()' function.
 */
std::unique_ptr<sbe::EExpression> buildMultiBranchConditionalFromCaseValuePairs(
    std::vector<CaseValuePair> caseValuePairs, std::unique_ptr<sbe::EExpression> defaultValue);

/**
 * Create tree consisting of coscan stage followed by limit stage.
 */
std::unique_ptr<sbe::PlanStage> makeLimitCoScanTree(PlanNodeId planNodeId, long long limit = 1);

/**
 * Check if expression returns Nothing and return boolean false if so. Otherwise, return the
 * expression.
 */
std::unique_ptr<sbe::EExpression> makeFillEmptyFalse(std::unique_ptr<sbe::EExpression> e);

std::unique_ptr<sbe::EExpression> makeFillEmptyTrue(std::unique_ptr<sbe::EExpression> e);

/**
 * Creates an EFunction expression with the given name and arguments.
 */
inline std::unique_ptr<sbe::EExpression> makeFunction(StringData name,
                                                      sbe::EExpression::Vector args) {
    return sbe::makeE<sbe::EFunction>(name, std::move(args));
}

template <typename... Args>
inline std::unique_ptr<sbe::EExpression> makeFunction(StringData name, Args&&... args) {
    return sbe::makeE<sbe::EFunction>(name, sbe::makeEs(std::forward<Args>(args)...));
}

inline auto makeConstant(sbe::value::TypeTags tag, sbe::value::Value val) {
    return sbe::makeE<sbe::EConstant>(tag, val);
}

inline auto makeNothingConstant() {
    return sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Nothing, 0);
}
inline auto makeNullConstant() {
    return sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0);
}
inline auto makeBoolConstant(bool boolVal) {
    auto val = sbe::value::bitcastFrom<bool>(boolVal);
    return sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean, val);
}
inline auto makeInt32Constant(int32_t num) {
    auto val = sbe::value::bitcastFrom<int32_t>(num);
    return sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32, val);
}
inline auto makeInt64Constant(int64_t num) {
    auto val = sbe::value::bitcastFrom<int64_t>(num);
    return sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64, val);
}
inline auto makeDoubleConstant(double num) {
    auto val = sbe::value::bitcastFrom<double>(num);
    return sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberDouble, val);
}
inline auto makeDecimalConstant(const Decimal128& num) {
    auto [tag, val] = sbe::value::makeCopyDecimal(num);
    return sbe::makeE<sbe::EConstant>(tag, val);
}
inline auto makeStrConstant(StringData str) {
    auto [tag, val] = sbe::value::makeNewString(str);
    return sbe::makeE<sbe::EConstant>(tag, val);
}

std::unique_ptr<sbe::EExpression> makeVariable(TypedSlot ts);

std::unique_ptr<sbe::EExpression> makeVariable(sbe::FrameId frameId, sbe::value::SlotId slotId);

std::unique_ptr<sbe::EExpression> makeMoveVariable(sbe::FrameId frameId, sbe::value::SlotId slotId);

inline auto makeFail(int code, StringData errorMessage) {
    return sbe::makeE<sbe::EFail>(ErrorCodes::Error{code}, errorMessage);
}

/**
 * Check if expression returns Nothing and return null if so. Otherwise, return the expression.
 */
std::unique_ptr<sbe::EExpression> makeFillEmptyNull(std::unique_ptr<sbe::EExpression> e);

/**
 * Check if expression returns Nothing and return bsonUndefined if so. Otherwise, return the
 * expression.
 */
std::unique_ptr<sbe::EExpression> makeFillEmptyUndefined(std::unique_ptr<sbe::EExpression> e);

/**
 * Makes "newObj" function from variadic parameter pack of 'FieldPair' which is a pair of a field
 * name and field expression.
 */
template <typename... Ts>
std::unique_ptr<sbe::EExpression> makeNewObjFunction(Ts... fields);

using FieldPair = std::pair<StringData, std::unique_ptr<sbe::EExpression>>;
template <size_t N>
using FieldExprs = std::array<std::unique_ptr<sbe::EExpression>, N>;

// The following two template functions convert 'FieldPair' to two 'EExpression's and add them to
// 'EExpression' array which will be converted back to variadic parameter pack for 'makeFunction()'.
template <size_t N, size_t... Is>
FieldExprs<N + 2> array_append(FieldExprs<N> fieldExprs,
                               const std::index_sequence<Is...>&,
                               std::unique_ptr<sbe::EExpression> nameExpr,
                               std::unique_ptr<sbe::EExpression> valExpr) {
    return FieldExprs<N + 2>{std::move(fieldExprs[Is])..., std::move(nameExpr), std::move(valExpr)};
}
template <size_t N>
FieldExprs<N + 2> array_append(FieldExprs<N> fieldExprs, FieldPair field) {
    return array_append(std::move(fieldExprs),
                        std::make_index_sequence<N>{},
                        makeStrConstant(field.first),
                        std::move(field.second));
}

// The following two template functions convert the 'EExpression' array back to variadic parameter
// pack and calls the 'makeFunction("newObj")'.
template <size_t N, size_t... Is>
std::unique_ptr<sbe::EExpression> makeNewObjFunction(FieldExprs<N> fieldExprs,
                                                     const std::index_sequence<Is...>&) {
    return makeFunction("newObj", std::move(fieldExprs[Is])...);
}
template <size_t N>
std::unique_ptr<sbe::EExpression> makeNewObjFunction(FieldExprs<N> fieldExprs) {
    return makeNewObjFunction(std::move(fieldExprs), std::make_index_sequence<N>{});
}

// Deals with the last 'FieldPair' and adds it to the 'EExpression' array.
template <size_t N>
std::unique_ptr<sbe::EExpression> makeNewObjFunction(FieldExprs<N> fieldExprs, FieldPair field) {
    return makeNewObjFunction(array_append(std::move(fieldExprs), std::move(field)));
}

// Deals with the intermediary 'FieldPair's and adds them to the 'EExpression' array.
template <size_t N, typename... Ts>
std::unique_ptr<sbe::EExpression> makeNewObjFunction(FieldExprs<N> fieldExprs,
                                                     FieldPair field,
                                                     Ts... fields) {
    return makeNewObjFunction(array_append(std::move(fieldExprs), std::move(field)),
                              std::forward<Ts>(fields)...);
}

// Deals with the first 'FieldPair' and adds it to the 'EExpression' array.
template <typename... Ts>
std::unique_ptr<sbe::EExpression> makeNewObjFunction(FieldPair field, Ts... fields) {
    return makeNewObjFunction(FieldExprs<2>{makeStrConstant(field.first), std::move(field.second)},
                              std::forward<Ts>(fields)...);
}

std::unique_ptr<sbe::EExpression> makeNewBsonObject(std::vector<std::string> projectFields,
                                                    sbe::EExpression::Vector projectValues);

/**
 * Generates an expression that returns shard key that behaves similarly to
 * ShardKeyPattern::extractShardKeyFromDoc. However, it will not check for arrays in shard key, as
 * it is used only for documents that are already persisted in a sharded collection
 */
std::unique_ptr<sbe::EExpression> makeShardKeyFunctionForPersistedDocuments(
    const std::vector<sbe::MatchPath>& shardKeyPaths,
    const std::vector<bool>& shardKeyHashed,
    const PlanStageSlots& slots);

SbStage makeProject(SbStage stage, sbe::SlotExprPairVector projects, PlanNodeId nodeId);

template <typename... Ts>
SbStage makeProject(SbStage stage, PlanNodeId nodeId, Ts&&... pack) {
    return makeProject(
        std::move(stage), sbe::makeSlotExprPairVec(std::forward<Ts>(pack)...), nodeId);
}

SbStage makeHashAgg(SbStage stage,
                    const sbe::value::SlotVector& gbs,
                    sbe::AggExprVector aggs,
                    boost::optional<sbe::value::SlotId> collatorSlot,
                    bool allowDiskUse,
                    sbe::SlotExprPairVector mergingExprs,
                    PlanNodeId planNodeId);

std::unique_ptr<sbe::EExpression> makeIf(std::unique_ptr<sbe::EExpression> condExpr,
                                         std::unique_ptr<sbe::EExpression> thenExpr,
                                         std::unique_ptr<sbe::EExpression> elseExpr);

std::unique_ptr<sbe::EExpression> makeLet(sbe::FrameId frameId,
                                          sbe::EExpression::Vector bindExprs,
                                          std::unique_ptr<sbe::EExpression> expr);

std::unique_ptr<sbe::EExpression> makeLocalLambda(sbe::FrameId frameId,
                                                  std::unique_ptr<sbe::EExpression> expr);

std::unique_ptr<sbe::EExpression> makeNumericConvert(std::unique_ptr<sbe::EExpression> expr,
                                                     sbe::value::TypeTags tag);

/**
 * Creates a chain of EIf expressions that will inspect each arg in order and return the first
 * arg that is not null or missing.
 */
std::unique_ptr<sbe::EExpression> makeIfNullExpr(sbe::EExpression::Vector values,
                                                 sbe::value::FrameIdGenerator* frameIdGenerator);

/** This helper takes an SBE SlotIdGenerator and an SBE Array and returns an output slot and a
 * unwind/project/limit/coscan subtree that streams out the elements of the array one at a time via
 * the output slot over a series of calls to getNext(), mimicking the output of a collection scan or
 * an index scan. Note that this method assumes ownership of the SBE Array being passed in.
 */
std::pair<sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>> generateVirtualScan(
    sbe::value::SlotIdGenerator* slotIdGenerator,
    sbe::value::TypeTags arrTag,
    sbe::value::Value arrVal,
    PlanYieldPolicy* yieldPolicy = nullptr);

/**
 * Make a mock scan with multiple output slots from an BSON array. This method does NOT assume
 * ownership of the BSONArray passed in.
 */
std::pair<sbe::value::SlotVector, std::unique_ptr<sbe::PlanStage>> generateVirtualScanMulti(
    sbe::value::SlotIdGenerator* slotIdGenerator,
    int numSlots,
    sbe::value::TypeTags arrTag,
    sbe::value::Value arrVal,
    PlanYieldPolicy* yieldPolicy = nullptr);

/**
 * Helper functions for converting from BSONObj/BSONArray to SBE Object/Array. Caller owns the SBE
 * Object/Array returned. These helper functions do not assume ownership of the BSONObj/BSONArray.
 */
std::pair<sbe::value::TypeTags, sbe::value::Value> makeValue(const BSONObj& bo);
std::pair<sbe::value::TypeTags, sbe::value::Value> makeValue(const BSONArray& ba);

/**
 * Returns a BSON type mask of all data types coercible to date.
 */
uint32_t dateTypeMask();

/**
 * Constructs local binding with inner expression built by 'innerExprFunc' and variables assigned
 * to expressions from 'bindings'.
 * Example usage:
 *
 * makeLocalBind(
 *   _context->frameIdGenerator,
 *   [](sbe::EVariable inputArray, sbe::EVariable index) {
 *     return <expression using inputArray and index>;
 *   },
 *   <expression to assign to inputArray>,
 *   <expression to assign to index>
 * );
 */
template <typename... Bindings,
          typename InnerExprFunc,
          typename = std::enable_if_t<
              std::conjunction_v<std::is_same<std::unique_ptr<sbe::EExpression>, Bindings>...>>>
std::unique_ptr<sbe::EExpression> makeLocalBind(sbe::value::FrameIdGenerator* frameIdGenerator,
                                                InnerExprFunc innerExprFunc,
                                                Bindings... bindings) {
    auto frameId = frameIdGenerator->generate();
    auto binds = sbe::makeEs();
    binds.reserve(sizeof...(Bindings));
    sbe::value::SlotId lastIndex = 0;
    auto convertToVariable = [&](std::unique_ptr<sbe::EExpression> expr) {
        binds.emplace_back(std::move(expr));
        auto currentIndex = lastIndex;
        lastIndex++;
        return sbe::EVariable{frameId, currentIndex};
    };
    auto innerExpr = innerExprFunc(convertToVariable(std::move(bindings))...);
    return sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(innerExpr));
}

std::unique_ptr<sbe::PlanStage> makeLoopJoinForFetch(std::unique_ptr<sbe::PlanStage> inputStage,
                                                     TypedSlot resultSlot,
                                                     TypedSlot recordIdSlot,
                                                     std::vector<std::string> fields,
                                                     sbe::value::SlotVector fieldSlots,
                                                     TypedSlot seekRecordIdSlot,
                                                     TypedSlot snapshotIdSlot,
                                                     TypedSlot indexIdentSlot,
                                                     TypedSlot indexKeySlot,
                                                     TypedSlot indexKeyPatternSlot,
                                                     const CollectionPtr& collToFetch,
                                                     PlanNodeId planNodeId,
                                                     sbe::value::SlotVector slotsToForward);

/**
 * Given an index key pattern, and a subset of the fields of the index key pattern that are depended
 * on to compute the query, returns the corresponding 'IndexKeysInclusionSet' bit vector and field
 * name vector.
 *
 * For example, suppose that we have an index key pattern {d: 1, c: 1, b: 1, a: 1}, and the caller
 * depends on the set of 'requiredFields' {"b", "d"}. In this case, the pair of return values would
 * be:
 *  - 'IndexKeysInclusionSet' bit vector of 1010
 *  - Field name vector of <"d", "b">
 */
template <typename T>
std::pair<sbe::IndexKeysInclusionSet, std::vector<std::string>> makeIndexKeyInclusionSet(
    const BSONObj& indexKeyPattern, const T& requiredFields) {
    sbe::IndexKeysInclusionSet indexKeyBitset;
    std::vector<std::string> keyFieldNames;
    size_t i = 0;
    for (auto&& elt : indexKeyPattern) {
        if (requiredFields.count(elt.fieldName())) {
            indexKeyBitset.set(i);
            keyFieldNames.push_back(elt.fieldName());
        }

        ++i;
    }

    return {std::move(indexKeyBitset), std::move(keyFieldNames)};
}

/**
 * A tree of nodes arranged based on field path. PathTreeNode can be used to represent index key
 * patterns, projections, etc. A PathTreeNode can also optionally hold a value of type T.
 *
 * For example, the key pattern {a.b: 1, x: 1, a.c: 1} in tree form would look like:
 *
 *         <root>
 *         /   |
 *        a    x
 *       / \
 *      b   c
 */
template <typename T>
struct PathTreeNode {
    PathTreeNode() = default;
    explicit PathTreeNode(std::string name) : name(std::move(name)) {}

    // Aside from the root node, it is very common for a node to have no children or only 1 child.
    using ChildrenVector = absl::InlinedVector<std::unique_ptr<PathTreeNode<T>>, 1>;

    // It is the caller's responsibility to verify that there is not an existing field with the
    // same name as 'fieldComponent'.
    PathTreeNode<T>* emplace_back(std::string fieldComponent) {
        auto newNode = std::make_unique<PathTreeNode<T>>(std::move(fieldComponent));
        const auto newNodeRaw = newNode.get();
        children.emplace_back(std::move(newNode));

        if (childrenMap) {
            childrenMap->emplace(newNodeRaw->name, newNodeRaw);
        } else if (children.size() >= 3) {
            // If 'childrenMap' is null and there are 3 or more children, build 'childrenMap' now.
            buildChildrenMap();
        }

        return newNodeRaw;
    }

    bool isLeaf() const {
        return children.empty();
    }

    PathTreeNode<T>* findChild(StringData fieldComponent) {
        if (childrenMap) {
            auto it = childrenMap->find(fieldComponent);
            return it != childrenMap->end() ? it->second : nullptr;
        }
        for (auto&& child : children) {
            if (child->name == fieldComponent) {
                return child.get();
            }
        }
        return nullptr;
    }

    PathTreeNode<T>* findNode(const sbe::MatchPath& fieldRef, size_t currentIndex = 0) {
        if (currentIndex == fieldRef.numParts()) {
            return this;
        }

        auto currentPart = fieldRef.getPart(currentIndex);
        if (auto child = findChild(currentPart)) {
            return child->findNode(fieldRef, currentIndex + 1);
        } else {
            return nullptr;
        }
    }

    /**
     * Returns leaf node matching field path. If the field path provided resolves to a non-leaf
     * node, null will be returned. For example, if tree was built for key pattern {a.b: 1}, this
     * method will return nullptr for field path "a".
     */
    PathTreeNode<T>* findLeafNode(const sbe::MatchPath& fieldRef, size_t currentIndex = 0) {
        auto* node = findNode(fieldRef, currentIndex);
        return (node && node->isLeaf() ? node : nullptr);
    }

    void clearChildren() {
        children.clear();
        childrenMap.reset();
    }

    void buildChildrenMap() {
        if (!childrenMap) {
            childrenMap = std::make_unique<StringDataMap<PathTreeNode<T>*>>();
            for (auto&& child : children) {
                childrenMap->insert_or_assign(child->name, child.get());
            }
        }
    }

    std::string name;

    ChildrenVector children;

    // We only build a hash map when there are 3 or more children. The vast majority of nodes
    // will have 2 children or less, so we dynamically allocate 'childrenMap' to save space.
    std::unique_ptr<StringDataMap<PathTreeNode<T>*>> childrenMap;

    T value = {};
};

using SlotTreeNode = PathTreeNode<boost::optional<sbe::value::SlotId>>;

std::unique_ptr<SlotTreeNode> buildKeyPatternTree(const BSONObj& keyPattern,
                                                  const sbe::value::SlotVector& slots);

std::unique_ptr<sbe::EExpression> buildNewObjExpr(const SlotTreeNode* slotTree);

std::unique_ptr<sbe::PlanStage> rehydrateIndexKey(std::unique_ptr<sbe::PlanStage> stage,
                                                  const BSONObj& indexKeyPattern,
                                                  PlanNodeId nodeId,
                                                  const sbe::value::SlotVector& indexKeySlots,
                                                  sbe::value::SlotId resultSlot);

template <typename T>
inline const char* getRawStringData(const T& str) {
    if constexpr (std::is_same_v<T, StringData>) {
        return str.rawData();
    } else {
        return str.data();
    }
}

enum class BuildPathTreeMode {
    AllowConflictingPaths,
    RemoveConflictingPaths,
    AssertNoConflictingPaths,
};

template <typename T, typename IterT, typename StringT>
inline std::unique_ptr<PathTreeNode<T>> buildPathTreeImpl(const std::vector<StringT>& paths,
                                                          boost::optional<IterT> valsBegin,
                                                          boost::optional<IterT> valsEnd,
                                                          BuildPathTreeMode mode) {
    auto tree = std::make_unique<PathTreeNode<T>>();
    auto valsIt = std::move(valsBegin);

    for (auto&& pathStr : paths) {
        auto path = sbe::MatchPath{pathStr};

        size_t numParts = path.numParts();
        size_t i = 0;

        auto* node = tree.get();
        StringData part;
        for (; i < numParts; ++i) {
            part = path.getPart(i);
            auto child = node->findChild(part);
            if (!child) {
                break;
            }
            node = child;
        }

        if (mode == BuildPathTreeMode::AssertNoConflictingPaths) {
            // If mode == AssertNoConflictingPaths, assert that no conflicting paths exist as
            // we build the path tree.
            tassert(7580701,
                    "Expected 'paths' to not contain any conflicting paths",
                    (node->isLeaf() && i == 0) || (!node->isLeaf() && i < numParts));
        } else if (mode == BuildPathTreeMode::RemoveConflictingPaths) {
            if (node->isLeaf() && i != 0) {
                // If 'mode == RemoveConflictingPaths' and we're about to process a path P and we've
                // already processed some other path that is a prefix of P, then ignore P.
                if (valsIt) {
                    ++(*valsIt);
                }

                continue;
            } else if (!node->isLeaf() && i == numParts) {
                // If 'mode == RemoveConflictingPaths' and we're about to process a path P that is a
                // prefix of another path that's already been processed, then delete the children of
                // 'node' to remove the longer conflicting path(s).
                node->clearChildren();
            }
        }

        if (i < numParts) {
            node = node->emplace_back(std::string(part));
            for (++i; i < numParts; ++i) {
                node = node->emplace_back(std::string(path.getPart(i)));
            }
        }

        if (valsIt) {
            tassert(7182003,
                    "Did not expect iterator 'valsIt' to reach the end yet",
                    !valsEnd || *valsIt != *valsEnd);

            node->value = **valsIt;

            ++(*valsIt);
        }
    }

    return tree;
}

/**
 * Builds a path tree from a set of paths and returns the root node of the tree.
 *
 * If 'mode == AllowConflictingPaths', this function will build a tree that contains all paths
 * specified in 'paths' (regardless of whether there are any paths that conflict).
 *
 * If 'mode == RemoveConflictingPaths', when there are two conflicting paths (ex. "a" and "a.b")
 * the conflict is resolved by removing the longer path ("a.b") and keeping the shorter path ("a").
 *
 * If 'mode == AssertNoConflictingPaths', this function will tassert() if it encounters any
 * conflicting paths.
 */
template <typename T, typename StringT>
std::unique_ptr<PathTreeNode<T>> buildPathTree(const std::vector<StringT>& paths,
                                               BuildPathTreeMode mode) {
    return buildPathTreeImpl<T, std::move_iterator<typename std::vector<T>::iterator>, StringT>(
        paths, boost::none, boost::none, mode);
}

/**
 * Builds a path tree from a set of paths, assigns a sequence of values to the sequence of nodes
 * corresponding to each path, and returns the root node of the tree.
 *
 * The 'values' sequence and the 'paths' vector are expected to have the same number of elements.
 * The kth value in the 'values' sequence will be assigned to the node corresponding to the kth path
 * in 'paths'.
 *
 * If 'mode == AllowConflictingPaths', this function will build a tree that contains all paths
 * specified in 'paths' (regardless of whether there are any paths that conflict).
 *
 * If 'mode == RemoveConflictingPaths', when there are two conflicting paths (ex. "a" and "a.b")
 * the conflict is resolved by removing the longer path ("a.b") and keeping the shorter path ("a").
 *
 * If 'mode == AssertNoConflictingPaths', this function will tassert() if it encounters any
 * conflicting paths.
 */
template <typename T, typename IterT, typename StringT>
std::unique_ptr<PathTreeNode<T>> buildPathTree(const std::vector<StringT>& paths,
                                               IterT valuesBegin,
                                               IterT valuesEnd,
                                               BuildPathTreeMode mode) {
    return buildPathTreeImpl<T, IterT, StringT>(
        paths, std::move(valuesBegin), std::move(valuesEnd), mode);
}

template <typename T, typename U>
std::unique_ptr<PathTreeNode<T>> buildPathTree(const std::vector<std::string>& paths,
                                               const std::vector<U>& values,
                                               BuildPathTreeMode mode) {
    tassert(7182004,
            "buildPathTree() expects 'paths' and 'values' to be the same size",
            paths.size() == values.size());

    return buildPathTree<T>(paths, values.begin(), values.end(), mode);
}

template <typename T, typename U>
std::unique_ptr<PathTreeNode<T>> buildPathTree(const std::vector<std::string>& paths,
                                               std::vector<U>&& values,
                                               BuildPathTreeMode mode) {
    tassert(7182005,
            "buildPathTree() expects 'paths' and 'values' to be the same size",
            paths.size() == values.size());

    return buildPathTree<T>(paths,
                            std::make_move_iterator(values.begin()),
                            std::make_move_iterator(values.end()),
                            mode);
}

/**
 * If a boolean can be constructed from type T, this function will construct a boolean from 'value'
 * and then return the negation. If a boolean cannot be constructed from type T, then this function
 * returns false.
 */
template <typename T>
bool convertsToFalse(const T& value) {
    if constexpr (std::is_constructible_v<bool, T>) {
        return !bool(value);
    } else {
        return false;
    }
}

template <typename T>
struct InvokeAndReturnBoolHelper {
    template <typename FuncT, typename... Args>
    static bool invoke(FuncT&& fn, bool defaultReturnValue, Args&&... args) {
        (std::forward<FuncT>(fn))(std::forward<Args>(args)...);
        return defaultReturnValue;
    }
};
template <>
struct InvokeAndReturnBoolHelper<bool> {
    template <typename FuncT, typename... Args>
    static bool invoke(FuncT&& fn, bool, Args&&... args) {
        return (std::forward<FuncT>(fn))(std::forward<Args>(args)...);
    }
};

/**
 * This function will invoke an invocable object ('fn') with the specified arguments ('args'). If
 * 'fn' returns bool, this function will return fn's return value. If 'fn' returns void or some type
 * other than bool, this function will return 'defaultReturnValue'.
 *
 * As a special case, this function alows fn's type (FuncT) to be nullptr_t. In such cases, this
 * function will do nothing and it will return 'defaultReturnValue'.
 *
 * If 'fn' is not invocable with the specified arguments and fn's type is not nullptr_t, this
 * function will raise a static assertion.
 *
 * Note that when a bool can be constructed from 'fn' (for example, if FuncT is a function pointer
 * type), this method will always invoke 'fn' regardless of whether "!bool(fn)" is true or false.
 * It is the caller's responsibility to do any necessary checks (ex. null checks) before calling
 * this function.
 */
template <typename FuncT, typename... Args>
inline bool invokeAndReturnBool(FuncT&& fn, bool defaultReturnValue, Args&&... args) {
    if constexpr (std::is_invocable_v<FuncT, Args...>) {
        return InvokeAndReturnBoolHelper<typename std::invoke_result<FuncT, Args...>::type>::invoke(
            std::forward<FuncT>(fn), defaultReturnValue, std::forward<Args>(args)...);
    } else {
        static_assert(std::is_null_pointer_v<std::remove_reference_t<FuncT>>);
        return defaultReturnValue;
    }
}

/**
 * This is a helper function used by visitPathTreeNodes() to invoke preVisit and postVisit callback
 * functions. This helper function will check if 'fn' supports invocation with the following args:
 *   (1) Node* node, const std::string& path, const DfsState& dfsState
 *   (2) Node* node, const DfsState& dfsState
 *   (3) Node* node, const std::string& path
 *   (4) Node* node
 *
 * After checking what 'fn' supports, this helper function will then use invokeAndReturnBool() to
 * invoke 'fn' accordingly and it will return invokeAndReturnBool()'s return value. If 'fn' supports
 * multiple signatures, whichever signature that appears first in the list above will be used.
 */
template <typename NodeT, typename FuncT>
inline bool invokeVisitPathTreeNodesCallback(
    FuncT&& fn,
    NodeT* node,
    const std::string& path,
    const std::vector<std::pair<NodeT*, size_t>>& dfsState) {
    using DfsState = std::vector<std::pair<NodeT*, size_t>>;

    if constexpr (std::is_invocable_v<FuncT, NodeT*, const std::string&, const DfsState&>) {
        return invokeAndReturnBool(std::forward<FuncT>(fn), true, node, path, dfsState);
    } else if constexpr (std::is_invocable_v<FuncT, NodeT*, const DfsState&>) {
        return invokeAndReturnBool(std::forward<FuncT>(fn), true, node, dfsState);
    } else if constexpr (std::is_invocable_v<FuncT, NodeT*, const std::string&>) {
        return invokeAndReturnBool(std::forward<FuncT>(fn), true, node, path);
    } else {
        return invokeAndReturnBool(std::forward<FuncT>(fn), true, node);
    }
}

/**
 * This function performs a DFS traversal on a path tree (as given by 'treeRoot') and it invokes
 * the specified preVisit and postVisit callbacks at the appropriate times.
 *
 * The caller may pass nullptr for 'preVisit' if they do not wish to perform any pre-visit actions,
 * and likewise the caller may pass nullptr for 'postVisit' if they do not wish to perform any
 * post-visit actions.
 *
 * Assuming 'preVisit' is not null, the 'preVisit' callback must support one of the following
 * signatures:
 *   (1) Node* node, const std::string& path, const DfsState& dfsState
 *   (2) Node* node, const DfsState& dfsState
 *   (3) Node* node, const std::string& path
 *   (4) Node* node
 *
 * Likewise, assuming 'postVisit' is not null, the 'postVisit' callback must support one of the
 * signatures listed above. For details, see invokeVisitPathTreeNodesCallback().
 *
 * The 'preVisit' callback can return any type. If preVisit's return type is not 'bool', its return
 * value will be ignored at run time. If preVisit's return type _is_ 'bool', then its return value
 * at run time will be used to decide whether the current node should be "skipped". If preVisit()
 * returns false, then the current node will be "skipped", its descendents will not be visited (i.e.
 * instead of the DFS descending, it will backtrack), and the 'postVisit' will not be called for the
 * node.
 *
 * The 'postVisit' callback can return any type. postVisit's return value (if any) is ignored.
 *
 * If 'invokeCallbacksForRootNode' is false (which is the default), the preVisit and postVisit
 * callbacks won't be invoked for the root node of the tree. If 'invokeCallbacksForRootNode' is
 * true, the preVisit and postVisit callbacks will be invoked for the root node of the tree at
 * the appropriate times.
 *
 * The 'rootPath' parameter allows the caller to specify the absolute path of 'treeRoot', which
 * will be used as the base/prefix to determine the paths of all the other nodes in the tree. If
 * no 'rootPath' argument is provided, then 'rootPath' defaults to boost::none.
 */
template <typename T, typename PreVisitFn, typename PostVisitFn>
void visitPathTreeNodes(PathTreeNode<T>* treeRoot,
                        const PreVisitFn& preVisit,
                        const PostVisitFn& postVisit,
                        bool invokeCallbacksForRootNode = false,
                        boost::optional<std::string> rootPath = boost::none) {
    using Node = PathTreeNode<T>;
    using DfsState = std::vector<std::pair<Node*, size_t>>;
    constexpr bool isPathNeeded =
        std::is_invocable_v<PreVisitFn, Node*, const std::string&, const DfsState&> ||
        std::is_invocable_v<PreVisitFn, Node*, const std::string&> ||
        std::is_invocable_v<PostVisitFn, Node*, const std::string&, const DfsState&> ||
        std::is_invocable_v<PostVisitFn, Node*, const std::string&>;

    if (!treeRoot || (treeRoot->children.empty() && !invokeCallbacksForRootNode)) {
        return;
    }

    const bool hasPreVisit = !convertsToFalse(preVisit);
    const bool hasPostVisit = !convertsToFalse(postVisit);

    // Perform a depth-first traversal using 'dfs' to keep track of where we are.
    DfsState dfs;
    dfs.emplace_back(treeRoot, std::numeric_limits<size_t>::max());
    boost::optional<std::string> path = std::move(rootPath);
    const std::string emptyPath;

    auto getPath = [&]() -> const std::string& {
        return path ? *path : emptyPath;
    };
    auto dfsPop = [&] {
        dfs.pop_back();
        if (isPathNeeded && path) {
            if (auto pos = path->find_last_of('.'); pos != std::string::npos) {
                path->resize(pos);
            } else {
                path = boost::none;
            }
        }
    };

    if (hasPreVisit && invokeCallbacksForRootNode) {
        // Invoke the pre-visit callback on the root node if appropriate.
        if (!invokeVisitPathTreeNodesCallback(preVisit, treeRoot, getPath(), dfs)) {
            dfsPop();
        }
    }

    while (!dfs.empty()) {
        ++dfs.back().second;
        auto [node, idx] = dfs.back();
        const bool isRootNode = dfs.size() == 1;

        if (idx < node->children.size()) {
            auto child = node->children[idx].get();
            dfs.emplace_back(child, std::numeric_limits<size_t>::max());
            if (isPathNeeded) {
                if (path) {
                    path->append(1, '.');
                    *path += child->name;
                } else {
                    path = child->name;
                }
            }

            if (hasPreVisit) {
                // Invoke the pre-visit callback.
                if (!invokeVisitPathTreeNodesCallback(preVisit, child, getPath(), dfs)) {
                    dfsPop();
                }
            }
        } else {
            if (hasPostVisit && (invokeCallbacksForRootNode || !isRootNode)) {
                // Invoke the post-visit callback.
                invokeVisitPathTreeNodesCallback(postVisit, node, getPath(), dfs);
            }
            dfsPop();
        }
    }
}

/**
 * Simple tagged pointer to a projection AST node. This class provides some useful methods for
 * extracting information from the AST node.
 */
class ProjectNode {
public:
    using ASTNode = projection_ast::ASTNode;
    using BooleanConstantASTNode = projection_ast::BooleanConstantASTNode;
    using ExpressionASTNode = projection_ast::ExpressionASTNode;
    using ProjectionSliceASTNode = projection_ast::ProjectionSliceASTNode;

    enum class Type { kBool, kExpr, kSbExpr, kSlice };

    struct Bool {
        bool value;
    };
    struct Expr {
        Expression* expr;
    };
    using Slice = std::pair<int32_t, boost::optional<int32_t>>;

    using VariantType = std::variant<Bool, Expr, SbExpr, Slice>;

    struct Keep {};
    struct Drop {};

    ProjectNode() = default;

    ProjectNode(Keep) : _data(Bool{true}) {}
    ProjectNode(Drop) : _data(Bool{false}) {}
    ProjectNode(Expression* expr) : _data(Expr{expr}) {}
    ProjectNode(SbExpr sbExpr) : _data(std::move(sbExpr)) {}
    ProjectNode(Slice slice) : _data(slice) {}

    ProjectNode(const BooleanConstantASTNode* n) : _data(Bool{n->value()}) {}
    ProjectNode(const ExpressionASTNode* n) : _data(Expr{n->expressionRaw()}) {}
    ProjectNode(const ProjectionSliceASTNode* n) : _data(Slice{n->limit(), n->skip()}) {}

    ProjectNode clone() const {
        return visit(OverloadedVisitor{[](const Bool& b) {
                                           return b.value ? ProjectNode(Keep{})
                                                          : ProjectNode(Drop{});
                                       },
                                       [](const Expr& e) { return ProjectNode(e.expr); },
                                       [](const SbExpr& e) { return ProjectNode(e.clone()); },
                                       [](const Slice& s) {
                                           return ProjectNode(s);
                                       }},
                     _data);
    }

    Type type() const {
        return visit(OverloadedVisitor{[](const Bool&) { return Type::kBool; },
                                       [](const Expr&) { return Type::kExpr; },
                                       [](const SbExpr&) { return Type::kSbExpr; },
                                       [](const Slice&) {
                                           return Type::kSlice;
                                       }},
                     _data);
    }

    bool isBool() const {
        return type() == Type::kBool;
    }
    bool isExpr() const {
        return type() == Type::kExpr;
    }
    bool isSbExpr() const {
        return type() == Type::kSbExpr;
    }
    bool isSlice() const {
        return type() == Type::kSlice;
    }

    bool getBool() const {
        tassert(7580702, "getBool() expected type() to be kBool", isBool());
        return get<Bool>(_data).value;
    }
    Expression* getExpr() const {
        tassert(7580703, "getExpr() expected type() to be kExpr", isExpr());
        return get<Expr>(_data).expr;
    }
    SbExpr getSbExpr() const {
        tassert(7580715, "getSbExpr() expected type() to be kSbExpr", isSbExpr());
        return get<SbExpr>(_data).clone();
    }
    SbExpr extractSbExpr() {
        tassert(7580716, "getSbExpr() expected type() to be kSbExpr", isSbExpr());
        return std::move(get<SbExpr>(_data));
    }
    Slice getSlice() const {
        tassert(7580704, "getSlice() expected type() to be kSlice", isSlice());
        return get<Slice>(_data);
    }

    bool isKeep() const {
        return type() == Type::kBool && get<Bool>(_data).value == true;
    }
    bool isDrop() const {
        return type() == Type::kBool && get<Bool>(_data).value == false;
    }

private:
    VariantType _data{};
};

/**
 * This function converts from projection AST to a pair of vectors: a vector of field paths and a
 * vector of ProjectNodes.
 */
std::pair<std::vector<std::string>, std::vector<ProjectNode>> getProjectNodes(
    const projection_ast::Projection& projection);

/**
 * This method retrieves the values of the specified field paths ('fields') from 'resultSlot'
 * and stores the values into slots.
 *
 * This method returns a pair containing: (1) the updated SBE plan stage tree and; (2) a vector of
 * the slots ('outSlots') containing the field path values.
 *
 * The order of slots in 'outSlots' will match the order of field paths in 'fields'.
 */
std::pair<std::unique_ptr<sbe::PlanStage>, TypedSlotVector> projectFieldsToSlots(
    std::unique_ptr<sbe::PlanStage> stage,
    const std::vector<std::string>& fields,
    TypedSlot resultSlot,
    PlanNodeId nodeId,
    sbe::value::SlotIdGenerator* slotIdGenerator,
    StageBuilderState& state,
    const PlanStageSlots* slots = nullptr);

template <typename T>
inline StringData getTopLevelField(const T& path) {
    auto idx = path.find('.');
    return StringData(getRawStringData(path), idx != std::string::npos ? idx : path.size());
}

inline std::vector<std::string> getTopLevelFields(const std::vector<std::string>& setOfPaths) {
    StringDataSet topLevelFieldsSet;
    std::vector<std::string> topLevelFields;

    for (size_t i = 0; i < setOfPaths.size(); ++i) {
        auto& path = setOfPaths[i];
        auto field = getTopLevelField(path);

        auto [_, inserted] = topLevelFieldsSet.insert(field);
        if (inserted) {
            topLevelFields.emplace_back(field.toString());
        }
    }

    return topLevelFields;
}

template <typename T>
inline std::vector<std::string> getTopLevelFields(const T& setOfPaths) {
    std::vector<std::string> topLevelFields;
    StringSet topLevelFieldsSet;

    for (const auto& path : setOfPaths) {
        auto field = getTopLevelField(path);
        if (!topLevelFieldsSet.count(field)) {
            topLevelFields.emplace_back(std::string(field));
            topLevelFieldsSet.emplace(std::string(field));
        }
    }

    return topLevelFields;
}

template <typename T, typename FuncT>
inline std::vector<T> filterVector(std::vector<T> vec, FuncT fn) {
    std::vector<T> result;
    std::copy_if(std::make_move_iterator(vec.begin()),
                 std::make_move_iterator(vec.end()),
                 std::back_inserter(result),
                 fn);
    return result;
}

template <typename T, typename FuncT>
inline std::pair<std::vector<T>, std::vector<T>> splitVector(std::vector<T> vec, FuncT fn) {
    std::pair<std::vector<T>, std::vector<T>> result;
    for (size_t i = 0; i < vec.size(); ++i) {
        if (fn(vec[i])) {
            result.first.emplace_back(std::move(vec[i]));
        } else {
            result.second.emplace_back(std::move(vec[i]));
        }
    }
    return result;
}

template <typename T>
inline std::vector<T> appendVectorUnique(std::vector<T> lhs, std::vector<T> rhs) {
    if (!rhs.empty()) {
        auto valueSet = std::set<T>{lhs.begin(), lhs.end()};
        for (size_t i = 0; i < rhs.size(); ++i) {
            if (valueSet.emplace(rhs[i]).second) {
                lhs.emplace_back(std::move(rhs[i]));
            }
        }
    }
    return lhs;
}

inline std::pair<std::unique_ptr<key_string::Value>, std::unique_ptr<key_string::Value>>
makeKeyStringPair(const BSONObj& lowKey,
                  bool lowKeyInclusive,
                  const BSONObj& highKey,
                  bool highKeyInclusive,
                  key_string::Version version,
                  Ordering ordering,
                  bool forward) {
    // Note that 'makeKeyFromBSONKeyForSeek()' is intended to compute the "start" key for an
    // index scan. The logic for computing a "discriminator" for an "end" key is reversed, which
    // is why we use 'makeKeyStringFromBSONKey()' to manually specify the discriminator for the
    // end key.
    return {
        std::make_unique<key_string::Value>(IndexEntryComparison::makeKeyStringFromBSONKeyForSeek(
            lowKey, version, ordering, forward, lowKeyInclusive)),
        std::make_unique<key_string::Value>(IndexEntryComparison::makeKeyStringFromBSONKey(
            highKey,
            version,
            ordering,
            forward != highKeyInclusive ? key_string::Discriminator::kExclusiveBefore
                                        : key_string::Discriminator::kExclusiveAfter))};
}

/**
 * The 'ProjectionEffects' class is used to represented the "effects" that a projection (either
 * (a single projection or multiple projections combined together) has on the set of all possible
 * top-level field names.
 *
 * Conceptully, a ProjectionEffects object can be thought of as a field name/Effect map plus
 * a "default" Effect to be applied to all fields that are not present in the map.
 *
 * The four possible Effects modeled by this class are: Keep, Drop, Modify, and Create. The
 * "default" Effect may be set to Keep, Drop, or Modify, but it cannot be set to Create. The
 * names of each kind of Effect are solely for "descriptive" purposes - for the formal definition
 * of each kind of Effect, see the docblock above the 'Effect' enum.
 *
 * A ProjectionEffects object can be constructed from a projection, or it can be constructed
 * using a single FieldSet (a "keep" set), or it can be constructed using 3 FieldSets (a
 * "allowed" set, a "modifiedOrCreated" set, and a "created" set).
 *
 * Two ProjectionEffects objects can also be combined together using the merge() method (to
 * merge two ProjectionEffects) or the compose() method (to "compose" a parent ProjectionEffects
 * and a child ProjectionEffects).
 */
class ProjectionEffects {
public:
    /**
     * Here is a Venn diagram showing what possible changes to a value are permitted for each
     * type of Effect:
     *
     *   +-----------------------+
     *   | Create                |
     *   | +-------------------+ |
     *   | | Modify            | |
     *   | | +------+ +------+ | |
     *   | | | Drop | | Keep | | |
     *   | | +------+ +------+ | |
     *   | +-------------------+ |
     *   +-----------------------+
     *
     * The "Keep" Effect is only allowed to return the input unmodified, and the "Drop" Effect is
     * only allowed to return Nothing. Thus, Keep's set contains only 1 possibility and Drop's set
     * contains only 1 possibility and the two sets do not overlap.
     *
     * The "Modify" Effect is allowed to return the input unmodified, or return Nothing, or make
     * any change to the input provided that when the input is Nothing it gets returned unmodified.
     * Modify's set is therefore a superset of Keep's set and Drop's set.
     *
     * The "Create" Effect is allowed to make any change to the input without any restrictions.
     * Specifically, when the input is Nothing, "Create" is allowed to return a non-Nothing value.
     * Therefore Create's set is a superset of Keep's set, Drop's set, and Modify's set.
     *
     * Note that the Keep, Drop, and Modify Effects cannot cause a field's position within the
     * object to change. (Drop makes the field disappear, but that technically doesn't count as
     * "changing" the field's position.)
     *
     * The Create effect is only Effect that can cause a field's position within the object to
     * change. (A single projection isn't capable of causinig a field's position within the object
     * o change, but multiple projections combined together are able to do this.)
     */
    enum Effect : int { kKeep, kDrop, kModify, kCreate };

    /**
     * Creates a ProjectionEffects that has a Keep Effect for all fields.
     */
    ProjectionEffects() = default;

    /**
     * Creates a ProjectionEffects from a projection as specified by 'isInclusion', 'paths',
     * and 'nodes'. 'isInclusion' indicates whether the projection is an inclusion or an
     * exclusion. 'paths' and 'nodes' are parallel vectors (one a vector of strings, the
     * other a vector of ProjectNodes) that specify the actions performed by the projection
     * on each path.
     *
     * The ProjectionEffects class models projections using the following rules:
     *
     * 1) For a given top-level field F, if the projection has a "computed" field on a path that
     *    F is a prefix of (or if $addFields created a field on a path that F is a prefix of),
     *    then the Effect for field F is 'Create'.
     * 2) Otherwise, if the projection has at least one keep or drop on a dotted path that is
     *    a prefix of a given top-level field F, then the Effect for field F is 'Modify'.
     * 3) Otherwise, if the projection has at least one $slice operation on any path (top-level
     *    or not) that is a prefix of a given top-level field F, then the Effect for field F
     *    is 'Modify'.
     * 4) Otherwise, if the none of the other rules apply for a given top-level field F, then
     *    the Effect for field F will either be 'Drop' (if 'isInclusion' is true) or 'Keep'
     *    (if 'isInclusion' is false).
     */
    ProjectionEffects(bool isInclusion,
                      const std::vector<std::string>& paths,
                      const std::vector<ProjectNode>& nodes);

    /**
     * Creates a ProjectionEffects representing a projection that has a Keep Effect for all the
     * fields 'keepFieldSet' and that has a Drop Effect for all other fields.
     */
    explicit ProjectionEffects(const FieldSet& keepFieldSet);

    /**
     * Creates a ProjectionEffects that has a Create Effect for fields in 'createdFieldSet',
     * that has a Modify Effect for fields in 'modifiedOrCreatedFieldSet' that are not present
     * in 'createdFieldSet', that has a Keep Effect for fields in 'allowedFieldSet' that are
     * not present in 'modifiedOrCreatedFieldSet' or 'createdFieldSet', and that has a Drop
     * Effect for all other fields.
     *
     * Note that 'createdFieldSet' must be a "closed" FieldSet.
     */
    ProjectionEffects(const FieldSet& allowedFieldSet,
                      const FieldSet& modifiedOrCreatedFieldSet,
                      const FieldSet& createdFieldSet = FieldSet::makeEmptySet(),
                      std::vector<std::string> displayOrder = {});

    /**
     * Same as 'ProjectionEffects(FieldSet, FieldSet, FieldSet, vector<string>)', except that
     * the second and third parameters are 'vector<string>' instead of 'FieldSet'.
     *
     * Note that the second and third parameters will be treated as "closed" field lists.
     */
    ProjectionEffects(const FieldSet& allowedFieldSet,
                      const std::vector<std::string>& modifiedOrCreatedFields,
                      const std::vector<std::string>& createdFields = {},
                      std::vector<std::string> displayOrder = {});

    /**
     * For each field, merge() will compute the union of the each child's Effect on the field.
     *
     * The merge() operation is typically used when a stage has 2 or more children and we want
     * to compute the combined effect of all of the stage's children together (as if the output
     * documents coming from the children were all mixed together into a single stream).
     *
     * merge() is commutative and associative, and any ProjectionEffects object that is merged with
     * itself produces itself. The merge() operation does not have a "neutral element" value.
     */
    ProjectionEffects& merge(const ProjectionEffects& other);

    /**
     * The compose() operation models the combined effects you would get if you had one projection
     * (parent) on top of another (child).
     *
     * compose() takes two ProjectionEffects as input that represent the effects in isolation of the
     * parent projection and the child projection. The output of compose() is computed using the
     * following table:
     *
     *   Parent's Effect | Child's Effect | Composed Effect
     *   ----------------+----------------+----------------
     *   Create          | Any effect     | Create
     *   Drop            | Any effect     | Drop
     *   Keep or Modify  | Create         | Create
     *   Keep or Modify  | Drop           | Drop
     *   Keep or Modify  | Modify         | Modify
     *   Modify          | Keep           | Modify
     *   Keep            | Keep           | Keep
     *
     * compose() is associative (but not commutative), and any ProjectionEffects object that is
     * composed with itself produces itself. The default ProjectionEffects object (no effects with
     * _defaultEffect == kKeep) behaves as the "neutral element" for the compose() operation.
     *
     * It's also worth noting that compose() is distributive over merge(), and furthermore that
     * for any 4 given ProjectionEffects objects (A,B,C,D) the following relationship will always
     * hold:
     *    (A*B)+(C*D) == (A+C)*(B+D)                    (where '+' is merge and '*' is compose)
     */
    ProjectionEffects& compose(const ProjectionEffects& child);

    /**
     * Returns the list of fields whose Effect is not equal to the "default" Effect.
     */
    const std::vector<std::string>& getFieldList() const {
        return _fields;
    }

    /**
     * Returns the "default" Effect.
     */
    Effect getDefaultEffect() const {
        return _defaultEffect;
    }

    /**
     * Returns the Effect for the specified field.
     */
    Effect get(StringData field) const {
        auto it = _effects.find(field);
        return it != _effects.end() ? it->second : _defaultEffect;
    }

    inline bool isKeep(StringData field) const {
        return get(field) == kKeep;
    }
    inline bool isDrop(StringData field) const {
        return get(field) == kDrop;
    }
    inline bool isModify(StringData field) const {
        return get(field) == kModify;
    }
    inline bool isCreate(StringData field) const {
        return get(field) == kCreate;
    }

    /**
     * Returns true if 'effect == _defaultEffect' is true or if one of the values in the
     * '_effects' map is equal to 'effect'. Otherwise, returns false.
     */
    bool hasEffect(Effect effect) const {
        if (effect == _defaultEffect) {
            return true;
        }
        for (auto&& field : _fields) {
            if (effect == _effects.find(field)->second) {
                return true;
            }
        }
        return false;
    }

    /**
     * Returns a FieldSet containing all the fields whose effect is not kDrop.
     *
     * If there are a _finite_ number of fields whose effect is not kDrop, then this function will
     * return a "closed" FieldSet, otherwise it will return an "open" FieldSet.
     */
    FieldSet getAllowedFieldSet() const;

    /**
     * Returns a FieldSet containing all the fields whose effect is kModify or kCreate.
     *
     * If '_defaultEffect' is kKeep or kDrop (which is usually the case), then this function will
     * return a "closed" FieldSet.
     *
     * If '_defaultEffect' is kModify (which can happen if you merge two ProjectionEffects with
     * different defaultEffects), then this function will return an "open" FieldSet.
     */
    FieldSet getModifiedOrCreatedFieldSet() const;

    /**
     * Returns a FieldSet containing all the fields whose effect is kCreate.
     *
     * This function always returns a "closed" FieldSet.
     */
    FieldSet getCreatedFieldSet() const;

    std::string toString() const;

private:
    void removeRedundantEffects();

    std::vector<std::string> _fields;
    StringMap<Effect> _effects;
    Effect _defaultEffect = kKeep;
};

FieldSet makeAllowedFieldSet(bool isInclusion,
                             const std::vector<std::string>& paths,
                             const std::vector<ProjectNode>& nodes);

FieldSet makeModifiedOrCreatedFieldSet(bool isInclusion,
                                       const std::vector<std::string>& paths,
                                       const std::vector<ProjectNode>& nodes);

FieldSet makeCreatedFieldSet(bool isInclusion,
                             const std::vector<std::string>& paths,
                             const std::vector<ProjectNode>& nodes);

}  // namespace mongo::stage_builder
