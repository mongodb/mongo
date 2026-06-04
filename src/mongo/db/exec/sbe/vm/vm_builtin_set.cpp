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
value::TagValueMaybeOwned ByteCode::builtinAddToSet(ArityType arity) {
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

    tassert(11086805,
            "Unexpected type of Agg parameter",
            ownAgg && tagAgg == value::TypeTags::ArraySet);
    auto arr = value::getArraySetView(valAgg);

    // Push back the value. Note that array will ignore Nothing.
    arr->push_back_clone(tagField, valField);

    guard.reset();
    return {ownAgg, tagAgg, valAgg};
}

value::TagValueMaybeOwned ByteCode::builtinAddToSetCapped(ArityType arity) {
    auto accumulatorState = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));
    auto newElem = value::TagValueMaybeOwned::fromRaw(moveFromStack(1));

    auto sizeCap = viewFromStack(2);

    // Return the unmodified accumulator state when the size cap is malformed.
    if (sizeCap.tag != value::TypeTags::NumberInt32) {
        return std::move(accumulatorState);
    }

    return addToSetCappedImpl(std::move(accumulatorState),
                              std::move(newElem),
                              value::bitcastTo<int32_t>(sizeCap.value),
                              nullptr /*collator*/);
}

value::TagValueMaybeOwned ByteCode::builtinCollAddToSet(ArityType arity) {
    auto [ownAgg, tagAgg, valAgg] = getFromStack(0);
    auto collView = viewFromStack(1);
    auto [tagField, valField] = moveOwnedFromStack(2);
    value::ValueGuard guardField{tagField, valField};

    // If the collator is Nothing or if it's some unexpected type, don't push back the value
    // and just return the accumulator.
    if (collView.tag != value::TypeTags::collator) {
        topStack(false, value::TypeTags::Nothing, 0);
        return {ownAgg, tagAgg, valAgg};
    }

    // Create a new array is it does not exist yet.
    if (tagAgg == value::TypeTags::Nothing) {
        ownAgg = true;
        std::tie(tagAgg, valAgg) = value::makeNewArraySet(value::getCollatorView(collView.value));
    } else {
        // Take ownership of the accumulator.
        topStack(false, value::TypeTags::Nothing, 0);
    }
    value::ValueGuard guard{tagAgg, valAgg};


    tassert(11086804,
            "Unexpected type of Agg parameter",
            ownAgg && tagAgg == value::TypeTags::ArraySet);
    auto arr = value::getArraySetView(valAgg);

    // Push back the value. Note that array will ignore Nothing.
    arr->push_back_clone(tagField, valField);

    guard.reset();
    return {ownAgg, tagAgg, valAgg};
}

value::TagValueMaybeOwned ByteCode::builtinCollAddToSetCapped(ArityType arity) {
    auto accumulatorState = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));

    auto collatorView = viewFromStack(1);

    auto newElem = value::TagValueMaybeOwned::fromRaw(moveFromStack(2));

    auto sizeCap = viewFromStack(3);

    // Return the unmodified accumulator state when the collator or size cap is malformed.
    if (collatorView.tag != value::TypeTags::collator ||
        sizeCap.tag != value::TypeTags::NumberInt32) {
        return std::move(accumulatorState);
    }

    return addToSetCappedImpl(std::move(accumulatorState),
                              std::move(newElem),
                              value::bitcastTo<int32_t>(sizeCap.value),
                              value::getCollatorView(collatorView.value));
}

value::TagValueMaybeOwned ByteCode::builtinSetUnionCapped(ArityType arity) {
    auto accumulatorState = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));
    auto newSetMembers = value::TagValueOwned::fromRaw(moveOwnedFromStack(1));

    auto sizeCap = viewFromStack(2);

    // Return the unmodified accumulator state when the size cap is malformed.
    if (sizeCap.tag != value::TypeTags::NumberInt32) {
        return std::move(accumulatorState);
    }

    return setUnionAccumImpl(std::move(accumulatorState),
                             std::move(newSetMembers),
                             value::bitcastTo<int32_t>(sizeCap.value),
                             nullptr /*collator*/);
}

value::TagValueMaybeOwned ByteCode::builtinCollSetUnionCapped(ArityType arity) {
    auto accumulatorState = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));

    auto collatorView = viewFromStack(1);

    auto newSetMembers = value::TagValueOwned::fromRaw(moveOwnedFromStack(2));

    auto sizeCap = viewFromStack(3);

    // Return the unmodified accumulator state when the size cap or collator is malformed.
    if (collatorView.tag != value::TypeTags::collator ||
        sizeCap.tag != value::TypeTags::NumberInt32) {
        return std::move(accumulatorState);
    }

    return setUnionAccumImpl(std::move(accumulatorState),
                             std::move(newSetMembers),
                             value::bitcastTo<int32_t>(sizeCap.value),
                             value::getCollatorView(collatorView.value));
}

namespace {
value::TagValueMaybeOwned setUnion(const std::vector<value::TypeTags>& argTags,
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

value::TagValueMaybeOwned setIntersection(const std::vector<value::TypeTags>& argTags,
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

value::TagValueMaybeOwned setDifference(value::TypeTags lhsTag,
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

value::TagValueMaybeOwned setEquals(const std::vector<value::TypeTags>& argTags,
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

value::TagValueMaybeOwned setIsSubset(value::TypeTags lhsTag,
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

value::TagValueMaybeOwned ByteCode::builtinCollSetUnion(ArityType arity) {
    tassert(11080016, "Unexpected arity value", arity >= 1);

    auto collView = viewFromStack(0);
    if (collView.tag != value::TypeTags::collator) {
        return {false, value::TypeTags::Nothing, 0};
    }

    std::vector<value::TypeTags> argTags;
    std::vector<value::Value> argVals;
    for (size_t idx = 1; idx < arity; ++idx) {
        auto arg = viewFromStack(idx);
        if (!value::isArray(arg.tag)) {
            return {false, value::TypeTags::Nothing, 0};
        }

        argTags.push_back(arg.tag);
        argVals.push_back(arg.value);
    }

    return setUnion(argTags, argVals, value::getCollatorView(collView.value));
}

value::TagValueMaybeOwned ByteCode::builtinSetUnion(ArityType arity) {
    std::vector<value::TypeTags> argTags;
    std::vector<value::Value> argVals;

    for (size_t idx = 0; idx < arity; ++idx) {
        auto arg = viewFromStack(idx);
        if (!value::isArray(arg.tag)) {
            return {false, value::TypeTags::Nothing, 0};
        }

        argTags.push_back(arg.tag);
        argVals.push_back(arg.value);
    }

    return setUnion(argTags, argVals);
}

value::TagValueMaybeOwned ByteCode::builtinCollSetIntersection(ArityType arity) {
    tassert(11080015, "Unexpected arity value", arity >= 1);

    auto collView = viewFromStack(0);
    if (collView.tag != value::TypeTags::collator) {
        return {false, value::TypeTags::Nothing, 0};
    }

    std::vector<value::TypeTags> argTags;
    std::vector<value::Value> argVals;

    for (size_t idx = 1; idx < arity; ++idx) {
        auto arg = viewFromStack(idx);
        if (!value::isArray(arg.tag)) {
            return {false, value::TypeTags::Nothing, 0};
        }

        argTags.push_back(arg.tag);
        argVals.push_back(arg.value);
    }

    return setIntersection(argTags, argVals, value::getCollatorView(collView.value));
}

value::TagValueMaybeOwned ByteCode::builtinSetIntersection(ArityType arity) {
    std::vector<value::TypeTags> argTags;
    std::vector<value::Value> argVals;

    for (size_t idx = 0; idx < arity; ++idx) {
        auto arg = viewFromStack(idx);
        if (!value::isArray(arg.tag)) {
            return {false, value::TypeTags::Nothing, 0};
        }

        argTags.push_back(arg.tag);
        argVals.push_back(arg.value);
    }

    return setIntersection(argTags, argVals);
}

value::TagValueMaybeOwned ByteCode::builtinCollSetDifference(ArityType arity) {
    tassert(11080014, "Unexpected arity value", arity == 3);

    auto collView = viewFromStack(0);
    if (collView.tag != value::TypeTags::collator) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto lhs = viewFromStack(1);
    auto rhs = viewFromStack(2);

    if (!value::isArray(lhs.tag) || !value::isArray(rhs.tag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    return setDifference(
        lhs.tag, lhs.value, rhs.tag, rhs.value, value::getCollatorView(collView.value));
}

value::TagValueMaybeOwned ByteCode::builtinCollSetEquals(ArityType arity) {
    tassert(11080013, "Unexpected arity value", arity >= 3);

    auto collView = viewFromStack(0);
    if (collView.tag != value::TypeTags::collator) {
        return {false, value::TypeTags::Nothing, 0};
    }

    std::vector<value::TypeTags> argTags;
    std::vector<value::Value> argVals;

    for (size_t idx = 1; idx < arity; ++idx) {
        auto arg = viewFromStack(idx);
        if (!value::isArray(arg.tag)) {
            return {false, value::TypeTags::Nothing, 0};
        }

        argTags.push_back(arg.tag);
        argVals.push_back(arg.value);
    }

    return setEquals(argTags, argVals, value::getCollatorView(collView.value));
}

value::TagValueMaybeOwned ByteCode::builtinCollSetIsSubset(ArityType arity) {
    tassert(5154701, "$setIsSubset expects two sets and a collator", arity == 3);

    auto collView = viewFromStack(0);
    if (collView.tag != value::TypeTags::collator) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto lhs = viewFromStack(1);
    auto rhs = viewFromStack(2);

    return setIsSubset(
        lhs.tag, lhs.value, rhs.tag, rhs.value, value::getCollatorView(collView.value));
}

value::TagValueMaybeOwned ByteCode::builtinSetDifference(ArityType arity) {
    tassert(11080012, "Unexpected arity value", arity == 2);

    auto lhs = viewFromStack(0);
    auto rhs = viewFromStack(1);

    if (!value::isArray(lhs.tag) || !value::isArray(rhs.tag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    return setDifference(lhs.tag, lhs.value, rhs.tag, rhs.value);
}

value::TagValueMaybeOwned ByteCode::builtinSetEquals(ArityType arity) {
    tassert(11080011, "Unexpected arity value", arity >= 2);

    std::vector<value::TypeTags> argTags;
    std::vector<value::Value> argVals;

    for (size_t idx = 0; idx < arity; ++idx) {
        auto arg = viewFromStack(idx);
        if (!value::isArray(arg.tag)) {
            return {false, value::TypeTags::Nothing, 0};
        }

        argTags.push_back(arg.tag);
        argVals.push_back(arg.value);
    }

    return setEquals(argTags, argVals);
}

value::TagValueMaybeOwned ByteCode::builtinSetIsSubset(ArityType arity) {
    tassert(5154702, "$setIsSubset expects two sets", arity == 2);

    auto lhs = viewFromStack(0);
    auto rhs = viewFromStack(1);

    return setIsSubset(lhs.tag, lhs.value, rhs.tag, rhs.value);
}

value::TagValueMaybeOwned ByteCode::builtinSetToArray(ArityType arity) {
    tassert(11080010, "Unexpected arity value", arity == 1);

    auto input = value::TagValueMaybeOwned::fromRaw(moveFromStack(0));

    if (input.tag() != value::TypeTags::ArraySet && input.tag() != value::TypeTags::ArrayMultiSet) {
        // passthrough if its not a set
        return input;
    }

    auto [resTag, resVal] = value::makeNewArray();
    value::ValueGuard resGuard{resTag, resVal};
    auto resView = value::getArrayView(resVal);

    value::arrayForEach(input.tag(), input.value(), [&](value::TypeTags elTag, value::Value elVal) {
        resView->push_back_raw(value::copyValue(elTag, elVal));
    });

    resGuard.reset();
    return {true, resTag, resVal};
}

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
