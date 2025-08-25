/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/values/util.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"

namespace mongo {
namespace sbe {
namespace vm {
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAddToSet(ArityType arity) {
    auto [ownAgg, tagAgg, valAgg] = getFromStack(0);
    auto [tagField, valField] = moveOwnedFromStack(1);
    value::ValueGuard guardField{tagField, valField};

    // Create a new array is it does not exist yet.
    if (tagAgg == value::TypeTags::Nothing) {
        ownAgg = true;
        std::tie(tagAgg, valAgg) = value::makeNewArraySet();
    } else {
        // Take ownership of the accumulator.
        topStack(false, value::TypeTags::Nothing, 0);
    }
    value::ValueGuard guard{tagAgg, valAgg};

    invariant(ownAgg && tagAgg == value::TypeTags::ArraySet);
    auto arr = value::getArraySetView(valAgg);

    // Push back the value. Note that array will ignore Nothing.
    guardField.reset();
    arr->push_back(tagField, valField);

    guard.reset();
    return {ownAgg, tagAgg, valAgg};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAddToSetCapped(ArityType arity) {
    auto [tagAccumulatorState, valAccumulatorState] = moveOwnedFromStack(0);
    value::ValueGuard guardAccumulatorState{tagAccumulatorState, valAccumulatorState};

    auto [ownedNewElem, tagNewElem, valNewElem] = moveFromStack(1);
    value::ValueGuard guardNewElem{ownedNewElem, tagNewElem, valNewElem};

    auto [_ownedSizeCap, tagSizeCap, valSizeCap] = getFromStack(2);

    // Return the unmodified accumulator state when the size cap is malformed.
    if (tagSizeCap != value::TypeTags::NumberInt32) {
        guardAccumulatorState.reset();
        return {true, tagAccumulatorState, valAccumulatorState};
    }

    guardAccumulatorState.reset();
    guardNewElem.reset();
    return addToSetCappedImpl(tagAccumulatorState,
                              valAccumulatorState,
                              ownedNewElem,
                              tagNewElem,
                              valNewElem,
                              value::bitcastTo<int32_t>(valSizeCap),
                              nullptr /*collator*/);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinCollAddToSet(ArityType arity) {
    auto [ownAgg, tagAgg, valAgg] = getFromStack(0);
    auto [ownColl, tagColl, valColl] = getFromStack(1);
    auto [tagField, valField] = moveOwnedFromStack(2);
    value::ValueGuard guardField{tagField, valField};

    // If the collator is Nothing or if it's some unexpected type, don't push back the value
    // and just return the accumulator.
    if (tagColl != value::TypeTags::collator) {
        topStack(false, value::TypeTags::Nothing, 0);
        return {ownAgg, tagAgg, valAgg};
    }

    // Create a new array is it does not exist yet.
    if (tagAgg == value::TypeTags::Nothing) {
        ownAgg = true;
        std::tie(tagAgg, valAgg) = value::makeNewArraySet(value::getCollatorView(valColl));
    } else {
        // Take ownership of the accumulator.
        topStack(false, value::TypeTags::Nothing, 0);
    }
    value::ValueGuard guard{tagAgg, valAgg};

    invariant(ownAgg && tagAgg == value::TypeTags::ArraySet);
    auto arr = value::getArraySetView(valAgg);

    // Push back the value. Note that array will ignore Nothing.
    guardField.reset();
    arr->push_back(tagField, valField);

    guard.reset();
    return {ownAgg, tagAgg, valAgg};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinCollAddToSetCapped(
    ArityType arity) {
    auto [tagAccumulatorState, valAccumulatorState] = moveOwnedFromStack(0);
    value::ValueGuard guardAccumulatorState{tagAccumulatorState, valAccumulatorState};

    auto [_ownedCollator, tagCollator, valCollator] = getFromStack(1);

    auto [ownedNewElem, tagNewElem, valNewElem] = moveFromStack(2);
    value::ValueGuard guardNewElem{ownedNewElem, tagNewElem, valNewElem};

    auto [_ownedSizeCap, tagSizeCap, valSizeCap] = getFromStack(3);

    // Return the unmodified accumulator state when the collator or size cap is malformed.
    if (tagCollator != value::TypeTags::collator || tagSizeCap != value::TypeTags::NumberInt32) {
        guardAccumulatorState.reset();
        return {true, tagAccumulatorState, valAccumulatorState};
    }

    guardAccumulatorState.reset();
    guardNewElem.reset();
    return addToSetCappedImpl(tagAccumulatorState,
                              valAccumulatorState,
                              ownedNewElem,
                              tagNewElem,
                              valNewElem,
                              value::bitcastTo<int32_t>(valSizeCap),
                              value::getCollatorView(valCollator));
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinSetUnionCapped(ArityType arity) {
    auto [tagAccumulatorState, valAccumulatorState] = moveOwnedFromStack(0);
    value::ValueGuard guardAccumulatorState{tagAccumulatorState, valAccumulatorState};

    auto [tagNewSetMembers, valNewSetMembers] = moveOwnedFromStack(1);
    value::ValueGuard guardNewSetMembers{tagNewSetMembers, valNewSetMembers};

    auto [_, tagSizeCap, valSizeCap] = getFromStack(2);

    // Return the unmodified accumulator state when the size cap is malformed.
    if (tagSizeCap != value::TypeTags::NumberInt32) {
        guardAccumulatorState.reset();
        return {true, tagAccumulatorState, valAccumulatorState};
    }

    guardAccumulatorState.reset();
    guardNewSetMembers.reset();
    return setUnionAccumImpl(tagAccumulatorState,
                             valAccumulatorState,
                             tagNewSetMembers,
                             valNewSetMembers,
                             value::bitcastTo<int32_t>(valSizeCap),
                             nullptr /*collator*/);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinCollSetUnionCapped(
    ArityType arity) {
    auto [tagAccumulatorState, valAccumulatorState] = moveOwnedFromStack(0);
    value::ValueGuard guardAccumulatorState{tagAccumulatorState, valAccumulatorState};

    auto [_ownedCollator, tagCollator, valCollator] = getFromStack(1);

    auto [tagNewSetMembers, valNewSetMembers] = moveOwnedFromStack(2);
    value::ValueGuard guardNewSetMembers{tagNewSetMembers, valNewSetMembers};

    auto [_ownedSizeCap, tagSizeCap, valSizeCap] = getFromStack(3);

    // Return the unmodified accumulator state when the size cap or collator is malformed.
    if (tagCollator != value::TypeTags::collator || tagSizeCap != value::TypeTags::NumberInt32) {
        auto [arrOwned, arrTag, arrVal] = getFromStack(0);
        topStack(false, value::TypeTags::Nothing, 0);
        return {arrOwned, arrTag, arrVal};
    }

    guardAccumulatorState.reset();
    guardNewSetMembers.reset();
    return setUnionAccumImpl(tagAccumulatorState,
                             valAccumulatorState,
                             tagNewSetMembers,
                             valNewSetMembers,
                             value::bitcastTo<int32_t>(valSizeCap),
                             value::getCollatorView(valCollator));
}

namespace {
FastTuple<bool, value::TypeTags, value::Value> setUnion(
    const std::vector<value::TypeTags>& argTags,
    const std::vector<value::Value>& argVals,
    const CollatorInterface* collator = nullptr) {
    auto [resTag, resVal] = value::makeNewArraySet(collator);
    value::ValueGuard resGuard{resTag, resVal};
    auto resView = value::getArraySetView(resVal);

    for (size_t idx = 0; idx < argVals.size(); ++idx) {
        auto argTag = argTags[idx];
        auto argVal = argVals[idx];

        value::arrayForEach(argTag, argVal, [&](value::TypeTags elTag, value::Value elVal) {
            resView->push_back_clone(elTag, elVal);
        });
    }
    resGuard.reset();
    return {true, resTag, resVal};
}

FastTuple<bool, value::TypeTags, value::Value> setIntersection(
    const std::vector<value::TypeTags>& argTags,
    const std::vector<value::Value>& argVals,
    const CollatorInterface* collator = nullptr) {
    auto intersectionMap =
        value::ValueMapType<size_t>{0, value::ValueHash(collator), value::ValueEq(collator)};

    auto [resTag, resVal] = value::makeNewArraySet(collator);
    value::ValueGuard resGuard{resTag, resVal};

    for (size_t idx = 0; idx < argVals.size(); ++idx) {
        auto tag = argTags[idx];
        auto val = argVals[idx];

        bool atLeastOneCommonElement = false;
        value::arrayForEach(tag, val, [&](value::TypeTags elTag, value::Value elVal) {
            if (idx == 0) {
                intersectionMap[{elTag, elVal}] = 1;
            } else {
                if (auto it = intersectionMap.find({elTag, elVal}); it != intersectionMap.end()) {
                    if (it->second == idx) {
                        it->second++;
                        atLeastOneCommonElement = true;
                    }
                }
            }
        });

        if (idx > 0 && !atLeastOneCommonElement) {
            resGuard.reset();
            return {true, resTag, resVal};
        }
    }

    auto resView = value::getArraySetView(resVal);
    for (auto&& [item, counter] : intersectionMap) {
        if (counter == argVals.size()) {
            auto [elTag, elVal] = item;
            resView->push_back_clone(elTag, elVal);
        }
    }

    resGuard.reset();
    return {true, resTag, resVal};
}

/**
 * Helper function that creates a set useful to quickly search through a dataset and then it is
 * destroyed.
 */
value::ValueSetType valueToShallowSetHelper(value::TypeTags tag,
                                            value::Value value,
                                            const CollatorInterface* collator) {
    value::ValueSetType setValues(0, value::ValueHash(collator), value::ValueEq(collator));
    setValues.reserve(getArraySize(tag, value));
    value::arrayForEach(tag, value, [&](value::TypeTags elemTag, value::Value elemVal) {
        setValues.insert({elemTag, elemVal});
    });
    return setValues;
}

FastTuple<bool, value::TypeTags, value::Value> setDifference(
    value::TypeTags lhsTag,
    value::Value lhsVal,
    value::TypeTags rhsTag,
    value::Value rhsVal,
    const CollatorInterface* collator = nullptr) {
    auto [resTag, resVal] = value::makeNewArraySet(collator);
    value::ValueGuard resGuard{resTag, resVal};
    auto resView = value::getArraySetView(resVal);

    auto process =
        [&resView](value::TypeTags lhsTag, value::Value lhsVal, const value::ValueSetType& rhsSet) {
            value::arrayForEach(lhsTag, lhsVal, [&](value::TypeTags elTag, value::Value elVal) {
                if (rhsSet.count({elTag, elVal}) == 0) {
                    resView->push_back_clone(elTag, elVal);
                }
            });
        };
    if (rhsTag == value::TypeTags::ArraySet &&
        value::getArraySetView(rhsVal)->values().hash_function().getCollator() == collator) {
        process(lhsTag, lhsVal, value::getArraySetView(rhsVal)->values());
    } else {
        process(lhsTag, lhsVal, valueToShallowSetHelper(rhsTag, rhsVal, collator));
    }

    resGuard.reset();
    return {true, resTag, resVal};
}

FastTuple<bool, value::TypeTags, value::Value> setEquals(
    const std::vector<value::TypeTags>& argTags,
    const std::vector<value::Value>& argVals,
    const CollatorInterface* collator = nullptr) {
    auto setValuesFirstArg = valueToShallowSetHelper(argTags[0], argVals[0], collator);

    for (size_t idx = 1; idx < argVals.size(); ++idx) {
        bool matches = false;
        if (argTags[idx] == value::TypeTags::ArraySet &&
            value::getArraySetView(argVals[idx])->values().hash_function().getCollator() ==
                collator) {
            matches = setValuesFirstArg == value::getArraySetView(argVals[idx])->values();
        } else {
            matches =
                setValuesFirstArg == valueToShallowSetHelper(argTags[idx], argVals[idx], collator);
        }

        if (!matches) {
            return {false, value::TypeTags::Boolean, false};
        }
    }

    return {false, value::TypeTags::Boolean, true};
}

FastTuple<bool, value::TypeTags, value::Value> setIsSubset(
    value::TypeTags lhsTag,
    value::Value lhsVal,
    value::TypeTags rhsTag,
    value::Value rhsVal,
    const CollatorInterface* collator = nullptr) {

    if (!value::isArray(lhsTag) || !value::isArray(rhsTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    bool isSubset = true;
    auto process = [&isSubset](value::TypeTags lhsTag,
                               value::Value lhsVal,
                               const value::ValueSetType& rhsSet) {
        value::arrayAny(lhsTag, lhsVal, [&](value::TypeTags elTag, value::Value elVal) {
            isSubset = (rhsSet.count({elTag, elVal}) > 0);
            return !isSubset;
        });
    };

    if (rhsTag == value::TypeTags::ArraySet &&
        value::getArraySetView(rhsVal)->values().hash_function().getCollator() == collator) {
        process(lhsTag, lhsVal, value::getArraySetView(rhsVal)->values());
    } else {
        process(lhsTag, lhsVal, valueToShallowSetHelper(rhsTag, rhsVal, collator));
    }

    return {false, value::TypeTags::Boolean, isSubset};
}
}  // namespace

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinCollSetUnion(ArityType arity) {
    invariant(arity >= 1);

    auto [_, collTag, collVal] = getFromStack(0);
    if (collTag != value::TypeTags::collator) {
        return {false, value::TypeTags::Nothing, 0};
    }

    std::vector<value::TypeTags> argTags;
    std::vector<value::Value> argVals;
    for (size_t idx = 1; idx < arity; ++idx) {
        auto [owned, tag, val] = getFromStack(idx);
        if (!value::isArray(tag)) {
            return {false, value::TypeTags::Nothing, 0};
        }

        argTags.push_back(tag);
        argVals.push_back(val);
    }

    return setUnion(argTags, argVals, value::getCollatorView(collVal));
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinSetUnion(ArityType arity) {
    std::vector<value::TypeTags> argTags;
    std::vector<value::Value> argVals;

    for (size_t idx = 0; idx < arity; ++idx) {
        auto [_, tag, val] = getFromStack(idx);
        if (!value::isArray(tag)) {
            return {false, value::TypeTags::Nothing, 0};
        }

        argTags.push_back(tag);
        argVals.push_back(val);
    }

    return setUnion(argTags, argVals);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinCollSetIntersection(
    ArityType arity) {
    invariant(arity >= 1);

    auto [_, collTag, collVal] = getFromStack(0);
    if (collTag != value::TypeTags::collator) {
        return {false, value::TypeTags::Nothing, 0};
    }

    std::vector<value::TypeTags> argTags;
    std::vector<value::Value> argVals;

    for (size_t idx = 1; idx < arity; ++idx) {
        auto [owned, tag, val] = getFromStack(idx);
        if (!value::isArray(tag)) {
            return {false, value::TypeTags::Nothing, 0};
        }

        argTags.push_back(tag);
        argVals.push_back(val);
    }

    return setIntersection(argTags, argVals, value::getCollatorView(collVal));
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinSetIntersection(ArityType arity) {
    std::vector<value::TypeTags> argTags;
    std::vector<value::Value> argVals;

    for (size_t idx = 0; idx < arity; ++idx) {
        auto [_, tag, val] = getFromStack(idx);
        if (!value::isArray(tag)) {
            return {false, value::TypeTags::Nothing, 0};
        }

        argTags.push_back(tag);
        argVals.push_back(val);
    }

    return setIntersection(argTags, argVals);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinCollSetDifference(ArityType arity) {
    invariant(arity == 3);

    auto [_, collTag, collVal] = getFromStack(0);
    if (collTag != value::TypeTags::collator) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(1);
    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(2);

    if (!value::isArray(lhsTag) || !value::isArray(rhsTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    return setDifference(lhsTag, lhsVal, rhsTag, rhsVal, value::getCollatorView(collVal));
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinCollSetEquals(ArityType arity) {
    invariant(arity >= 3);

    auto [_, collTag, collVal] = getFromStack(0);
    if (collTag != value::TypeTags::collator) {
        return {false, value::TypeTags::Nothing, 0};
    }

    std::vector<value::TypeTags> argTags;
    std::vector<value::Value> argVals;

    for (size_t idx = 1; idx < arity; ++idx) {
        auto [owned, tag, val] = getFromStack(idx);
        if (!value::isArray(tag)) {
            return {false, value::TypeTags::Nothing, 0};
        }

        argTags.push_back(tag);
        argVals.push_back(val);
    }

    return setEquals(argTags, argVals, value::getCollatorView(collVal));
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinCollSetIsSubset(ArityType arity) {
    tassert(5154701, "$setIsSubset expects two sets and a collator", arity == 3);

    auto [_, collTag, collVal] = getFromStack(0);
    if (collTag != value::TypeTags::collator) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(1);
    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(2);

    return setIsSubset(lhsTag, lhsVal, rhsTag, rhsVal, value::getCollatorView(collVal));
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinSetDifference(ArityType arity) {
    invariant(arity == 2);

    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);
    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(1);

    if (!value::isArray(lhsTag) || !value::isArray(rhsTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    return setDifference(lhsTag, lhsVal, rhsTag, rhsVal);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinSetEquals(ArityType arity) {
    invariant(arity >= 2);

    std::vector<value::TypeTags> argTags;
    std::vector<value::Value> argVals;

    for (size_t idx = 0; idx < arity; ++idx) {
        auto [_, tag, val] = getFromStack(idx);
        if (!value::isArray(tag)) {
            return {false, value::TypeTags::Nothing, 0};
        }

        argTags.push_back(tag);
        argVals.push_back(val);
    }

    return setEquals(argTags, argVals);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinSetIsSubset(ArityType arity) {
    tassert(5154702, "$setIsSubset expects two sets", arity == 2);

    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);
    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(1);

    return setIsSubset(lhsTag, lhsVal, rhsTag, rhsVal);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinSetToArray(ArityType arity) {
    invariant(arity == 1);

    auto [owned, tag, val] = getFromStack(0);

    if (tag != value::TypeTags::ArraySet && tag != value::TypeTags::ArrayMultiSet) {
        // passthrough if its not a set
        topStack(false, value::TypeTags::Nothing, 0);
        return {owned, tag, val};
    }

    auto [resTag, resVal] = value::makeNewArray();
    value::ValueGuard resGuard{resTag, resVal};
    auto resView = value::getArrayView(resVal);

    value::arrayForEach(tag, val, [&](value::TypeTags elTag, value::Value elVal) {
        resView->push_back(value::copyValue(elTag, elVal));
    });

    resGuard.reset();
    return {true, resTag, resVal};
}

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
