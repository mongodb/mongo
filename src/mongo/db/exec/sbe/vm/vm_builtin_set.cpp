// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/values/util.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/values/value_size.h"
#include "mongo/db/exec/sbe/vm/vm.h"

namespace mongo {
namespace sbe {
namespace vm {
namespace {
template <class GetOwned>
void insertUnionElement(value::Array& result,
                        value::ValueSetType& membership,
                        size_t& memoryBytes,
                        size_t limit,
                        value::TagValueView element,
                        GetOwned&& getOwned) {
    // The input holders outlive membership processing, even after a retained payload moves to
    // the result array.
    if (element.tag == value::TypeTags::Nothing || !membership.insert(element).second) {
        return;
    }
    memoryBytes += value::getApproximateSize(element.tag, element.value);
    uassert(ErrorCodes::ExceededMemoryLimit,
            str::stream() << "$setUnion would use too much memory (" << memoryBytes
                          << " bytes) and cannot spill to disk. Memory limit: " << limit
                          << " bytes",
            memoryBytes <= limit);
    result.push_back(getOwned());
}

value::ValueSetType valueToShallowSetHelper(value::TypeTags tag,
                                            value::Value value,
                                            const CollatorInterface* collator);

// Calls 'fn' with a hash table of the values in 'input'. Reuses the hash table of a prepared
// ArraySet if it uses the same collator, which saves building one on every call when constant
// folding has prepared the input. Otherwise, builds a temporary table of views into 'input'.
template <class Fn>
auto withHashTable(value::TagValueView input, const CollatorInterface* collator, Fn&& fn) {
    if (input.tag == value::TypeTags::ArraySet &&
        value::getArraySetView(input.value)->getCollator() == collator) {
        return fn(value::getArraySetView(input.value)->values());
    }
    return fn(valueToShallowSetHelper(input.tag, input.value, collator));
}

// Returns whether 'other' contains exactly the values in 'values'.
bool sameValues(const value::ValueSetType& values,
                value::TagValueView other,
                const CollatorInterface* collator) {
    if (other.tag == value::TypeTags::ArraySet &&
        value::getArraySetView(other.value)->getCollator() == collator) {
        return values == value::getArraySetView(other.value)->values();
    }
    // Stop at the first value that is missing from 'values'. Otherwise, 'other' is equal if it
    // contains as many distinct values as 'values'.
    value::ValueSetType seen(0, value::ValueHash(collator), value::ValueEq(collator));
    bool missing = value::arrayAny(other.tag, other.value, [&](auto tag, auto val) {
        if (!values.contains({tag, val})) {
            return true;
        }
        seen.insert({tag, val});
        return false;
    });
    return !missing && seen.size() == values.size();
}

// Returns the distinct values of 'lhs' that are not in 'rhsValues', in 'lhs' order.
value::TagValueOwned arrayDifference(value::TagValueMaybeOwned& lhs,
                                     const value::ValueSetType& rhsValues,
                                     const CollatorInterface* collator) {
    auto result = value::TagValueOwned::array();
    auto array = value::getArrayView(result.value());
    // 'added' holds views of the values already in the result. Moving an owned value into the
    // result doesn't move its payload, so these views stay valid.
    value::ValueSetType added(0, value::ValueHash(collator), value::ValueEq(collator));
    auto insert = [&](value::TagValueView element, auto&& getOwned) {
        if (!rhsValues.contains(element) && added.insert(element).second) {
            array->push_back(getOwned());
        }
    };
    if (lhs.owned() && lhs.tag() == value::TypeTags::Array) {
        auto ownedElements = value::getArrayView(lhs.value())->takeValues();
        for (auto& element : ownedElements) {
            insert(element.view(), [&] { return std::move(element); });
        }
    } else {
        value::arrayForEach(lhs.tag(), lhs.value(), [&](auto tag, auto val) {
            value::TagValueView element{tag, val};
            insert(element, [&] { return element.copy(); });
        });
    }
    return result;
}

// Returns the collator argument of an array-result set builtin, which may be Nothing.
const CollatorInterface* getSetCollator(value::TagValueView coll) {
    tassert(13889300,
            "Expected a collator or Nothing for an array-result set builtin",
            coll.tag == value::TypeTags::collator || coll.tag == value::TypeTags::Nothing);
    return coll.tag == value::TypeTags::collator ? value::getCollatorView(coll.value) : nullptr;
}
}  // namespace

boost::optional<std::vector<value::TagValueMaybeOwned>> ByteCode::moveSetOperands(ArityType arity) {
    // Operands may be views (for example, field paths or constants). Keep owned inputs and
    // their extracted elements alive until construction of the result is complete.
    std::vector<value::TagValueMaybeOwned> inputs;
    inputs.reserve(arity - 1);
    for (size_t arg = 1; arg < arity; ++arg) {
        if (!value::isArray(viewFromStack(arg).tag)) {
            return boost::none;
        }
        inputs.push_back(moveMaybeOwnedFromStack(arg));
    }
    return inputs;
}

value::TagValueMaybeOwned ByteCode::builtinSetUnionArray(ArityType arity) {
    const CollatorInterface* collator = getSetCollator(viewFromStack(0));
    auto maybeInputs = moveSetOperands(arity);
    if (!maybeInputs) {
        return value::TagValueMaybeOwned::nothing();
    }
    auto& inputs = *maybeInputs;
    auto result = value::TagValueOwned::array();
    auto array = value::getArrayView(result.value());
    // The hash table stores views into the values owned by the arrays and doesn't copy anything:
    //   hash table --> value <-- owning array
    // Growing the result array does not move the values themselves, so the hash keys stay valid.
    value::ValueSetType membership(0, value::ValueHash(collator), value::ValueEq(collator));
    std::vector<std::vector<value::TagValueOwned>> ownedElements(inputs.size());
    size_t memoryBytes = 0;
    const auto limit = internalQueryMaxSingleExpressionMemoryUsageBytes.loadRelaxed();
    for (size_t arg = 0; arg < inputs.size(); ++arg) {
        auto& input = inputs[arg];
        if (input.owned() && input.tag() == value::TypeTags::Array) {
            ownedElements[arg] = value::getArrayView(input.value())->takeValues();
            for (auto& element : ownedElements[arg]) {
                insertUnionElement(*array, membership, memoryBytes, limit, element.view(), [&] {
                    return std::move(element);
                });
            }
        } else {
            value::arrayForEach(input.tag(), input.value(), [&](auto tag, auto val) {
                value::TagValueView element{tag, val};
                insertUnionElement(*array, membership, memoryBytes, limit, element, [&] {
                    return element.copy();
                });
            });
        }
    }
    return std::move(result);
}

value::TagValueMaybeOwned ByteCode::builtinSetIntersectionArray(ArityType arity) {
    const CollatorInterface* collator = getSetCollator(viewFromStack(0));
    auto maybeInputs = moveSetOperands(arity);
    if (!maybeInputs) {
        return value::TagValueMaybeOwned::nothing();
    }
    auto& inputs = *maybeInputs;
    auto result = value::TagValueOwned::array();
    if (inputs.empty()) {
        return std::move(result);
    }

    // Candidates are the distinct elements of the first input, in order, with the number of inputs
    // that contain them. The hash table maps views of the elements to their candidates and doesn't
    // copy anything. The result takes its values from the first input, so only that input's owned
    // values are transferred.
    struct Candidate {
        value::TagValueView element;
        // The index in 'ownedElements', or none when the first input is a view.
        boost::optional<size_t> ownedIndex;
        size_t count;
    };
    std::vector<Candidate> candidates;
    value::ValueMapType<size_t> positions(0, value::ValueHash(collator), value::ValueEq(collator));
    std::vector<value::TagValueOwned> ownedElements;
    auto& first = inputs[0];
    auto addCandidate = [&](value::TagValueView element, boost::optional<size_t> ownedIndex) {
        if (positions.insert(element, candidates.size()).second) {
            candidates.push_back({element, ownedIndex, 1});
        }
    };
    if (first.owned() && first.tag() == value::TypeTags::Array) {
        ownedElements = value::getArrayView(first.value())->takeValues();
        for (size_t i = 0; i < ownedElements.size(); ++i) {
            addCandidate(ownedElements[i].view(), i);
        }
    } else {
        value::arrayForEach(first.tag(), first.value(), [&](auto tag, auto val) {
            addCandidate({tag, val}, boost::none);
        });
    }

    for (size_t arg = 1; arg < inputs.size(); ++arg) {
        // An element counts at most once per input, and only if every earlier input contains it.
        bool anyMatch = false;
        value::arrayForEach(inputs[arg].tag(), inputs[arg].value(), [&](auto tag, auto val) {
            if (auto it = positions.find({tag, val}); it != positions.end()) {
                auto& candidate = candidates[it->second];
                if (candidate.count == arg) {
                    ++candidate.count;
                    anyMatch = true;
                }
            }
        });
        if (!anyMatch) {
            // We can short circuit since the intersection is empty.
            return std::move(result);
        }
    }

    auto array = value::getArrayView(result.value());
    for (const auto& candidate : candidates) {
        if (candidate.count == inputs.size()) {
            array->push_back(candidate.ownedIndex ? std::move(ownedElements[*candidate.ownedIndex])
                                                  : candidate.element.copy());
        }
    }
    return std::move(result);
}

value::TagValueMaybeOwned ByteCode::builtinSetDifferenceArray(ArityType arity) {
    // Compiling setDifferenceArray already checks that it has three arguments: the collator and
    // the two arrays.
    tassert(13889301, "Unexpected arity value", arity == 3);
    const CollatorInterface* collator = getSetCollator(viewFromStack(0));
    auto maybeInputs = moveSetOperands(arity);
    if (!maybeInputs) {
        return value::TagValueMaybeOwned::nothing();
    }
    auto& lhs = (*maybeInputs)[0];
    auto& rhs = (*maybeInputs)[1];

    return withHashTable(rhs.view(), collator, [&](const value::ValueSetType& rhsValues) {
        return arrayDifference(lhs, rhsValues, collator);
    });
}

value::TagValueMaybeOwned ByteCode::builtinSetEqualsArray(ArityType arity) {
    // Compiling setEqualsArray already checks that it has the collator and at least two arrays.
    tassert(13889302, "Unexpected arity value", arity >= 3);
    const CollatorInterface* collator = getSetCollator(viewFromStack(0));
    // The result is a boolean, so the operands can stay on the stack as views.
    for (size_t arg = 1; arg < arity; ++arg) {
        if (!value::isArray(viewFromStack(arg).tag)) {
            return value::TagValueMaybeOwned::nothing();
        }
    }
    return withHashTable(viewFromStack(1), collator, [&](const value::ValueSetType& first) {
        for (size_t arg = 2; arg < arity; ++arg) {
            if (!sameValues(first, viewFromStack(arg), collator)) {
                return value::TagValueMaybeOwned::boolean(false);
            }
        }
        return value::TagValueMaybeOwned::boolean(true);
    });
}

value::TagValueMaybeOwned ByteCode::builtinSetIsSubsetArray(ArityType arity) {
    MONGO_UNREACHABLE_TASSERT(13890504);
}

value::TagValueMaybeOwned ByteCode::builtinAddToSet(ArityType arity) {
    auto [ownAgg, tagAgg, valAgg] = getFromStack(0);
    value::TagValueOwned field = moveOwnedFromStack(1);

    // Create a new array if it does not exist yet.
    if (tagAgg == value::TypeTags::Nothing) {
        ownAgg = true;
        std::tie(tagAgg, valAgg) = value::makeNewArraySet();
    } else {
        // Take ownership of the accumulator.
        topStack(false, value::TypeTags::Nothing, 0);
    }
    value::TagValueOwned agg = value::TagValueOwned::fromRaw(tagAgg, valAgg);

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
                              /*collator=*/nullptr);
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
    value::TagValueOwned agg = value::TagValueOwned::fromRaw(tagAgg, valAgg);

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
                             /*collator=*/nullptr);
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
    value::TagValueOwned res = value::TagValueOwned::fromRaw(value::makeNewArraySet(collator));
    auto resView = value::getArraySetView(res.value());

    size_t currentMemoryBytes = 0;
    const size_t maxMemoryBytes = internalQueryMaxSingleExpressionMemoryUsageBytes.loadRelaxed();

    for (size_t idx = 0; idx < argVals.size(); ++idx) {
        auto argTag = argTags[idx];
        auto argVal = argVals[idx];

        value::arrayForEach(argTag, argVal, [&](value::TypeTags elTag, value::Value elVal) {
            if (resView->push_back_clone(elTag, elVal)) {
                currentMemoryBytes += value::getApproximateSize(elTag, elVal);
                if (MONGO_unlikely(currentMemoryBytes > maxMemoryBytes)) {
                    uasserted(ErrorCodes::ExceededMemoryLimit,
                              str::stream()
                                  << "$setUnion would use too much memory (" << currentMemoryBytes
                                  << " bytes) and cannot spill to disk. Memory limit: "
                                  << maxMemoryBytes << " bytes");
                }
            }
        });
    }
    return std::move(res);
}

value::TagValueMaybeOwned setIntersection(const std::vector<value::TypeTags>& argTags,
                                          const std::vector<value::Value>& argVals,
                                          const CollatorInterface* collator = nullptr) {
    auto intersectionMap =
        value::ValueMapType<size_t>{0, value::ValueHash(collator), value::ValueEq(collator)};

    value::TagValueOwned res = value::TagValueOwned::fromRaw(value::makeNewArraySet(collator));

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
    value::TagValueOwned res = value::TagValueOwned::fromRaw(value::makeNewArraySet(collator));
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

    value::TagValueOwned res = value::TagValueOwned::fromRaw(value::makeNewArray());
    auto resView = value::getArrayView(res.value());

    value::arrayForEach(input.tag(), input.value(), [&](value::TypeTags elTag, value::Value elVal) {
        resView->push_back_raw(value::copyValue(elTag, elVal));
    });

    return std::move(res);
}

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
