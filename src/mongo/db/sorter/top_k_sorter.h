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

#include "mongo/db/sorter/spillable_sorter.h"

namespace mongo::sorter {
template <typename Key, typename Value>
class TopKSorter : public SpillableSorter<Key, Value> {
public:
    using Base = SpillableSorter<Key, Value>;
    using Data = typename Base::Data;
    using Iterator = typename Base::Iterator;
    using CompFn = typename Base::CompFn;
    using Settings = typename Base::Settings;

    using Base::_data;
    using Base::_done;
    using Base::_less;
    using Base::_memUsed;
    using Base::_numSorted;
    using Base::_options;
    using Base::_spill;
    using Base::_totalDataSizeSorted;

    TopKSorter(StringData name,
               const Options& options,
               const CompFn& comp,
               const Settings& settings)
        : Base(name, options, comp, settings) {
        // This also works with limit 1, but LimitOneSorter should be used instead for that case.
        invariant(options.limit > 1);

        // Preallocate a fixed sized vector of the required size if we don't expect it to have a
        // major impact on our memory budget. This is the common case with small limits.
        if (options.limit < std::min((options.maxMemoryUsageBytes / 10) /
                                         sizeof(typename decltype(_data)::value_type),
                                     _data.max_size())) {
            _data.reserve(options.limit);
        }
    }

    void add(const Key& key, const Value& val) {
        invariant(!_done);

        ++_numSorted;

        Data contender{key, val};

        if (_data.size() < _options.limit) {
            if (_haveCutoff && !_less(contender, _cutoff)) {
                return;
            }

            _data.emplace_back(contender.first.getOwned(), contender.second.getOwned());

            auto memUsage = key.memUsageForSorter() + val.memUsageForSorter();
            _memUsed += memUsage;
            _totalDataSizeSorted += memUsage;

            if (_data.size() == _options.limit) {
                std::make_heap(_data.begin(), _data.end(), _less);
            }

            if (_memUsed > _options.maxMemoryUsageBytes) {
                _spill();
            }

            return;
        }

        invariant(_data.size() == _options.limit);

        if (!_less(contender, _data.front())) {
            return;
        }

        // Remove the old worst pair and insert the contender, adjusting _memUsed.

        auto memUsage = key.memUsageForSorter() + val.memUsageForSorter();
        _memUsed += memUsage;
        _totalDataSizeSorted += memUsage;

        _memUsed -= _data.front().first.memUsageForSorter();
        _memUsed -= _data.front().second.memUsageForSorter();

        std::pop_heap(_data.begin(), _data.end(), _less);
        _data.back() = {contender.first.getOwned(), contender.second.getOwned()};
        std::push_heap(_data.begin(), _data.end(), _less);

        if (_memUsed > _options.maxMemoryUsageBytes) {
            _spill();
        }
    }

private:
    void _sort() {
        if (_data.size() == _options.limit) {
            std::sort_heap(_data.begin(), _data.end(), _less);
        } else {
            std::stable_sort(_data.begin(), _data.end(), _less);
        }
    }

    // Can only be called after _data is sorted
    void _updateStateAfterSort() override {
        // Theory of operation: We want to be able to eagerly ignore values we know will not
        // be in the TopK result set by setting _cutoff to a value we know we have at least
        // K values equal to or better than. There are two values that we track to
        // potentially become the next value of _cutoff: _worstSeen and _lastMedian. When
        // one of these values becomes the new _cutoff, its associated counter is reset to 0
        // and a new value is chosen for that member the next time we spill.
        //
        // _worstSeen is the worst value we've seen so that all kept values are better than
        // (or equal to) it. This means that once _worstCount >= _opts.limit there is no
        // reason to consider values worse than _worstSeen so it can become the new _cutoff.
        // This technique is especially useful when the input is already roughly sorted (eg
        // sorting ASC on an ObjectId or Date field) since we will quickly find a cutoff
        // that will exclude most later values, making the full TopK operation including
        // the MergeIterator phase is O(K) in space and O(N + K*Log(K)) in time.
        //
        // _lastMedian was the median of the _data in the first spill() either overall or
        // following a promotion of _lastMedian to _cutoff. We count the number of kept
        // values that are better than or equal to _lastMedian in _medianCount and can
        // promote _lastMedian to _cutoff once _medianCount >=_opts.limit. Assuming
        // reasonable median selection (which should happen when the data is completely
        // unsorted), after the first K spilled values, we will keep roughly 50% of the
        // incoming values, 25% after the second K, 12.5% after the third K, etc. This means
        // that by the time we spill 3*K values, we will have seen (1*K + 2*K + 4*K) values,
        // so the expected number of kept values is O(Log(N/K) * K). The final run time if
        // using the O(K*Log(N)) merge algorithm in MergeIterator is O(N + K*Log(K) +
        // K*LogLog(N/K)) which is much closer to O(N) than O(N*Log(K)).
        //
        // This leaves a currently unoptimized worst case of data that is already roughly
        // sorted, but in the wrong direction, such that the desired results are all the
        // last ones seen. It will require O(N) space and O(N*Log(K)) time. Since this
        // should be trivially detectable, as a future optimization it might be nice to
        // detect this case and reverse the direction of input (if possible) which would
        // turn this into the best case described above.
        //
        // Pedantic notes: The time complexities above (which count number of comparisons)
        // ignore the sorting of batches prior to spilling to disk since they make it more
        // confusing without changing the results. If you want to add them back in, add an
        // extra term to each time complexity of (SPACE_COMPLEXITY * Log(BATCH_SIZE)). Also,
        // all space complexities measure disk space rather than memory since this class is
        // O(1) in memory due to the _opts.maxMemoryUsageBytes limit.

        // Pick a new _worstSeen or _lastMedian if should.
        if (_worstCount == 0 || _less(_worstSeen, _data.back())) {
            _worstSeen = _data.back();
        }
        if (_medianCount == 0) {
            size_t medianIndex = _data.size() / 2;  // Chooses the higher if size is even.
            _lastMedian = _data[medianIndex];
        }

        // Add the counters of kept objects better than or equal to _worstSeen/_lastMedian.
        _worstCount += _data.size();  // Everything is better or equal.
        typename std::vector<Data>::iterator firstWorseThanLastMedian =
            std::upper_bound(_data.begin(), _data.end(), _lastMedian, _less);
        _medianCount += std::distance(_data.begin(), firstWorseThanLastMedian);


        // Promote _worstSeen or _lastMedian to _cutoff and reset counters if we should.
        if (_worstCount >= _options.limit) {
            if (!_haveCutoff || _less(_worstSeen, _cutoff)) {
                _cutoff = _worstSeen;
                _haveCutoff = true;
            }
            _worstCount = 0;
        }
        if (_medianCount >= _options.limit) {
            if (!_haveCutoff || _less(_lastMedian, _cutoff)) {
                _cutoff = _lastMedian;
                _haveCutoff = true;
            }
            _medianCount = 0;
        }
    }

    // See updateCutoff() for a full description of how these members are used.
    bool _haveCutoff = false;
    Data _cutoff;             // We can definitely ignore values worse than this.
    Data _worstSeen;          // The worst Data seen so far. Reset when _worstCount >= _opts.limit.
    size_t _worstCount = 0;   // Number of docs better or equal to _worstSeen kept so far.
    Data _lastMedian;         // Median of a batch. Reset when _medianCount >= _opts.limit.
    size_t _medianCount = 0;  // Number of docs better or equal to _lastMedian kept so far.
};
}  // namespace mongo::sorter
