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

#include "mongo/db/query/optimizer/rewrites/path_lower.h"


namespace mongo::optimizer {
bool EvalPathLowering::optimize(ABT& n) {
    _changed = false;

    algebra::transport<true>(n, *this);

    if (_changed) {
        _env.rebuild(n);
    }

    return _changed;
}

void EvalPathLowering::transport(ABT& n, const PathConstant&, ABT& c) {
    n = make<LambdaAbstraction>(_prefixId.getNextId("_"), std::exchange(c, make<Blackhole>()));
    _changed = true;
}

void EvalPathLowering::transport(ABT& n, const PathIdentity&) {
    const std::string& name = _prefixId.getNextId("x");

    n = make<LambdaAbstraction>(name, make<Variable>(name));
    _changed = true;
}

void EvalPathLowering::transport(ABT& n, const PathLambda&, ABT& lam) {
    n = std::exchange(lam, make<Blackhole>());
    _changed = true;
}

void EvalPathLowering::transport(ABT& n, const PathDefault&, ABT& c) {
    // if (exists(x), x, c)
    const std::string& name = _prefixId.getNextId("valDefault");

    n = make<LambdaAbstraction>(
        name,
        make<If>(make<FunctionCall>("exists", makeSeq(make<Variable>(name))),
                 make<Variable>(name),
                 std::exchange(c, make<Blackhole>())));
    _changed = true;
}

void EvalPathLowering::transport(ABT& n, const PathCompare&, ABT& c) {
    uasserted(6624132, "cannot lower compare in projection");
}

void EvalPathLowering::transport(ABT& n, const PathGet& p, ABT& inner) {
    const std::string& name = _prefixId.getNextId("inputGet");

    n = make<LambdaAbstraction>(
        name,
        make<LambdaApplication>(
            std::exchange(inner, make<Blackhole>()),
            make<FunctionCall>("getField",
                               makeSeq(make<Variable>(name), Constant::str(p.name())))));
    _changed = true;
}

void EvalPathLowering::transport(ABT& n, const PathDrop& drop) {
    // if (isObject(x), dropFields(x,...) , x)
    // Alternatively, we can implement a special builtin function that does the comparison and drop.
    const std::string& name = _prefixId.getNextId("valDrop");

    std::vector<ABT> params;
    params.emplace_back(make<Variable>(name));
    for (const auto& fieldName : drop.getNames()) {
        params.emplace_back(Constant::str(fieldName));
    }

    n = make<LambdaAbstraction>(
        name,
        make<If>(make<FunctionCall>("isObject", makeSeq(make<Variable>(name))),
                 make<FunctionCall>("dropFields", std::move(params)),
                 make<Variable>(name)));
    _changed = true;
}

void EvalPathLowering::transport(ABT& n, const PathKeep& keep) {
    // if (isObject(x), keepFields(x,...) , x)
    // Alternatively, we can implement a special builtin function that does the comparison and drop.
    const std::string& name = _prefixId.getNextId("valKeep");

    std::vector<ABT> params;
    params.emplace_back(make<Variable>(name));
    for (const auto& fieldName : keep.getNames()) {
        params.emplace_back(Constant::str(fieldName));
    }

    n = make<LambdaAbstraction>(
        name,
        make<If>(make<FunctionCall>("isObject", makeSeq(make<Variable>(name))),
                 make<FunctionCall>("keepFields", std::move(params)),
                 make<Variable>(name)));
    _changed = true;
}

void EvalPathLowering::transport(ABT& n, const PathObj&) {
    // if (isObject(x), x, Nothing)
    const std::string& name = _prefixId.getNextId("valObj");

    n = make<LambdaAbstraction>(
        name,
        make<If>(make<FunctionCall>("isObject", makeSeq(make<Variable>(name))),
                 make<Variable>(name),
                 Constant::nothing()));
    _changed = true;
}

void EvalPathLowering::transport(ABT& n, const PathArr&) {
    // if (isArray(x), x, Nothing)
    const std::string& name = _prefixId.getNextId("valArr");

    n = make<LambdaAbstraction>(
        name,
        make<If>(make<FunctionCall>("isArray", makeSeq(make<Variable>(name))),
                 make<Variable>(name),
                 Constant::nothing()));

    _changed = true;
}

void EvalPathLowering::transport(ABT& n, const PathTraverse& p, ABT& inner) {
    // TODO: SERVER-67306. Allow single-level traverse under EvalPath.

    uassert(6624167,
            "Currently we allow only multi-level traversal under EvalPath",
            p.getMaxDepth() == PathTraverse::kUnlimited);

    const std::string& name = _prefixId.getNextId("valTraverse");

    n = make<LambdaAbstraction>(name,
                                make<FunctionCall>("traverseP",
                                                   makeSeq(make<Variable>(name),
                                                           std::exchange(inner, make<Blackhole>()),
                                                           Constant::nothing())));
    _changed = true;
}

void EvalPathLowering::transport(ABT& n, const PathField& p, ABT& inner) {
    const std::string& name = _prefixId.getNextId("inputField");
    const std::string& val = _prefixId.getNextId("valField");

    n = make<LambdaAbstraction>(
        name,
        make<Let>(
            val,
            make<LambdaApplication>(
                std::exchange(inner, make<Blackhole>()),
                make<FunctionCall>("getField",
                                   makeSeq(make<Variable>(name), Constant::str(p.name())))),
            make<If>(
                make<BinaryOp>(Operations::Or,
                               make<FunctionCall>("exists", makeSeq(make<Variable>(val))),
                               make<FunctionCall>("isObject", makeSeq(make<Variable>(name)))),
                make<FunctionCall>(
                    "setField",
                    makeSeq(make<Variable>(name), Constant::str(p.name()), make<Variable>(val))),
                make<Variable>(name))));

    _changed = true;
}

void EvalPathLowering::transport(ABT& n, const PathComposeM&, ABT& p1, ABT& p2) {
    // p1 * p2 -> (p2 (p1 input))
    const std::string& name = _prefixId.getNextId("inputComposeM");

    n = make<LambdaAbstraction>(
        name,
        make<LambdaApplication>(
            std::exchange(p2, make<Blackhole>()),
            make<LambdaApplication>(std::exchange(p1, make<Blackhole>()), make<Variable>(name))));

    _changed = true;
}

void EvalPathLowering::transport(ABT& n, const PathComposeA&, ABT& p1, ABT& p2) {
    uasserted(6624133, "cannot lower additive composite in projection");
}

void EvalPathLowering::transport(ABT& n, const EvalPath&, ABT& path, ABT& input) {
    // In order to completely dissolve EvalPath the incoming path must be lowered to an expression
    // (lambda).
    uassert(6624134, "Incomplete evalpath lowering", path.is<LambdaAbstraction>());

    n = make<LambdaApplication>(std::exchange(path, make<Blackhole>()),
                                std::exchange(input, make<Blackhole>()));

    _changed = true;
}

bool EvalFilterLowering::optimize(ABT& n) {
    _changed = false;

    algebra::transport<true>(n, *this);

    if (_changed) {
        _env.rebuild(n);
    }

    return _changed;
}

void EvalFilterLowering::transport(ABT& n, const PathConstant&, ABT& c) {
    n = make<LambdaAbstraction>(_prefixId.getNextId("_"), std::exchange(c, make<Blackhole>()));
    _changed = true;
}

void EvalFilterLowering::transport(ABT& n, const PathIdentity&) {
    n = make<LambdaAbstraction>(_prefixId.getNextId("_"), Constant::boolean(true));
    _changed = true;

    // TODO - do we need an identity element for the additive composition? i.e. false constant
    // Or should Identity be left undefined and removed by the PathFuse?
}

void EvalFilterLowering::transport(ABT& n, const PathLambda&, ABT& lam) {
    n = std::exchange(lam, make<Blackhole>());
    _changed = true;
}

void EvalFilterLowering::transport(ABT& n, const PathDefault&, ABT& c) {
    const std::string& name = _prefixId.getNextId("valDefault");

    n = make<LambdaAbstraction>(
        name,
        make<If>(make<FunctionCall>("exists", makeSeq(make<Variable>(name))),
                 make<UnaryOp>(Operations::Not, c),
                 c));

    _changed = true;
}

void EvalFilterLowering::transport(ABT& n, const PathCompare& cmp, ABT& c) {
    const std::string& name = _prefixId.getNextId("valCmp");

    if (cmp.op() == Operations::Eq) {
        n = make<LambdaAbstraction>(
            name,
            make<BinaryOp>(cmp.op(), make<Variable>(name), std::exchange(c, make<Blackhole>())));
    } else if (cmp.op() == Operations::EqMember) {
        n = make<LambdaAbstraction>(
            name,
            make<If>(make<FunctionCall>("isArray", makeSeq(c)),
                     make<FunctionCall>("isMember", makeSeq(make<Variable>(name), c)),
                     make<BinaryOp>(Operations::Eq, make<Variable>(name), c)));
    } else {
        n = make<LambdaAbstraction>(
            name,
            make<BinaryOp>(cmp.op(),
                           make<BinaryOp>(Operations::Cmp3w,
                                          make<Variable>(name),
                                          std::exchange(c, make<Blackhole>())),
                           Constant::int64(0)));
    }

    _changed = true;
}

void EvalFilterLowering::transport(ABT& n, const PathGet& p, ABT& inner) {
    const std::string& name = _prefixId.getNextId("inputGet");

    int idx;
    bool isNumber = NumberParser{}(p.name(), &idx).isOK();
    n = make<LambdaAbstraction>(
        name,
        make<LambdaApplication>(
            std::exchange(inner, make<Blackhole>()),
            make<FunctionCall>(isNumber ? "getFieldOrElement" : "getField",
                               makeSeq(make<Variable>(name), Constant::str(p.name())))));
    _changed = true;
}

void EvalFilterLowering::transport(ABT& n, const PathDrop& drop) {
    uasserted(6624136, "cannot lower drop in filter");
}

void EvalFilterLowering::transport(ABT& n, const PathKeep& keep) {
    uasserted(6624137, "cannot lower keep in filter");
}

void EvalFilterLowering::transport(ABT& n, const PathObj&) {
    const std::string& name = _prefixId.getNextId("valObj");
    n = make<LambdaAbstraction>(name,
                                make<FunctionCall>("isObject", makeSeq(make<Variable>(name))));
    _changed = true;
}

void EvalFilterLowering::transport(ABT& n, const PathArr&) {
    const std::string& name = _prefixId.getNextId("valArr");
    n = make<LambdaAbstraction>(name, make<FunctionCall>("isArray", makeSeq(make<Variable>(name))));
    _changed = true;
}

void EvalFilterLowering::prepare(ABT& n, const PathTraverse& t) {
    int idx;
    // This is a bad hack that detect if a child is number path element
    if (auto child = t.getPath().cast<PathGet>();
        child && NumberParser{}(child->name(), &idx).isOK()) {
        _traverseStack.emplace_back(n.ref());
    }
}

void EvalFilterLowering::transport(ABT& n, const PathTraverse& p, ABT& inner) {
    // TODO: SERVER-67306. Allow multi-level traverse under EvalFilter.

    uassert(6624166,
            "Currently we allow only single-level traversal under EvalFilter",
            p.getMaxDepth() == PathTraverse::kSingleLevel);

    const std::string& name = _prefixId.getNextId("valTraverse");

    ABT numberPath = Constant::boolean(false);
    if (!_traverseStack.empty() && _traverseStack.back() == n.ref()) {
        numberPath = Constant::boolean(true);
        _traverseStack.pop_back();
    }
    n = make<LambdaAbstraction>(name,
                                make<FunctionCall>("traverseF",
                                                   makeSeq(make<Variable>(name),
                                                           std::exchange(inner, make<Blackhole>()),
                                                           std::move(numberPath))));

    _changed = true;
}

void EvalFilterLowering::transport(ABT& n, const PathField& p, ABT& inner) {
    uasserted(6624140, "cannot lower field in filter");
}

void EvalFilterLowering::transport(ABT& n, const PathComposeM&, ABT& p1, ABT& p2) {
    const std::string& name = _prefixId.getNextId("inputComposeM");

    n = make<LambdaAbstraction>(
        name,
        make<If>(
            make<FunctionCall>("fillEmpty",
                               makeSeq(make<LambdaApplication>(std::exchange(p1, make<Blackhole>()),
                                                               make<Variable>(name)),
                                       Constant::boolean(false))),
            make<LambdaApplication>(std::exchange(p2, make<Blackhole>()), make<Variable>(name)),
            Constant::boolean(false)));

    _changed = true;
}

void EvalFilterLowering::transport(ABT& n, const PathComposeA&, ABT& p1, ABT& p2) {
    const std::string& name = _prefixId.getNextId("inputComposeA");

    n = make<LambdaAbstraction>(
        name,
        make<If>(
            make<FunctionCall>("fillEmpty",
                               makeSeq(make<LambdaApplication>(std::exchange(p1, make<Blackhole>()),
                                                               make<Variable>(name)),
                                       Constant::boolean(false))),
            Constant::boolean(true),
            make<LambdaApplication>(std::exchange(p2, make<Blackhole>()), make<Variable>(name))));

    _changed = true;
}

void EvalFilterLowering::transport(ABT& n, const EvalFilter&, ABT& path, ABT& input) {
    // In order to completely dissolve EvalFilter the incoming path must be lowered to an expression
    // (lambda).
    uassert(6624141, "Incomplete evalfilter lowering", path.is<LambdaAbstraction>());

    n = make<LambdaApplication>(std::exchange(path, make<Blackhole>()),
                                std::exchange(input, make<Blackhole>()));

    _changed = true;
}

void PathLowering::transport(ABT& n, const EvalPath&, ABT&, ABT&) {
    _changed = _changed || _project.optimize(n);
}

void PathLowering::transport(ABT& n, const EvalFilter&, ABT&, ABT&) {
    _changed = _changed || _filter.optimize(n);
}

bool PathLowering::optimize(ABT& n) {
    _changed = false;

    algebra::transport<true>(n, *this);

    // TODO investigate why we crash when this is removed. It should not be needed here.
    if (_changed) {
        _env.rebuild(n);
    }

    return _changed;
}


}  // namespace mongo::optimizer
