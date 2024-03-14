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

#include <set>
#include <utility>

#include <absl/container/node_hash_map.h>

#include "mongo/base/parse_number.h"
#include "mongo/base/status.h"
#include "mongo/db/query/optimizer/algebra/operator.h"
#include "mongo/db/query/optimizer/comparison_op.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/rewrites/proj_spec_lower.h"
#include "mongo/db/query/optimizer/utils/strong_alias.h"
#include "mongo/util/assert_util.h"

namespace mongo::optimizer {

static ABT fillEmpty(ABT n, ABT emptyVal) {
    return make<BinaryOp>(Operations::FillEmpty, std::move(n), std::move(emptyVal));
}

bool EvalPathLowering::optimize(ABT& n) {
    // Try to lower directly into a function call to makeBsonObj.
    auto inputArgs = generateMakeObjArgs(n);
    if (inputArgs.empty()) {
        algebra::transport<true>(n, *this);

    } else {
        // We were able to generate arguments for a makeObj/makeBsonObj call! Now we need to lower
        // them. We want to lower all arguments as usual. Note that we are intentionally lowering
        // path components like PathLambda to LambdaAbstractions here.
        for (auto& arg : inputArgs) {
            algebra::transport<true>(arg, *this);
        }

        // TODO SERVER-62830: detect if we need to lower to makeObj instead.
        n = make<FunctionCall>("makeBsonObj", std::move(inputArgs));
        _changed = true;
    }

    return _changed;
}

void EvalPathLowering::transport(ABT& n, const PathConstant&, ABT& c) {
    n = make<LambdaAbstraction>(_prefixId.getNextId("_"), std::exchange(c, make<Blackhole>()));
    _changed = true;
}

void EvalPathLowering::transport(ABT& n, const PathIdentity&) {
    const ProjectionName name{_prefixId.getNextId("x")};

    n = make<LambdaAbstraction>(name, make<Variable>(name));
    _changed = true;
}

void EvalPathLowering::transport(ABT& n, const PathLambda&, ABT& lam) {
    n = std::exchange(lam, make<Blackhole>());
    _changed = true;
}

void EvalPathLowering::transport(ABT& n, const PathDefault&, ABT& c) {
    // if (exists(x), x, c)
    const ProjectionName name{_prefixId.getNextId("valDefault")};

    n = make<LambdaAbstraction>(
        name,
        make<If>(make<FunctionCall>("exists", makeSeq(make<Variable>(name))),
                 make<Variable>(name),
                 std::exchange(c, make<Blackhole>())));
    _changed = true;
}

void EvalPathLowering::transport(ABT& n, const PathCompare&, ABT& c) {
    tasserted(6624132, "cannot lower compare in projection");
}

void EvalPathLowering::transport(ABT& n, const PathGet& p, ABT& inner) {
    const ProjectionName name{_prefixId.getNextId("inputGet")};

    n = make<LambdaAbstraction>(
        name,
        make<LambdaApplication>(
            std::exchange(inner, make<Blackhole>()),
            make<FunctionCall>("getField",
                               makeSeq(make<Variable>(name), Constant::str(p.name().value())))));
    _changed = true;
}

void EvalPathLowering::transport(ABT& n, const PathDrop& drop) {
    // if (isObject(x), dropFields(x,...) , x)
    // Alternatively, we can implement a special builtin function that does the comparison and drop.
    const ProjectionName name{_prefixId.getNextId("valDrop")};

    std::vector<ABT> params;
    params.emplace_back(make<Variable>(name));
    for (const auto& fieldName : drop.getNames()) {
        params.emplace_back(Constant::str(fieldName.value()));
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
    const ProjectionName name{_prefixId.getNextId("valKeep")};

    std::vector<ABT> params;
    params.emplace_back(make<Variable>(name));
    for (const auto& fieldName : keep.getNames()) {
        params.emplace_back(Constant::str(fieldName.value()));
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
    const ProjectionName name{_prefixId.getNextId("valObj")};

    n = make<LambdaAbstraction>(
        name,
        make<If>(make<FunctionCall>("isObject", makeSeq(make<Variable>(name))),
                 make<Variable>(name),
                 Constant::nothing()));
    _changed = true;
}

void EvalPathLowering::transport(ABT& n, const PathArr&) {
    // if (isArray(x), x, Nothing)
    const ProjectionName name{_prefixId.getNextId("valArr")};

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

    const ProjectionName name{_prefixId.getNextId("valTraverse")};

    n = make<LambdaAbstraction>(name,
                                make<FunctionCall>("traverseP",
                                                   makeSeq(make<Variable>(name),
                                                           std::exchange(inner, make<Blackhole>()),
                                                           Constant::nothing())));
    _changed = true;
}

void EvalPathLowering::transport(ABT& n, const PathField& p, ABT& inner) {
    const ProjectionName name{_prefixId.getNextId("inputField")};
    const ProjectionName val{_prefixId.getNextId("valField")};

    n = make<LambdaAbstraction>(
        name,
        make<Let>(
            val,
            make<LambdaApplication>(
                std::exchange(inner, make<Blackhole>()),
                make<FunctionCall>("getField",
                                   makeSeq(make<Variable>(name), Constant::str(p.name().value())))),
            make<If>(make<BinaryOp>(Operations::Or,
                                    make<FunctionCall>("exists", makeSeq(make<Variable>(val))),
                                    make<FunctionCall>("isObject", makeSeq(make<Variable>(name)))),
                     make<FunctionCall>("setField",
                                        makeSeq(make<Variable>(name),
                                                Constant::str(p.name().value()),
                                                make<Variable>(val))),
                     make<Variable>(name))));

    _changed = true;
}

void EvalPathLowering::transport(ABT& n, const PathComposeM&, ABT& p1, ABT& p2) {
    // p1 * p2 -> (p2 (p1 input))
    const ProjectionName name{_prefixId.getNextId("inputComposeM")};

    n = make<LambdaAbstraction>(
        name,
        make<LambdaApplication>(
            std::exchange(p2, make<Blackhole>()),
            make<LambdaApplication>(std::exchange(p1, make<Blackhole>()), make<Variable>(name))));

    _changed = true;
}

void EvalPathLowering::transport(ABT& n, const PathComposeA&, ABT& p1, ABT& p2) {
    tasserted(6624133, "cannot lower additive composite in projection");
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

    return _changed;
}

void EvalFilterLowering::transport(ABT& n, const PathConstant&, ABT& c) {
    n = make<LambdaAbstraction>(_prefixId.getNextId("_"), std::exchange(c, make<Blackhole>()));
    _changed = true;
}

void EvalFilterLowering::transport(ABT& n, const PathIdentity&) {
    tasserted(6893500, "PathIdentity not allowed in EvalFilter (match) context");
}

void EvalFilterLowering::transport(ABT& n, const PathLambda&, ABT& lam) {
    n = std::exchange(lam, make<Blackhole>());
    _changed = true;
}

void EvalFilterLowering::transport(ABT& n, const PathDefault&, ABT& c) {
    const ProjectionName name{_prefixId.getNextId("valDefault")};

    n = make<LambdaAbstraction>(
        name,
        make<If>(make<FunctionCall>("exists", makeSeq(make<Variable>(name))),
                 make<UnaryOp>(Operations::Not, c),
                 c));

    _changed = true;
}

void EvalFilterLowering::transport(ABT& n, const PathCompare& cmp, ABT& c) {
    const ProjectionName name{_prefixId.getNextId("valCmp")};

    if (cmp.op() == Operations::EqMember) {
        // If c is not an array we want EqMember to have the same behavior as Eq (for example:
        // EqMember (Const 5) should have the same behavior as Eq (Const 5)). Thus, we use cmp3w in
        // the non-array case in the lowering below.
        n = make<LambdaAbstraction>(
            name,
            make<If>(make<BinaryOp>(Operations::Or,
                                    make<FunctionCall>("isArray", makeSeq(c)),
                                    make<FunctionCall>("isInListData", makeSeq(c))),
                     make<BinaryOp>(Operations::EqMember, make<Variable>(name), c),
                     make<BinaryOp>(Operations::Eq,
                                    make<BinaryOp>(Operations::Cmp3w, make<Variable>(name), c),
                                    Constant::int64(0))));
    } else {
        n = make<LambdaAbstraction>(
            name,
            make<BinaryOp>(cmp.op(), make<Variable>(name), std::exchange(c, make<Blackhole>())));
    }

    _changed = true;
}

void EvalFilterLowering::transport(ABT& n, const PathGet& p, ABT& inner) {
    const ProjectionName name{_prefixId.getNextId("inputGet")};

    int idx;
    bool isNumber = NumberParser{}(p.name().value(), &idx).isOK();
    n = make<LambdaAbstraction>(
        name,
        make<LambdaApplication>(
            std::exchange(inner, make<Blackhole>()),
            make<FunctionCall>(isNumber ? "getFieldOrElement" : "getField",
                               makeSeq(make<Variable>(name), Constant::str(p.name().value())))));
    _changed = true;
}

void EvalFilterLowering::transport(ABT& n, const PathDrop& drop) {
    tasserted(6624136, "cannot lower drop in filter");
}

void EvalFilterLowering::transport(ABT& n, const PathKeep& keep) {
    tasserted(6624137, "cannot lower keep in filter");
}

void EvalFilterLowering::transport(ABT& n, const PathObj&) {
    const ProjectionName name{_prefixId.getNextId("valObj")};
    n = make<LambdaAbstraction>(name,
                                make<FunctionCall>("isObject", makeSeq(make<Variable>(name))));
    _changed = true;
}

void EvalFilterLowering::transport(ABT& n, const PathArr&) {
    const ProjectionName name{_prefixId.getNextId("valArr")};
    n = make<LambdaAbstraction>(name, make<FunctionCall>("isArray", makeSeq(make<Variable>(name))));
    _changed = true;
}

void EvalFilterLowering::prepare(ABT& n, const PathTraverse& t) {
    int idx;
    // This is a bad hack that detect if a child is number path element
    if (auto child = t.getPath().cast<PathGet>();
        child && NumberParser{}(child->name().value(), &idx).isOK()) {
        _traverseStack.emplace_back(n.ref());
    }
}

void EvalFilterLowering::transport(ABT& n, const PathTraverse& p, ABT& inner) {
    // TODO: SERVER-67306. Allow multi-level traverse under EvalFilter.

    uassert(6624166,
            "Currently we allow only single-level traversal under EvalFilter",
            p.getMaxDepth() == PathTraverse::kSingleLevel);

    const ProjectionName name{_prefixId.getNextId("valTraverse")};

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
    tasserted(6624140, "cannot lower field in filter");
}

void EvalFilterLowering::transport(ABT& n, const PathComposeM&, ABT& p1, ABT& p2) {
    const ProjectionName name{_prefixId.getNextId("inputComposeM")};

    n = make<LambdaAbstraction>(
        name,
        make<If>(
            // If p1 is Nothing, then the expression will short-circuit and Nothing will be
            // propagated to this operator's parent, and eventually coerced to false.
            make<LambdaApplication>(std::exchange(p1, make<Blackhole>()), make<Variable>(name)),
            make<LambdaApplication>(std::exchange(p2, make<Blackhole>()), make<Variable>(name)),
            Constant::boolean(false)));

    _changed = true;
}

void EvalFilterLowering::transport(ABT& n, const PathComposeA&, ABT& p1, ABT& p2) {
    const ProjectionName name{_prefixId.getNextId("inputComposeA")};

    n = make<LambdaAbstraction>(
        name,
        make<If>(
            fillEmpty(
                make<LambdaApplication>(std::exchange(p1, make<Blackhole>()), make<Variable>(name)),
                Constant::boolean(false)),
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

    // Wrap EvalFilter in fillEmpty to coerce it to a boolean.
    n = fillEmpty(std::move(n), Constant::boolean(false));

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

    return _changed;
}


}  // namespace mongo::optimizer
