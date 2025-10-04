/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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


#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/expression/evaluate.h"

namespace mongo {

namespace exec::expression {

ValueFlatUnorderedSet arrayToUnorderedSet(const Value& val,
                                          const ValueComparator& valueComparator) {
    const std::vector<Value>& array = val.getArray();
    ValueFlatUnorderedSet valueSet = valueComparator.makeFlatUnorderedValueSet();
    valueSet.insert(array.begin(), array.end());
    return valueSet;
}

ValueUnorderedMap<std::vector<int>> arrayToIndexMap(const Value& val,
                                                    const ValueComparator& valueComparator) {
    const std::vector<Value>& array = val.getArray();
    // To handle the case of duplicate values the values need to map to a vector of indecies.
    ValueUnorderedMap<std::vector<int>> indexMap =
        valueComparator.makeUnorderedValueMap<std::vector<int>>();

    for (int i = 0; i < int(array.size()); i++) {
        indexMap[array[i]].push_back(i);
    }
    return indexMap;
}

Value evaluate(const ExpressionArray& expr, const Document& root, Variables* variables) {
    auto& children = expr.getChildren();
    std::vector<Value> values;
    values.reserve(children.size());
    for (auto&& child : children) {
        Value elemVal = child->evaluate(root, variables);
        values.push_back(elemVal.missing() ? Value(BSONNULL) : std::move(elemVal));
    }
    return Value(std::move(values));
}

namespace {
Value arrayElemAt(const ExpressionNary& self, Value array, Value indexArg) {
    if (array.nullish() || indexArg.nullish()) {
        return Value(BSONNULL);
    }

    size_t arity = self.getOperandList().size();
    uassert(28689,
            str::stream() << self.getOpName() << "'s "
                          << (arity == 1 ? "argument" : "first argument")
                          << " must be an array, but is " << typeName(array.getType()),
            array.isArray());
    uassert(28690,
            str::stream() << self.getOpName() << "'s second argument must be a numeric value,"
                          << " but is " << typeName(indexArg.getType()),
            indexArg.numeric());
    uassert(28691,
            str::stream() << self.getOpName() << "'s second argument must be representable as"
                          << " a 32-bit integer: " << indexArg.coerceToDouble(),
            indexArg.integral());

    long long i = indexArg.coerceToLong();
    if (i < 0 && static_cast<size_t>(std::abs(i)) > array.getArrayLength()) {
        // Positive indices that are too large are handled automatically by Value.
        return Value();
    } else if (i < 0) {
        // Index from the back of the array.
        i = array.getArrayLength() + i;
    }
    const size_t index = static_cast<size_t>(i);
    return array[index];
}
}  // namespace

Value evaluate(const ExpressionArrayElemAt& expr, const Document& root, Variables* variables) {
    auto& children = expr.getChildren();
    const Value array = children[0]->evaluate(root, variables);
    const Value indexArg = children[1]->evaluate(root, variables);
    return arrayElemAt(expr, array, indexArg);
}

Value evaluate(const ExpressionFirst& expr, const Document& root, Variables* variables) {
    auto& children = expr.getChildren();
    const Value array = children[0]->evaluate(root, variables);
    return arrayElemAt(expr, array, Value(0));
}

Value evaluate(const ExpressionLast& expr, const Document& root, Variables* variables) {
    auto& children = expr.getChildren();
    const Value array = children[0]->evaluate(root, variables);
    return arrayElemAt(expr, array, Value(-1));
}

Value evaluate(const ExpressionObjectToArray& expr, const Document& root, Variables* variables) {
    auto& children = expr.getChildren();
    const Value targetVal = children[0]->evaluate(root, variables);

    if (targetVal.nullish()) {
        return Value(BSONNULL);
    }

    uassert(40390,
            str::stream() << "$objectToArray requires a document input, found: "
                          << typeName(targetVal.getType()),
            (targetVal.getType() == BSONType::object));

    std::vector<Value> output;

    FieldIterator iter = targetVal.getDocument().fieldIterator();
    while (iter.more()) {
        Document::FieldPair pair = iter.next();
        MutableDocument keyvalue;
        keyvalue.addField("k", Value(pair.first));
        keyvalue.addField("v", std::move(pair.second));
        output.push_back(keyvalue.freezeToValue());
    }

    return Value(std::move(output));
}

Value evaluate(const ExpressionArrayToObject& expr, const Document& root, Variables* variables) {
    auto& children = expr.getChildren();
    const Value input = children[0]->evaluate(root, variables);
    if (input.nullish()) {
        return Value(BSONNULL);
    }

    uassert(40386,
            str::stream() << "$arrayToObject requires an array input, found: "
                          << typeName(input.getType()),
            input.isArray());

    MutableDocument output;
    const std::vector<Value>& array = input.getArray();
    if (array.empty()) {
        return output.freezeToValue();
    }

    // There are two accepted input formats in an array: [ [key, val] ] or [ {k:key, v:val} ]. The
    // first array element determines the format for the rest of the array. Mixing input formats is
    // not allowed.
    bool inputArrayFormat;
    if (array[0].isArray()) {
        inputArrayFormat = true;
    } else if (array[0].getType() == BSONType::object) {
        inputArrayFormat = false;
    } else {
        uasserted(40398,
                  str::stream() << "Unrecognised input type format for $arrayToObject: "
                                << typeName(array[0].getType()));
    }

    for (auto&& elem : array) {
        if (inputArrayFormat == true) {
            uassert(
                40396,
                str::stream() << "$arrayToObject requires a consistent input format. Elements must"
                                 "all be arrays or all be objects. Array was detected, now found: "
                              << typeName(elem.getType()),
                elem.isArray());

            const std::vector<Value>& valArray = elem.getArray();

            uassert(40397,
                    str::stream() << "$arrayToObject requires an array of size 2 arrays,"
                                     "found array of size: "
                                  << valArray.size(),
                    (valArray.size() == 2));

            uassert(40395,
                    str::stream() << "$arrayToObject requires an array of key-value pairs, where "
                                     "the key must be of type string. Found key type: "
                                  << typeName(valArray[0].getType()),
                    (valArray[0].getType() == BSONType::string));

            auto keyName = valArray[0].getStringData();

            uassert(4940400,
                    "Key field cannot contain an embedded null byte",
                    keyName.find('\0') == std::string::npos);

            output[keyName] = valArray[1];

        } else {
            uassert(
                40391,
                str::stream() << "$arrayToObject requires a consistent input format. Elements must"
                                 "all be arrays or all be objects. Object was detected, now found: "
                              << typeName(elem.getType()),
                (elem.getType() == BSONType::object));

            uassert(40392,
                    str::stream() << "$arrayToObject requires an object keys of 'k' and 'v'. "
                                     "Found incorrect number of keys:"
                                  << elem.getDocument().computeSize(),
                    (elem.getDocument().computeSize() == 2));

            Value key = elem.getDocument().getField("k");
            Value value = elem.getDocument().getField("v");

            uassert(40393,
                    str::stream() << "$arrayToObject requires an object with keys 'k' and 'v'. "
                                     "Missing either or both keys from: "
                                  << elem.toString(),
                    (!key.missing() && !value.missing()));

            uassert(
                40394,
                str::stream() << "$arrayToObject requires an object with keys 'k' and 'v', where "
                                 "the value of 'k' must be of type string. Found type: "
                              << typeName(key.getType()),
                (key.getType() == BSONType::string));

            auto keyName = key.getStringData();

            uassert(4940401,
                    "Key field cannot contain an embedded null byte",
                    keyName.find('\0') == std::string::npos);

            output[keyName] = value;
        }
    }

    return output.freezeToValue();
}

Value evaluate(const ExpressionConcatArrays& expr, const Document& root, Variables* variables) {
    auto& children = expr.getChildren();
    const size_t n = children.size();
    std::vector<Value> values;

    for (size_t i = 0; i < n; ++i) {
        Value val = children[i]->evaluate(root, variables);
        if (val.nullish()) {
            return Value(BSONNULL);
        }

        uassert(28664,
                str::stream() << "$concatArrays only supports arrays, not "
                              << typeName(val.getType()),
                val.isArray());

        const auto& subValues = val.getArray();
        values.insert(values.end(), subValues.begin(), subValues.end());
    }
    return Value(std::move(values));
}

namespace {

struct Arguments {
    Arguments(Value targetOfSearch, int startIndex, int endIndex)
        : targetOfSearch(targetOfSearch), startIndex(startIndex), endIndex(endIndex) {}

    Value targetOfSearch;
    int startIndex;
    int endIndex;
};

void uassertIfNotIntegralAndNonNegative(Value val,
                                        StringData expressionName,
                                        StringData argumentName) {
    uassert(9711600,
            str::stream() << expressionName << "requires an integral " << argumentName
                          << ", found a value of type: " << typeName(val.getType())
                          << ", with value: " << val.toString(),
            val.integral());
    uassert(9711601,
            str::stream() << expressionName << " requires a nonnegative " << argumentName
                          << ", found: " << val.toString(),
            val.coerceToInt() >= 0);
}

/**
 * When given 'operands' which correspond to the arguments to $indexOfArray, evaluates and
 * validates the target value, starting index, and ending index arguments and returns their
 * values as a Arguments struct. The starting index and ending index are optional, so as default
 * 'startIndex' will be 0 and 'endIndex' will be the length of the input array. Throws a
 * UserException if the values are found to be invalid in some way, e.g. if the indexes are not
 * numbers.
 */
Arguments evaluateAndValidateArguments(const ExpressionIndexOfArray& expr,
                                       const Document& root,
                                       const Expression::ExpressionVector& operands,
                                       size_t arrayLength,
                                       Variables* variables) {

    int startIndex = 0;
    if (operands.size() > 2) {
        Value startIndexArg = operands[2]->evaluate(root, variables);
        uassertIfNotIntegralAndNonNegative(startIndexArg, expr.getOpName(), "starting index");

        startIndex = startIndexArg.coerceToInt();
    }

    int endIndex = arrayLength;
    if (operands.size() > 3) {
        Value endIndexArg = operands[3]->evaluate(root, variables);
        uassertIfNotIntegralAndNonNegative(endIndexArg, expr.getOpName(), "ending index");
        // Don't let 'endIndex' exceed the length of the array.

        endIndex = std::min(static_cast<int>(arrayLength), endIndexArg.coerceToInt());
    }
    return {operands[1]->evaluate(root, variables), startIndex, endIndex};
}
}  // namespace

Value evaluate(const ExpressionIndexOfArray& expr, const Document& root, Variables* variables) {
    auto& children = expr.getChildren();
    Value arrayArg = children[0]->evaluate(root, variables);

    if (arrayArg.nullish()) {
        return Value(BSONNULL);
    }

    uassert(40090,
            str::stream() << "$indexOfArray requires an array as a first argument, found: "
                          << typeName(arrayArg.getType()),
            arrayArg.isArray());

    const std::vector<Value>& array = arrayArg.getArray();
    auto args = evaluateAndValidateArguments(expr, root, children, array.size(), variables);

    if (expr.getParsedIndexMap()) {
        auto indexVec = expr.getParsedIndexMap()->find(args.targetOfSearch);

        if (indexVec == expr.getParsedIndexMap()->end()) {
            return Value(-1);
        }

        // Search through the vector of indexes for first index in our range.
        for (auto index : indexVec->second) {
            if (index >= args.startIndex && index < args.endIndex) {
                return Value(index);
            }
        }
    } else {
        for (int i = args.startIndex; i < args.endIndex; i++) {
            if (expr.getExpressionContext()->getValueComparator().evaluate(array[i] ==
                                                                           args.targetOfSearch)) {
                return Value(static_cast<int>(i));
            }
        }
    }

    // The value was not found in the specified range.
    return Value(-1);
}

Value evaluate(const ExpressionIsArray& expr, const Document& root, Variables* variables) {
    auto& children = expr.getChildren();
    Value argument = children[0]->evaluate(root, variables);
    return Value(argument.isArray());
}

Value evaluate(const ExpressionReverseArray& expr, const Document& root, Variables* variables) {
    auto& children = expr.getChildren();
    Value input(children[0]->evaluate(root, variables));

    if (input.nullish()) {
        return Value(BSONNULL);
    }

    uassert(34435,
            str::stream() << "The argument to $reverseArray must be an array, but was of type: "
                          << typeName(input.getType()),
            input.isArray());

    if (input.getArrayLength() < 2) {
        return input;
    }

    std::vector<Value> array = input.getArray();
    std::reverse(array.begin(), array.end());
    return Value(std::move(array));
}

Value evaluate(const ExpressionSortArray& expr, const Document& root, Variables* variables) {
    Value input(expr.getInput()->evaluate(root, variables));

    if (input.nullish()) {
        return Value(BSONNULL);
    }

    uassert(2942504,
            str::stream() << "The input argument to $sortArray must be an array, but was of type: "
                          << typeName(input.getType()),
            input.isArray());

    if (input.getArrayLength() < 2) {
        return input;
    }

    std::vector<Value> array = input.getArray();
    std::sort(array.begin(), array.end(), expr.getSortBy());
    return Value(std::move(array));
}

namespace {

ValueSet arrayToSet(const Value& val, const ValueComparator& valueComparator) {
    const std::vector<Value>& array = val.getArray();
    ValueSet valueSet = valueComparator.makeOrderedValueSet();
    valueSet.insert(array.begin(), array.end());
    return valueSet;
}

bool setEqualsHelper(const ValueFlatUnorderedSet& lhs,
                     const ValueFlatUnorderedSet& rhs,
                     const ValueComparator& valueComparator) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (const auto& entry : lhs) {
        if (!rhs.count(entry)) {
            return false;
        }
    }
    return true;
}

}  // namespace

Value evaluate(const ExpressionSetDifference& expr, const Document& root, Variables* variables) {
    auto& children = expr.getChildren();
    const Value lhs = children[0]->evaluate(root, variables);
    const Value rhs = children[1]->evaluate(root, variables);

    if (lhs.nullish() || rhs.nullish()) {
        return Value(BSONNULL);
    }

    uassert(17048,
            str::stream() << "both operands of $setDifference must be arrays. First "
                          << "argument is of type: " << typeName(lhs.getType()),
            lhs.isArray());
    uassert(17049,
            str::stream() << "both operands of $setDifference must be arrays. Second "
                          << "argument is of type: " << typeName(rhs.getType()),
            rhs.isArray());

    ValueSet rhsSet = arrayToSet(rhs, expr.getExpressionContext()->getValueComparator());
    const std::vector<Value>& lhsArray = lhs.getArray();
    std::vector<Value> returnVec;

    for (std::vector<Value>::const_iterator it = lhsArray.begin(); it != lhsArray.end(); ++it) {
        // rhsSet serves the dual role of filtering out elements that were originally present
        // in RHS and of eleminating duplicates from LHS
        if (rhsSet.insert(*it).second) {
            returnVec.push_back(*it);
        }
    }
    return Value(std::move(returnVec));
}

Value evaluate(const ExpressionSetEquals& expr, const Document& root, Variables* variables) {
    auto& children = expr.getChildren();
    const size_t n = children.size();
    const auto& valueComparator = expr.getExpressionContext()->getValueComparator();

    auto evaluateChild = [&](size_t index) {
        const Value entry = children[index]->evaluate(root, variables);
        uassert(17044,
                str::stream() << "All operands of $setEquals must be arrays. " << (index + 1)
                              << "-th argument is of type: " << typeName(entry.getType()),
                entry.isArray());
        ValueFlatUnorderedSet entrySet = valueComparator.makeFlatUnorderedValueSet();
        entrySet.insert(entry.getArray().begin(), entry.getArray().end());
        return entrySet;
    };

    auto cachedConstant = expr.getCachedConstant();
    size_t lhsIndex = cachedConstant ? cachedConstant->first : 0;
    // The $setEquals expression has at least two children, so accessing the first child without
    // check is fine.
    ValueFlatUnorderedSet lhs = cachedConstant ? cachedConstant->second : evaluateChild(0);

    for (size_t i = 0; i < n; i++) {
        if (i != lhsIndex) {
            ValueFlatUnorderedSet rhs = evaluateChild(i);
            if (!setEqualsHelper(lhs, rhs, valueComparator)) {
                return Value(false);
            }
        }
    }
    return Value(true);
}

Value evaluate(const ExpressionSetIntersection& expr, const Document& root, Variables* variables) {
    auto& children = expr.getChildren();
    const size_t n = children.size();
    const auto& valueComparator = expr.getExpressionContext()->getValueComparator();
    ValueSet currentIntersection = valueComparator.makeOrderedValueSet();
    for (size_t i = 0; i < n; i++) {
        const Value nextEntry = children[i]->evaluate(root, variables);
        if (nextEntry.nullish()) {
            return Value(BSONNULL);
        }
        uassert(17047,
                str::stream() << "All operands of $setIntersection must be arrays. One "
                              << "argument is of type: " << typeName(nextEntry.getType()),
                nextEntry.isArray());

        if (i == 0) {
            currentIntersection.insert(nextEntry.getArray().begin(), nextEntry.getArray().end());
        } else if (!currentIntersection.empty()) {
            ValueSet nextSet = arrayToSet(nextEntry, valueComparator);
            if (currentIntersection.size() > nextSet.size()) {
                // to iterate over whichever is the smaller set
                nextSet.swap(currentIntersection);
            }
            ValueSet::iterator it = currentIntersection.begin();
            while (it != currentIntersection.end()) {
                if (!nextSet.count(*it)) {
                    ValueSet::iterator del = it;
                    ++it;
                    currentIntersection.erase(del);
                } else {
                    ++it;
                }
            }
        }
    }
    return Value(std::vector<Value>(currentIntersection.begin(), currentIntersection.end()));
}

namespace {

Value setIsSubsetHelper(const std::vector<Value>& lhs, const ValueFlatUnorderedSet& rhs) {
    // do not shortcircuit when lhs.size() > rhs.size()
    // because lhs can have redundant entries
    for (std::vector<Value>::const_iterator it = lhs.begin(); it != lhs.end(); ++it) {
        if (!rhs.count(*it)) {
            return Value(false);
        }
    }
    return Value(true);
}

}  // namespace

Value evaluate(const ExpressionSetIsSubset& expr, const Document& root, Variables* variables) {
    auto& children = expr.getChildren();
    const Value lhs = children[0]->evaluate(root, variables);

    uassert(17046,
            str::stream() << "both operands of $setIsSubset must be arrays. First "
                          << "argument is of type: " << typeName(lhs.getType()),
            lhs.isArray());

    if (expr.getCachedRhsSet()) {
        return setIsSubsetHelper(lhs.getArray(), *expr.getCachedRhsSet());
    } else {
        const Value rhs = children[1]->evaluate(root, variables);
        uassert(17042,
                str::stream() << "both operands of $setIsSubset must be arrays. Second "
                              << "argument is of type: " << typeName(rhs.getType()),
                rhs.isArray());

        return setIsSubsetHelper(
            lhs.getArray(),
            arrayToUnorderedSet(rhs, expr.getExpressionContext()->getValueComparator()));
    }
}

Value evaluate(const ExpressionSetUnion& expr, const Document& root, Variables* variables) {
    ValueSet unionedSet = expr.getExpressionContext()->getValueComparator().makeOrderedValueSet();
    auto& children = expr.getChildren();
    const size_t n = children.size();
    for (size_t i = 0; i < n; i++) {
        const Value newEntries = children[i]->evaluate(root, variables);
        if (newEntries.nullish()) {
            return Value(BSONNULL);
        }
        uassert(17043,
                str::stream() << "All operands of $setUnion must be arrays. One argument"
                              << " is of type: " << typeName(newEntries.getType()),
                newEntries.isArray());

        unionedSet.insert(newEntries.getArray().begin(), newEntries.getArray().end());
    }
    return Value(std::vector<Value>(unionedSet.begin(), unionedSet.end()));
}

namespace {
/*
 * Ensure both arguments to a vector similarity algorithm are arrays of numeric values of the same
 * size.
 */
std::pair<std::vector<Value>, std::vector<Value>> validate(const Value& val1,
                                                           const Value& val2,
                                                           const std::string& opName) {

    uassert(10413200,
            str::stream() << "First argument to " << opName
                          << " must be an array, but is of type: " << typeName(val1.getType()),
            val1.isArray());

    uassert(10413201,
            str::stream() << "Second argument to " << opName
                          << " must be an array, but is of type: " << typeName(val2.getType()),
            val2.isArray());

    const auto& array1 = val1.getArray();
    const auto& array2 = val2.getArray();

    uassert(10413202,
            str::stream() << "Array arguments to " << opName
                          << " must be the same size, but the first is of size "
                          << std::to_string(array1.size()) << " and the second is of size "
                          << std::to_string(array2.size()),
            array1.size() == array2.size());

    uassert(10413203,
            str::stream() << "All elements in the first argument to " << opName
                          << " must be numeric",
            std::all_of(array1.begin(), array1.end(), [](const auto& e) { return e.numeric(); }));

    uassert(10413204,
            str::stream() << "All elements in the second argument to " << opName
                          << " must be numeric",
            std::all_of(array2.begin(), array2.end(), [](const auto& e) { return e.numeric(); }));

    return {array1, array2};
}

/*
 * Calculate the dot product of array1 and array2.
 */
double calculateDotProduct(const std::vector<Value>& array1, const std::vector<Value>& array2) {
    double sum = 0;
    for (size_t i = 0; i < array1.size(); i++) {
        sum += array1[i].coerceToDouble() * array2[i].coerceToDouble();
    }

    return sum;
}

/*
 * Calculate the euclidean product of array1 and array2.
 */
double calculateEuclideanDistance(const std::vector<Value>& array1,
                                  const std::vector<Value>& array2) {
    double sum = 0;
    for (size_t i = 0; i < array1.size(); i++) {
        auto difference = array1[i].coerceToDouble() - array2[i].coerceToDouble();
        sum += std::pow(difference, 2);
    }

    return std::sqrt(sum);
}

/*
 * Calculate the magnitude of a vector.
 */
double calculateMagnitude(const std::vector<Value>& vector) {
    double sum = 0;
    for (size_t i = 0; i < vector.size(); ++i) {
        double d = vector[i].coerceToDouble();
        sum += std::pow(d, 2);
    }
    return std::sqrt(sum);
}

/*
 * Calculate the cosine similarity of array1 and array2.
 */
double calculateCosineSimilarity(const std::vector<Value>& array1,
                                 const std::vector<Value>& array2) {
    const auto magnitudeProduct = calculateMagnitude(array1) * calculateMagnitude(array2);

    // Avoid division by zero.
    if (magnitudeProduct == 0) {
        return 0;
    }

    return calculateDotProduct(array1, array2) / magnitudeProduct;
}

Value evaluateSimilarity(
    const ExpressionVectorSimilarity& expr,
    const Document& root,
    Variables* variables,
    std::function<double(const std::vector<Value>&, const std::vector<Value>&)> calculateSimilarity,
    std::function<double(double)> normalize) {
    const auto& children = expr.getChildren();
    const Value arrayVal1 = children[0]->evaluate(root, variables);
    const Value arrayVal2 = children[1]->evaluate(root, variables);

    if (arrayVal1.nullish() || arrayVal2.nullish()) {
        return Value(BSONNULL);
    }

    const auto& [array1, array2] = validate(arrayVal1, arrayVal2, expr.getOpName());
    const auto similarity = calculateSimilarity(array1, array2);

    return Value(expr.isScore() ? normalize(similarity) : similarity);
}

}  // namespace

Value evaluate(const ExpressionSimilarityDotProduct& expr,
               const Document& root,
               Variables* variables) {
    // Normalize according to the corresponding formula defined in the Atlas documentation at the
    // link below.
    // https://www.mongodb.com/docs/atlas/atlas-vector-search/vector-search-stage/#atlas-vector-search-score
    const auto normalizeDotProduct = [](double dotProduct) {
        return (1 + dotProduct) / 2;
    };

    return evaluateSimilarity(expr, root, variables, calculateDotProduct, normalizeDotProduct);
}

Value evaluate(const ExpressionSimilarityCosine& expr, const Document& root, Variables* variables) {
    // Normalize according to the corresponding formula defined in the Atlas documentation at the
    // link below.
    // https://www.mongodb.com/docs/atlas/atlas-vector-search/vector-search-stage/#atlas-vector-search-score
    const auto normalizeCosine = [](double cosine) {
        return (1 + cosine) / 2;
    };

    return evaluateSimilarity(expr, root, variables, calculateCosineSimilarity, normalizeCosine);
}

Value evaluate(const ExpressionSimilarityEuclidean& expr,
               const Document& root,
               Variables* variables) {
    // Normalize according to the corresponding formula defined in the Atlas documentation at the
    // link below.
    // https://www.mongodb.com/docs/atlas/atlas-vector-search/vector-search-stage/#atlas-vector-search-score
    const auto normalizeDistance = [](double dist) {
        return 1 / (1 + dist);
    };

    return evaluateSimilarity(expr, root, variables, calculateEuclideanDistance, normalizeDistance);
}

Value evaluate(const ExpressionSlice& expr, const Document& root, Variables* variables) {
    auto& children = expr.getChildren();
    const size_t n = children.size();

    Value arrayVal = children[0]->evaluate(root, variables);
    // Could be either a start index or the length from 0.
    Value arg2 = children[1]->evaluate(root, variables);

    if (arrayVal.nullish() || arg2.nullish()) {
        return Value(BSONNULL);
    }

    uassert(28724,
            str::stream() << "First argument to $slice must be an array, but is"
                          << " of type: " << typeName(arrayVal.getType()),
            arrayVal.isArray());
    uassert(28725,
            str::stream() << "Second argument to $slice must be a numeric value,"
                          << " but is of type: " << typeName(arg2.getType()),
            arg2.numeric());
    uassert(28726,
            str::stream() << "Second argument to $slice can't be represented as"
                          << " a 32-bit integer: " << arg2.coerceToDouble(),
            arg2.integral());

    const auto& array = arrayVal.getArray();
    size_t start;
    size_t end;

    if (n == 2) {
        // Only count given.
        int count = arg2.coerceToInt();
        start = 0;
        end = array.size();
        if (count >= 0) {
            end = std::min(end, size_t(count));
        } else {
            // Negative count's start from the back. If a abs(count) is greater
            // than the
            // length of the array, return the whole array.
            start = std::max(0, static_cast<int>(array.size()) + count);
        }
    } else {
        // We have both a start index and a count.
        int startInt = arg2.coerceToInt();
        if (startInt < 0) {
            // Negative values start from the back. If a abs(start) is greater
            // than the length
            // of the array, start from 0.
            start = std::max(0, static_cast<int>(array.size()) + startInt);
        } else {
            start = std::min(array.size(), size_t(startInt));
        }

        Value countVal = children[2]->evaluate(root, variables);

        if (countVal.nullish()) {
            return Value(BSONNULL);
        }

        uassert(28727,
                str::stream() << "Third argument to $slice must be numeric, but "
                              << "is of type: " << typeName(countVal.getType()),
                countVal.numeric());
        uassert(28728,
                str::stream() << "Third argument to $slice can't be represented"
                              << " as a 32-bit integer: " << countVal.coerceToDouble(),
                countVal.integral());
        uassert(28729,
                str::stream() << "Third argument to $slice must be positive: "
                              << countVal.coerceToInt(),
                countVal.coerceToInt() > 0);

        size_t count = size_t(countVal.coerceToInt());
        end = std::min(start + count, array.size());
    }

    return Value(std::vector<Value>(array.begin() + start, array.begin() + end));
}

Value evaluate(const ExpressionSize& expr, const Document& root, Variables* variables) {
    auto& children = expr.getChildren();
    Value array = children[0]->evaluate(root, variables);

    uassert(17124,
            str::stream() << "The argument to $size must be an array, but was of type: "
                          << typeName(array.getType()),
            array.isArray());
    return Value::createIntOrLong(array.getArray().size());
}

Value evaluate(const ExpressionZip& expr, const Document& root, Variables* variables) {
    // Evaluate input values.
    std::vector<std::vector<Value>> inputValues;
    auto& inputs = expr.getInputs();
    inputValues.reserve(inputs.size());

    size_t minArraySize = 0;
    size_t maxArraySize = 0;
    for (size_t i = 0; i < inputs.size(); i++) {
        Value evalExpr = inputs[i].get()->evaluate(root, variables);
        if (evalExpr.nullish()) {
            return Value(BSONNULL);
        }

        uassert(34468,
                str::stream() << "$zip found a non-array expression in input: "
                              << evalExpr.toString(),
                evalExpr.isArray());

        inputValues.push_back(evalExpr.getArray());

        size_t arraySize = evalExpr.getArrayLength();

        if (i == 0) {
            minArraySize = arraySize;
            maxArraySize = arraySize;
        } else {
            auto arraySizes = std::minmax({minArraySize, arraySize, maxArraySize});
            minArraySize = arraySizes.first;
            maxArraySize = arraySizes.second;
        }
    }

    std::vector<Value> evaluatedDefaults(inputs.size(), Value(BSONNULL));

    // If we need default values, evaluate each expression.
    if (minArraySize != maxArraySize) {
        auto& defaults = expr.getDefaults();
        for (size_t i = 0; i < defaults.size(); i++) {
            evaluatedDefaults[i] = defaults[i].get()->evaluate(root, variables);
        }
    }

    size_t outputLength = expr.getUseLongestLength() ? maxArraySize : minArraySize;

    // The final output array, e.g. [[1, 2, 3], [2, 3, 4]].
    std::vector<Value> output;

    // Used to construct each array in the output, e.g. [1, 2, 3].
    std::vector<Value> outputChild;

    output.reserve(outputLength);
    outputChild.reserve(inputs.size());

    for (size_t row = 0; row < outputLength; row++) {
        outputChild.clear();
        for (size_t col = 0; col < inputs.size(); col++) {
            if (inputValues[col].size() > row) {
                // Add the value from the appropriate input array.
                outputChild.push_back(inputValues[col][row]);
            } else {
                // Add the corresponding default value.
                outputChild.push_back(evaluatedDefaults[col]);
            }
        }
        output.push_back(Value(outputChild));
    }

    return Value(std::move(output));
}

}  // namespace exec::expression
}  // namespace mongo
