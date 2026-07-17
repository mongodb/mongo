// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/values/util.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"

namespace mongo {
namespace sbe {
namespace vm {
value::TagValueMaybeOwned ByteCode::builtinAddToSet(ArityType arity) {
    auto [ownAgg, tagAgg, valAgg] = getFromStack(0);
    value::TagValueOwned field = moveOwnedFromStack(1);

    // Create a new array is it does not exist yet.
    if (tagAgg == value::TypeTags::Nothing) {
        ownAgg = true;
        std::tie(tagAgg, valAgg) = value::makeNewArraySet();
    } else {
        // Take ownership of the accumulator.
        topStack(false, value::TypeTags::Nothing, 0);
    }
    value::TagValueOwned agg{tagAgg, valAgg};

    tassert(11086805,
            "Unexpected type of Agg parameter",
            ownAgg && tagAgg == value::TypeTags::ArraySet);
    auto arr = value::getArraySetView(valAgg);

    // Push back the value. Note that array will ignore Nothing.
    arr->push_back(std::move(field));
    return std::move(agg);
}

value::TagValueMaybeOwned ByteCode::builtinAddToSetCapped(ArityType arity) {
    auto accumulatorState = moveOwnedFromStack(0);
    auto newElem = moveMaybeOwnedFromStack(1);

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
    value::TagValueOwned field = moveOwnedFromStack(2);

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
    value::TagValueOwned agg{tagAgg, valAgg};

    tassert(11086804,
            "Unexpected type of Agg parameter",
            ownAgg && tagAgg == value::TypeTags::ArraySet);
    auto arr = value::getArraySetView(valAgg);

    // Push back the value. Note that array will ignore Nothing.
    arr->push_back(std::move(field));
    return std::move(agg);
}

value::TagValueMaybeOwned ByteCode::builtinCollAddToSetCapped(ArityType arity) {
    auto accumulatorState = moveOwnedFromStack(0);

    auto collatorView = viewFromStack(1);

    auto newElem = moveMaybeOwnedFromStack(2);

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
    auto accumulatorState = moveOwnedFromStack(0);
    auto newSetMembers = moveOwnedFromStack(1);

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
    auto accumulatorState = moveOwnedFromStack(0);

    auto collatorView = viewFromStack(1);

    auto newSetMembers = moveOwnedFromStack(2);

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
    value::TagValueOwned res{value::makeNewArraySet(collator)};
    auto resView = value::getArraySetView(res.value());

    for (size_t idx = 0; idx < argVals.size(); ++idx) {
        auto argTag = argTags[idx];
        auto argVal = argVals[idx];

        value::arrayForEach(argTag, argVal, [&](value::TypeTags elTag, value::Value elVal) {
            resView->push_back_clone(elTag, elVal);
        });
    }
    return std::move(res);
}

value::TagValueMaybeOwned setIntersection(const std::vector<value::TypeTags>& argTags,
                                          const std::vector<value::Value>& argVals,
                                          const CollatorInterface* collator = nullptr) {
    auto intersectionMap =
        value::ValueMapType<size_t>{0, value::ValueHash(collator), value::ValueEq(collator)};

    value::TagValueOwned res{value::makeNewArraySet(collator)};

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
            return std::move(res);
        }
    }

    auto resView = value::getArraySetView(res.value());
    for (auto&& [item, counter] : intersectionMap) {
        if (counter == argVals.size()) {
            auto [elTag, elVal] = item;
            resView->push_back_clone(elTag, elVal);
        }
    }

    return std::move(res);
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
    value::TagValueOwned res{value::makeNewArraySet(collator)};
    auto resView = value::getArraySetView(res.value());

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

    return std::move(res);
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
        return value::TagValueMaybeOwned::nothing();
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
        return value::TagValueMaybeOwned::nothing();
    }

    std::vector<value::TypeTags> argTags;
    std::vector<value::Value> argVals;
    for (size_t idx = 1; idx < arity; ++idx) {
        auto arg = viewFromStack(idx);
        if (!value::isArray(arg.tag)) {
            return value::TagValueMaybeOwned::nothing();
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
            return value::TagValueMaybeOwned::nothing();
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
        return value::TagValueMaybeOwned::nothing();
    }

    std::vector<value::TypeTags> argTags;
    std::vector<value::Value> argVals;

    for (size_t idx = 1; idx < arity; ++idx) {
        auto arg = viewFromStack(idx);
        if (!value::isArray(arg.tag)) {
            return value::TagValueMaybeOwned::nothing();
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
            return value::TagValueMaybeOwned::nothing();
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
        return value::TagValueMaybeOwned::nothing();
    }

    auto lhs = viewFromStack(1);
    auto rhs = viewFromStack(2);

    if (!value::isArray(lhs.tag) || !value::isArray(rhs.tag)) {
        return value::TagValueMaybeOwned::nothing();
    }

    return setDifference(
        lhs.tag, lhs.value, rhs.tag, rhs.value, value::getCollatorView(collView.value));
}

value::TagValueMaybeOwned ByteCode::builtinCollSetEquals(ArityType arity) {
    tassert(11080013, "Unexpected arity value", arity >= 3);

    auto collView = viewFromStack(0);
    if (collView.tag != value::TypeTags::collator) {
        return value::TagValueMaybeOwned::nothing();
    }

    std::vector<value::TypeTags> argTags;
    std::vector<value::Value> argVals;

    for (size_t idx = 1; idx < arity; ++idx) {
        auto arg = viewFromStack(idx);
        if (!value::isArray(arg.tag)) {
            return value::TagValueMaybeOwned::nothing();
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
        return value::TagValueMaybeOwned::nothing();
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
        return value::TagValueMaybeOwned::nothing();
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
            return value::TagValueMaybeOwned::nothing();
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

    auto input = moveMaybeOwnedFromStack(0);

    if (input.tag() != value::TypeTags::ArraySet && input.tag() != value::TypeTags::ArrayMultiSet) {
        // passthrough if its not a set
        return input;
    }

    value::TagValueOwned res{value::makeNewArray()};
    auto resView = value::getArrayView(res.value());

    value::arrayForEach(input.tag(), input.value(), [&](value::TypeTags elTag, value::Value elVal) {
        resView->push_back_raw(value::copyValue(elTag, elVal));
    });

    return std::move(res);
}

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
