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

#include "mongo/db/query/optimizer/cascades/memo_defs.h"

#include "mongo/db/query/optimizer/utils/abt_hash.h"


namespace mongo::optimizer::cascades {

size_t MemoNodeRefHash::operator()(const ABT::reference_type& nodeRef) const {
    // Compare delegator as well.
    return ABTHashGenerator::generate(nodeRef);
}

bool MemoNodeRefCompare::operator()(const ABT::reference_type& left,
                                    const ABT::reference_type& right) const {
    // Deep comparison.
    return left.follow() == right.follow();
}

ABT::reference_type OrderPreservingABTSet::at(const size_t index) const {
    return _vector.at(index).ref();
}

std::pair<size_t, bool> OrderPreservingABTSet::emplace_back(ABT node) {
    auto [index, found] = find(node.ref());
    if (found) {
        return {index, false};
    }

    const size_t id = _vector.size();
    _vector.emplace_back(std::move(node));
    _map.emplace(_vector.back().ref(), id);
    return {id, true};
}

std::pair<size_t, bool> OrderPreservingABTSet::find(ABT::reference_type node) const {
    auto it = _map.find(node);
    if (it == _map.end()) {
        return {0, false};
    }

    return {it->second, true};
}

void OrderPreservingABTSet::clear() {
    _map.clear();
    _vector.clear();
}

size_t OrderPreservingABTSet::size() const {
    return _vector.size();
}

const ABTVector& OrderPreservingABTSet::getVector() const {
    return _vector;
}

PhysOptimizationResult::PhysOptimizationResult()
    : PhysOptimizationResult(0, {}, CostType::kInfinity) {}

PhysOptimizationResult::PhysOptimizationResult(size_t index,
                                               properties::PhysProps physProps,
                                               CostType costLimit)
    : _index(index),
      _physProps(std::move(physProps)),
      _costLimit(std::move(costLimit)),
      _nodeInfo(),
      _rejectedNodeInfo() {}

}  // namespace mongo::optimizer::cascades
