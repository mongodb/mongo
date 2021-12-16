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

#include "mongo/db/sorter/sorted_data_iterator.h"
#include "mongo/db/sorter/util.h"

namespace mongo::sorter {
/**
 * Merge-sorts results from 0 or more FileIterators, all of which should be iterating over sorted
 * ranges within the same file.
 */
template <typename Key, typename Value>
class MergeIterator : public SortedDataIterator<Key, Value> {
public:
    using Base = SortedDataIterator<Key, Value>;
    using Data = typename Base::Data;
    using CompFn = std::function<int(const Data&, const Data&)>;

    MergeIterator(const std::vector<std::unique_ptr<Base>>& iters,
                  unsigned long long limit,
                  const CompFn& comp)
        : _remaining(limit ? limit : std::numeric_limits<unsigned long long>::max()),
          _greater([comp](const std::unique_ptr<Stream>& lhs, const std::unique_ptr<Stream>& rhs) {
              dassertCompIsSane(comp, lhs->peek(), rhs->peek());
              int result = comp(lhs->peek(), rhs->peek());
              return result ? result > 0 : lhs->num() > rhs->num();
          }) {
        for (size_t i = 0; i < iters.size(); ++i) {
            if (iters[i]->more()) {
                _heap.push_back(std::make_unique<Stream>(iters[i].get(), i));
            }
        }

        if (_heap.empty()) {
            _remaining = 0;
            return;
        }

        std::make_heap(_heap.begin(), _heap.end(), _greater);
        std::pop_heap(_heap.begin(), _heap.end(), _greater);
        _current = std::move(_heap.back());
        _heap.pop_back();
    }

    ~MergeIterator() {
        _current.reset();
        _heap.clear();
    }

    bool more() const {
        return _remaining > 0 && (_first || !_heap.empty() || _current->more());
    }

    Data next() {
        invariant(more());

        --_remaining;

        if (_first) {
            _first = false;
            return _current->current();
        }

        if (!_current->advance()) {
            invariant(!_heap.empty());
            std::pop_heap(_heap.begin(), _heap.end(), _greater);
            _current = std::move(_heap.back());
            _heap.pop_back();
        } else if (!_heap.empty() && _greater(_current, _heap.front())) {
            std::pop_heap(_heap.begin(), _heap.end(), _greater);
            _current.swap(_heap.back());
            std::push_heap(_heap.begin(), _heap.end(), _greater);
        }

        return _current->current();
    }


private:
    class Stream {
    public:
        Stream(Base* data, size_t num) : _data(data), _current(data->next()), _num(num) {}

        const Data& peek() const {
            return _current;
        }

        Data current() {
            return std::move(_current);
        }

        bool more() const {
            return _data->more();
        }

        bool advance() {
            if (!_data->more()) {
                return false;
            }

            _current = std::move(_data->next());
            return true;
        }

        size_t num() const {
            return _num;
        }

    private:
        Base* _data;
        Data _current;
        size_t _num;
    };

    unsigned long long _remaining;
    bool _first = true;
    std::unique_ptr<Stream> _current;
    std::vector<std::unique_ptr<Stream>> _heap;  // MinHeap
    std::function<bool(const std::unique_ptr<Stream>& lhs, const std::unique_ptr<Stream>& rhs)>
        _greater;
};
}  // namespace mongo::sorter
