/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/sorter/sorter.h"

#include "mongo/db/sorter/single_elem_iterator.h"
#include "mongo/db/sorter/util.h"
#include "mongo/util/assert_util.h"

namespace mongo::sorter {
template <typename Key, typename Value>
class LimitOneSorter : public Sorter<Key, Value> {
public:
    using Base = Sorter<Key, Value>;
    using Data = typename Base::Data;
    using Iterator = typename Base::Iterator;
    using CompFn = typename Base::CompFn;

    using Base::_comp;
    using Base::_done;
    using Base::_numSorted;
    using Base::_totalDataSizeSorted;

    explicit LimitOneSorter(const CompFn& comp) : Base(comp) {}

    void add(const Key& key, const Value& val) {
        invariant(!_done);

        ++_numSorted;
        _totalDataSizeSorted += key.memUsageForSorter() + val.memUsageForSorter();

        Data contender{key, val};

        if (_haveData) {
            dassertCompIsSane(_comp, _best, contender);
            if (_comp(_best, contender) <= 0) {
                return;
            }
        } else {
            _haveData = true;
        }

        _best = {contender.first.getOwned(), contender.second.getOwned()};
    }

    std::unique_ptr<Iterator> done(typename Iterator::ReturnPolicy returnPolicy) {
        _done = true;
        return _haveData ? std::make_unique<SingleElemIterator<Key, Value>>(_best, returnPolicy)
                         : std::make_unique<SingleElemIterator<Key, Value>>(returnPolicy);
    }

private:
    Data _best;
    bool _haveData = false;
};
}  // namespace mongo::sorter
