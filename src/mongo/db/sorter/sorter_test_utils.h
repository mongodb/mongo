/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/db/sorter/sorter_template_defs.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

#include <climits>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::sorter {
//
// Sorter framework testing utilities
//
class IntWrapper {
public:
    IntWrapper(int i = 0) : _i(i) {}
    operator const int&() const {
        return _i;
    }

    /// members for Sorter
    struct SorterDeserializeSettings {};  // unused
    void serializeForSorter(BufBuilder& buf) const {
        buf.appendNum(_i);
    }
    static IntWrapper deserializeForSorter(BufReader& buf, const SorterDeserializeSettings&) {
        return buf.read<LittleEndian<int>>().value;
    }
    int memUsageForSorter() const {
        return sizeof(IntWrapper);
    }
    IntWrapper getOwned() const {
        return *this;
    }
    void makeOwned() {}

    std::string toString() const {
        return std::to_string(_i);
    }

private:
    int _i;
};

typedef std::pair<IntWrapper, IntWrapper> IWPair;
typedef SortIteratorInterface<IntWrapper, IntWrapper> IWIterator;
typedef Sorter<IntWrapper, IntWrapper> IWSorter;

enum Direction { ASC = 1, DESC = -1 };
class IWComparator {
public:
    IWComparator(Direction dir = ASC) : _dir(dir) {}
    int operator()(const IntWrapper& lhs, const IntWrapper& rhs) const {
        if (lhs == rhs)
            return 0;
        if (lhs < rhs)
            return -1 * _dir;
        return 1 * _dir;
    }

private:
    Direction _dir;
};

class IntIterator : public IWIterator {
public:
    IntIterator(int start = 0, int stop = INT_MAX, int increment = 1)
        : _current(start), _increment(increment), _stop(stop) {}
    bool more() override {
        if (_increment == 0)
            return true;
        if (_increment > 0)
            return _current < _stop;
        return _current > _stop;
    }
    IWPair next() override {
        IWPair out(_current, -_current);
        _current += _increment;
        return out;
    }
    IntWrapper nextWithDeferredValue() override {
        MONGO_UNREACHABLE;
    }
    IntWrapper getDeferredValue() override {
        MONGO_UNREACHABLE;
    }
    const IntWrapper& current() override {
        MONGO_UNREACHABLE;
    }

private:
    int _current;
    int _increment;
    int _stop;
};

class EmptyIterator : public IWIterator {
public:
    bool more() override {
        return false;
    }
    Data next() override {
        MONGO_UNREACHABLE;
    }
    IntWrapper nextWithDeferredValue() override {
        MONGO_UNREACHABLE;
    }
    IntWrapper getDeferredValue() override {
        MONGO_UNREACHABLE;
    }
    const IntWrapper& current() override {
        MONGO_UNREACHABLE;
    }
};

class LimitIterator : public IWIterator {
public:
    LimitIterator(long long limit, std::shared_ptr<IWIterator> source)
        : _remaining(limit), _source(source) {
        invariant(limit > 0);
    }

    bool more() override {
        return _remaining && _source->more();
    }
    Data next() override {
        invariant(more());
        _remaining--;
        return _source->next();
    }
    IntWrapper nextWithDeferredValue() override {
        MONGO_UNREACHABLE;
    }
    IntWrapper getDeferredValue() override {
        MONGO_UNREACHABLE;
    }
    const IntWrapper& current() override {
        MONGO_UNREACHABLE;
    }

private:
    long long _remaining;
    std::shared_ptr<IWIterator> _source;
};

template <typename It1, typename It2>
void _assertIteratorsEquivalent(It1&& it1, It2&& it2, int line) {
    int iteration;
    try {
        for (iteration = 0; true; iteration++) {
            ASSERT_EQUALS(it1->more(), it2->more()) << "on iteration " << iteration;
            // make sure more() is safe to call twice
            ASSERT_EQUALS(it1->more(), it2->more()) << "on iteration " << iteration;
            if (!it1->more())
                return;

            sorter::IWPair pair1 = it1->next();
            sorter::IWPair pair2 = it2->next();
            ASSERT_EQUALS(pair1.first, pair2.first) << "on iteration " << iteration;
            ASSERT_EQUALS(pair1.second, pair2.second) << "on iteration " << iteration;
        }
    } catch (...) {
        LOGV2(22047,
              "Failure from line {line} on iteration {iteration}",
              "line"_attr = line,
              "iteration"_attr = iteration);
        throw;
    }
}

#define ASSERT_ITERATORS_EQUIVALENT(it1, it2) _assertIteratorsEquivalent(it1, it2, __LINE__)

template <typename It1, typename It2>
void _assertIteratorsEquivalentForNSteps(It1& it1, It2& it2, int maxSteps, int line) {
    int iteration;
    try {
        for (iteration = 0; iteration < maxSteps; iteration++) {
            ASSERT_EQUALS(it1->more(), it2->more()) << "on iteration " << iteration;
            // make sure more() is safe to call twice
            ASSERT_EQUALS(it1->more(), it2->more()) << "on iteration " << iteration;
            if (!it1->more())
                return;

            IWPair pair1 = it1->next();
            IWPair pair2 = it2->next();
            ASSERT_EQUALS(pair1.first, pair2.first) << "on iteration " << iteration;
            ASSERT_EQUALS(pair1.second, pair2.second) << "on iteration " << iteration;
        }
    } catch (...) {
        LOGV2(6409300,
              "Failure from line {line} on iteration {iteration}",
              "line"_attr = line,
              "iteration"_attr = iteration);
        throw;
    }
}

#define ASSERT_ITERATORS_EQUIVALENT_FOR_N_STEPS(it1, it2, n) \
    _assertIteratorsEquivalentForNSteps(it1, it2, n, __LINE__)

template <int N>
std::shared_ptr<IWIterator> makeInMemIterator(const int (&array)[N]);

template <int N>
std::shared_ptr<IWIterator> makeInMemIterator(const int (&array)[N]) {
    std::vector<IWPair> vec;
    for (int i = 0; i < N; i++)
        vec.push_back(IWPair(array[i], -array[i]));
    return std::make_shared<InMemIterator<IntWrapper, IntWrapper>>(vec);
}

/**
 * Spills the contents of inputIter to a file and returns a FileIterator for reading the data
 * back. This is needed because the MergeIterator currently requires that it is merging from
 * sorted spill file segments (as opposed to any other kind of iterator).
 */
template <typename IteratorPtr>
std::shared_ptr<IWIterator> spillToFile(IteratorPtr inputIter, const unittest::TempDir& tempDir) {
    if (!inputIter->more()) {
        return std::make_shared<EmptyIterator>();
    }
    SorterFileStats sorterFileStats(nullptr /* sorterTracker */);
    const SortOptions opts = SortOptions().TempDir(tempDir.path());
    auto spillFile = std::make_shared<Sorter<IntWrapper, IntWrapper>::File>(
        sorter::nextFileName(*(opts.tempDir)), opts.sorterFileStats);
    SortedFileWriter<IntWrapper, IntWrapper> writer(opts, spillFile);
    while (inputIter->more()) {
        auto pair = inputIter->next();
        writer.addAlreadySorted(pair.first, pair.second);
    }
    return writer.done();
}

template <typename IteratorPtr, int N>
std::shared_ptr<IWIterator> mergeIterators(IteratorPtr (&array)[N],
                                           const unittest::TempDir& tempDir,
                                           Direction Dir = ASC,
                                           const SortOptions& opts = SortOptions()) {
    invariant(!opts.tempDir);
    std::vector<std::shared_ptr<IWIterator>> vec;
    for (auto& it : array) {
        // Spill iterator outputs to a file and obtain a new iterator for it.
        vec.push_back(spillToFile(std::move(it), tempDir));
    }
    return IWIterator::merge(vec, opts, IWComparator(Dir));
}
}  // namespace mongo::sorter

#undef MONGO_LOGV2_DEFAULT_COMPONENT
