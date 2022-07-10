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
 * A constant path element - any input value is disregarded and replaced by the result of (constant)
 * expression.
 *
 * It could also be expressed as lambda that ignores its input: \ _ . c
 */
class PathConstant final : public Operator<PathConstant, 1>, public PathSyntaxSort {
    using Base = Operator<PathConstant, 1>;

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
class PathLambda final : public Operator<PathLambda, 1>, public PathSyntaxSort {
    using Base = Operator<PathLambda, 1>;

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
 * An identity path element - the input is not disturbed at all.
 *
 * It could also be expressed as lambda : \ x . x
 */
class PathIdentity final : public Operator<PathIdentity, 0>, public PathSyntaxSort {
public:
    bool operator==(const PathIdentity& other) const {
        return true;
    }
};

/**
 * A default path element - If input is Nothing then return the result of expression (assumed to
 * return non-Nothing) otherwise return the input undisturbed.
 */
class PathDefault final : public Operator<PathDefault, 1>, public PathSyntaxSort {
    using Base = Operator<PathDefault, 1>;

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
 * A comparison path element - the input value is compared to the result of (constant) expression.
 * The actual semantics (return value) depends on what component is evaluating the paths (i.e.
 * filter or project).
 */
class PathCompare : public Operator<PathCompare, 1>, public PathSyntaxSort {
    using Base = Operator<PathCompare, 1>;

    Operations _cmp;

public:
    PathCompare(Operations inCmp, ABT inVal) : Base(std::move(inVal)), _cmp(inCmp) {
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
 * A drop path element - drops fields from the input if it is an object otherwise returns it
 * undisturbed.
 */
class PathDrop final : public Operator<PathDrop, 0>, public PathSyntaxSort {
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
 * A keep path element - keeps fields from the input if it is an object otherwise returns it
 * undisturbed.
 */
class PathKeep final : public Operator<PathKeep, 0>, public PathSyntaxSort {
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
 * Returns input undisturbed if it is an object otherwise return Nothing.
 */
class PathObj final : public Operator<PathObj, 0>, public PathSyntaxSort {
public:
    bool operator==(const PathObj& other) const {
        return true;
    }
};

/**
 * Returns input undisturbed if it is an array otherwise return Nothing.
 */
class PathArr final : public Operator<PathArr, 0>, public PathSyntaxSort {
public:
    bool operator==(const PathArr& other) const {
        return true;
    }
};

/**
 * A traverse path element - apply the inner path to every element of an array.
 * Specifies a maximum depth of the traversal: how many nested arrays are we allowed to descend. "0"
 * specifies unlimited depth.
 */
class PathTraverse final : public Operator<PathTraverse, 1>, public PathSyntaxSort {
    using Base = Operator<PathTraverse, 1>;

public:
    static constexpr size_t kUnlimited = 0;
    static constexpr size_t kSingleLevel = 1;

    PathTraverse(ABT inPath, const size_t maxDepth) : Base(std::move(inPath)), _maxDepth(maxDepth) {
        assertPathSort(getPath());

        uassert(6743600,
                "For now only 0 and 1 is supported for maxDepth",
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
 * A field path element - apply the inner path to an object field.
 */
class PathField final : public Operator<PathField, 1>, public PathSyntaxSort {
    using Base = Operator<PathField, 1>;
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
 * A get path element - similar to the path element.
 */
class PathGet final : public Operator<PathGet, 1>, public PathSyntaxSort {
    using Base = Operator<PathGet, 1>;
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
 */
class PathComposeM final : public Operator<PathComposeM, 2>, public PathSyntaxSort {
    using Base = Operator<PathComposeM, 2>;

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
 * An additive composition path element.
 */
class PathComposeA final : public Operator<PathComposeA, 2>, public PathSyntaxSort {
    using Base = Operator<PathComposeA, 2>;

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
