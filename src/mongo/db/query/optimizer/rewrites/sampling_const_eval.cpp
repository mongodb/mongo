/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/query/optimizer/rewrites/sampling_const_eval.h"

#include "mongo/db/query/optimizer/algebra/operator.h"

namespace mongo::optimizer {
bool SamplingConstEval::optimize(ABT& n) {
    _changed = false;
    algebra::transport<true>(n, *this);
    return _changed;
}

void SamplingConstEval::constFold(ABT& n) {
    SamplingConstEval{}.optimize(n);
}

void SamplingConstEval::transport(ABT& n, const LambdaApplication& app, ABT& lam, ABT& arg) {
    // If the 'lam' expression is LambdaAbstraction then we can do the inplace beta reduction.
    if (auto lambda = lam.cast<LambdaAbstraction>(); lambda) {
        auto result = make<Let>(lambda->varName(),
                                std::exchange(arg, make<Blackhole>()),
                                std::exchange(lambda->getBody(), make<Blackhole>()));

        swapAndUpdate(n, std::move(result));
    }
}

void SamplingConstEval::swapAndUpdate(ABT& n, ABT newN) {
    // Do the swap.
    std::swap(n, newN);
    _changed = true;
}
}  // namespace mongo::optimizer
