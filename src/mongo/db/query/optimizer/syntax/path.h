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

#include <ostream>
#include <unordered_set>

#include "mongo/db/query/optimizer/syntax/syntax.h"


namespace mongo::optimizer {

/**
 * Marker class for paths. Mutually exclusive with nodes and expressions.
 */
class PathSyntaxSort {};

/**
 * A constant path element - any input value is disregarded and replaced by the result of the child
 * expression. The child expression does not depend on the values unpacked by the path.
 *
 * It could also be expressed as lambda that ignores its input: \ _ . c
 */
class PathConstant final : public Operator<1>, public PathSyntaxSort {
    using Base = Operator<1>;

public:
    PathConstant(ABT inConstant) : Base(std::move(inConstant)) {
        assertExprSort(getConstant());
    }

    bool operator==(const PathConstant& other) const {
        return getConstant() == other.getConstant();
    }

    const ABT& getConstant() const {
        return get<0>();
    }

    ABT& getConstant() {
        return get<0>();
    }
};

/**
 * A lambda path element - the expression must be a single argument lambda. The lambda is applied
 * with the input value.
 */
class PathLambda final : public Operator<1>, public PathSyntaxSort {
    using Base = Operator<1>;

public:
    PathLambda(ABT inLambda) : Base(std::move(inLambda)) {
        assertExprSort(getLambda());
    }

    bool operator==(const PathLambda& other) const {
        return getLambda() == other.getLambda();
    }

    const ABT& getLambda() const {
        return get<0>();
    }
};

/**
 * An identity path element. Returns the input undisturbed. It can be expressed as lambda : \ x . x.
 *
 * Not permitted under EvalFilter.
 */
class PathIdentity final : public Operator<0>, public PathSyntaxSort {
public:
    bool operator==(const PathIdentity& other) const {
        return true;
    }
};

/**
 * A default path element - combines an existence check with a replacement step.
 * Under EvalPath:  If input is Nothing then return the result of the child expression, otherwise
 *      return the input undisturbed.
 * Under EvalFilter: If input is Nothing then return the result of the child expression, otherwise
 *      return the child expression negated.
 */
class PathDefault final : public Operator<1>, public PathSyntaxSort {
    using Base = Operator<1>;

public:
    PathDefault(ABT inDefault) : Base(std::move(inDefault)) {
        assertExprSort(getDefault());
    }

    bool operator==(const PathDefault& other) const {
        return getDefault() == other.getDefault();
    }

    const ABT& getDefault() const {
        return get<0>();
    }
};

/**
 * A comparison path element - compares the input value to the result of the child expression using
 * a comparison operator, and returns a boolean indicating the result of the comparison. The child
 * expression does not depend on the values unpacked by the path.
 *
 * Not permitted under EvalPath.
 */
class PathCompare : public Operator<1>, public PathSyntaxSort {
    using Base = Operator<1>;

    Operations _cmp;

public:
    PathCompare(Operations inCmp, ABT inVal) : Base(std::move(inVal)), _cmp(inCmp) {
        tassert(6684500, "Comparison op expected", isComparisonOp(_cmp));
        assertExprSort(getVal());
    }

    bool operator==(const PathCompare& other) const {
        return _cmp == other._cmp && getVal() == other.getVal();
    }

    auto op() const {
        return _cmp;
    }

    const ABT& getVal() const {
        return get<0>();
    }

    ABT& getVal() {
        return get<0>();
    }
};

/**
 * A drop path element - If the input is an object, drops the specified fields, otherwise returns
 * the input unmodified. The fields are treated as simple field paths.
 *
 * Not permitted under EvalFilter.
 */
class PathDrop final : public Operator<0>, public PathSyntaxSort {
public:
    using NameSet = std::set<std::string>;

    PathDrop(NameSet inNames) : _names(std::move(inNames)) {}

    bool operator==(const PathDrop& other) const {
        return _names == other._names;
    }

    const NameSet& getNames() const {
        return _names;
    }

private:
    const NameSet _names;
};

/**
 * A keep path element - If the input is an object, keeps the specified fields, otherwise returns
 * the input unmodified. The fields are treated as simple field paths.
 *
 * Not permitted in EvalFilter.
 */
class PathKeep final : public Operator<0>, public PathSyntaxSort {
public:
    using NameSet = std::set<std::string>;

    PathKeep(NameSet inNames) : _names(std::move(inNames)) {}

    bool operator==(const PathKeep other) const {
        return _names == other._names;
    }

    const NameSet& getNames() const {
        return _names;
    }

private:
    const NameSet _names;
};

/**
 * Combines an object type check with a potential replacement.
 * Under EvalPath: If input is an object then return it unmodified, otherwise return Nothing.
 * Under EvalFilter: If input is an object then return true, otherwise return false.
 */
class PathObj final : public Operator<0>, public PathSyntaxSort {
public:
    bool operator==(const PathObj& other) const {
        return true;
    }
};

/**
 * Combines an array type check with a potential replacement.
 * Under EvalPath: If input is an object then return it unmodified, otherwise return Nothing.
 * Under EvalFilter: If input is an object then return true, otherwise return false.
 */
class PathArr final : public Operator<0>, public PathSyntaxSort {
public:
    bool operator==(const PathArr& other) const {
        return true;
    }
};

/**
 * A traverse path element - if the input is not an array, applies the inner path to the input.
 * Otherwise, recursively evaluates on each element of the array.
 *
 * Under EvalPath: re-assembles the result from each element into an array, and returns the array.
 * Under EvalFilter: returns true if the inner path applied to any of the array elements is true.
 *
 * Specifies a maximum depth of the traversal: how many nested arrays are we allowed to descend. "0"
 * denotes unlimited depth.
 */
class PathTraverse final : public Operator<1>, public PathSyntaxSort {
    using Base = Operator<1>;

public:
    static constexpr size_t kUnlimited = 0;
    static constexpr size_t kSingleLevel = 1;

    PathTraverse(ABT inPath, const size_t maxDepth) : Base(std::move(inPath)), _maxDepth(maxDepth) {
        assertPathSort(getPath());

        // TODO SERVER-67306: Support different maxDepth values.
        tassert(6743600,
                "maxDepth must be either 0 or 1",
                maxDepth == kUnlimited || maxDepth == kSingleLevel);
    }

    bool operator==(const PathTraverse& other) const {
        return getPath() == other.getPath() && _maxDepth == other._maxDepth;
    }

    size_t getMaxDepth() const {
        return _maxDepth;
    }

    const ABT& getPath() const {
        return get<0>();
    }

    ABT& getPath() {
        return get<0>();
    }

private:
    const size_t _maxDepth;
};

/**
 * A field path element - models the act of setting a field in an object. Extracts the specified
 * field from the input and runs the inner path with that value following "get" semantics. Then,
 * 1. If its input is an object: sets the field in the input to the result.
 * 2. If the input is not an object: returns the input unmodified if the inner path returned
 *    Nothing, otherwise returns an object with the single field and the result as its value.
 *
 * Not permitted in EvalFilter.
 */
class PathField final : public Operator<1>, public PathSyntaxSort {
    using Base = Operator<1>;
    std::string _name;

public:
    PathField(std::string inName, ABT inPath) : Base(std::move(inPath)), _name(std::move(inName)) {
        assertPathSort(getPath());
    }

    bool operator==(const PathField& other) const {
        return _name == other._name && getPath() == other.getPath();
    }

    auto& name() const {
        return _name;
    }

    const ABT& getPath() const {
        return get<0>();
    }

    ABT& getPath() {
        return get<0>();
    }
};

/**
 * A get path element. If the input is an object and the specified field exists in it, gets the
 * value for the field, and returns the result of the inner path applied to the value. Otherwise,
 * returns the result of the inner path applied to Nothing.
 *
 * The specified field name is treated as a simple path.
 */
class PathGet final : public Operator<1>, public PathSyntaxSort {
    using Base = Operator<1>;
    std::string _name;

public:
    PathGet(std::string inName, ABT inPath) : Base(std::move(inPath)), _name(std::move(inName)) {
        assertPathSort(getPath());
    }

    bool operator==(const PathGet& other) const {
        return _name == other._name && getPath() == other.getPath();
    }

    auto& name() const {
        return _name;
    }

    const ABT& getPath() const {
        return get<0>();
    }

    ABT& getPath() {
        return get<0>();
    }
};

/**
 * A multiplicative composition path element.
 * Under EvalPath: evaluates the first inner path evaluated over the input, and then evaluates the
 * second inner path over that result.
 * Under EvalFilter: evaluates both inner paths over the input. Returns true if both inner paths
 * return true.
 */
class PathComposeM final : public Operator<2>, public PathSyntaxSort {
    using Base = Operator<2>;

public:
    PathComposeM(ABT inPath1, ABT inPath2) : Base(std::move(inPath1), std::move(inPath2)) {
        assertPathSort(getPath1());
        assertPathSort(getPath2());
    }

    bool operator==(const PathComposeM& other) const {
        return getPath1() == other.getPath1() && getPath2() == other.getPath2();
    }

    const ABT& getPath1() const {
        return get<0>();
    }

    const ABT& getPath2() const {
        return get<1>();
    }
};

/**
 * An additive composition path element. Runs the inner paths with the input and returns true if
 * either inner path returns true.
 *
 * Not permitted within EvalPath.
 */
class PathComposeA final : public Operator<2>, public PathSyntaxSort {
    using Base = Operator<2>;

public:
    PathComposeA(ABT inPath1, ABT inPath2) : Base(std::move(inPath1), std::move(inPath2)) {
        assertPathSort(getPath1());
        assertPathSort(getPath2());
    }

    bool operator==(const PathComposeA& other) const {
        return getPath1() == other.getPath1() && getPath2() == other.getPath2();
    }

    const ABT& getPath1() const {
        return get<0>();
    }

    const ABT& getPath2() const {
        return get<1>();
    }
};

}  // namespace mongo::optimizer
