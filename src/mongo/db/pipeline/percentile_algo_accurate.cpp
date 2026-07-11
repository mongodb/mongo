// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/percentile_algo_accurate.h"

#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/db/sorter/file_based_spiller.h"
#include "mongo/db/sorter/sorter_template_defs.h"
#include "mongo/platform/atomic.h"

#include <algorithm>
#include <cmath>


namespace mongo {
using std::vector;


/**
 * Generates a new file name on each call using a static, atomic and monotonically increasing
 * number.
 *
 * Each user of the Sorter must implement this function to ensure that all temporary files that the
 * Sorter instances produce are uniquely identified using a unique file name extension with separate
 * atomic variable. This is necessary because the sorter.cpp code is separately included in multiple
 * places, rather than compiled in one place and linked, and so cannot provide a globally unique ID.
 */
namespace {
std::string nextFileName() {
    static Atomic<unsigned> percentileAccumulatorFileCounter;
    return "percentile-accumulator." +
        std::to_string(percentileAccumulatorFileCounter.fetchAndAdd(1));
}
}  // namespace

void AccuratePercentile::incorporate(const std::vector<double>& inputs) {
    _accumulatedValues.reserve(_accumulatedValues.size() + inputs.size());
    for (double val : inputs) {
        incorporate(val);
    }
}

void AccuratePercentile::spill() {
    uassert(9299404,
            "Accumulator is out of memory but expression context does not permit spilling to disk.",
            _expCtx->getAllowDiskUse());

    // Initialize '_spillFile' in a lazy manner only when it is needed.
    if (!_spillFile) {
        _spillStats = std::make_unique<SorterFileStats>(nullptr /* sorterTracker */);
        _spillFile = std::make_shared<sorter::File>(sorter::nextFileName(_expCtx->getTempDir()),
                                                    _spillStats.get());
    }

    if (_accumulatedValues.size() == 0) {
        return;
    }

    _numTotalValuesSpilled += _accumulatedValues.size();

    sorter::FileBasedStorage<Value, Value> sorterStorage(_spillFile,
                                                         /*dbName=*/boost::none,
                                                         sorter::kLatestChecksumVersion);
    std::unique_ptr<SortedStorageWriter<Value, Value>> writer =
        sorterStorage.makeWriter(SortOptions(), /*settings=*/{});

    std::sort(_accumulatedValues.begin(), _accumulatedValues.end());

    for (auto value : _accumulatedValues) {
        writer->addAlreadySorted(Value(value), Value(value));
    }

    // Store a pointer to the start of this run of sorted data.
    _spilledSortedSegments.emplace_back(writer->done());

    emptyMemory();
}

void AccuratePercentile::reset() {
    // Empty _accumulatedValues vector and free that memory.
    emptyMemory();

    _negInfCount = 0;
    _posInfCount = 0;
    _shouldSort = true;

    std::vector<std::shared_ptr<Sorter<Value, Value>::Iterator>> emptyVector;
    _spilledSortedSegments.swap(emptyVector);
    _sorterIterator.reset();
    _indexNextSorted = 0;
    _numTotalValuesSpilled = 0;
}

void AccuratePercentile::emptyMemory() {
    // Clear _accumulatedValues to release memory.
    std::vector<double> emptyVector;
    _accumulatedValues.swap(emptyVector);
}

/*
 * Serialize data currently in algorithm by casting to Value type.
 */
Value AccuratePercentile::serialize() {
    std::vector<Value> serializedValues(_accumulatedValues.begin(), _accumulatedValues.end());

    serializedValues.reserve(serializedValues.size() + _negInfCount + _posInfCount);

    for (int i = 0; i < _negInfCount; i++) {
        serializedValues.push_back(Value(-std::numeric_limits<double>::infinity()));
    }

    for (int i = 0; i < _posInfCount; i++) {
        serializedValues.push_back(Value(std::numeric_limits<double>::infinity()));
    }

    return Value(std::move(serializedValues));
}

void AccuratePercentile::combine(const Value& partial) {
    tassert(9299300,
            str::stream() << "'partial' is expected to be an array; found " << partial.getType(),
            partial.isArray());

    auto partialArray = partial.getArray();

    _accumulatedValues.reserve(_accumulatedValues.size() + partialArray.size());

    for (auto& value : partialArray) {
        incorporate(value.getDouble());
    }
}

vector<double> AccuratePercentile::computePercentiles(const vector<double>& ps) {
    if (!_spilledSortedSegments.empty()) {
        // We've exceeded the memory limit and spilled some data to disk, which means that there
        // might still be data in _accumulatedValues that was incorporated into the algorithm, but
        // not enough to hit the limit and spill again. So that we don't miss those values, we force
        // the algorithm to spill again here so that all of the data is on disk when we sort it.
        spill();

        auto comparator = [valueComparator = ValueComparator()](const Value& lhs,
                                                                const Value& rhs) -> int {
            return valueComparator.compare(lhs, rhs);
        };

        // Initializes _sorterIterator as a MergeIterator that merge sorts the spilled sorted
        // segments of data together so that values can be iterated through in sorted order.
        _sorterIterator =
            sorter::merge<Value, Value>(_spilledSortedSegments, SortOptions(), comparator);
        _indexNextSorted = 0;

        // Since we're reading one value from disk at a time, we compute the percentiles in
        // ascending order but we still return the results in the original order.
        vector<double> pctls(ps.size());

        vector<int> indices(ps.size());
        std::iota(indices.begin(), indices.end(), 0);

        // Sort the indices in ascending order by their corresponding p value in ps.
        std::sort(indices.begin(), indices.end(), [ps](const int a, const int b) {
            return ps[a] < ps[b];
        });

        for (int i : indices) {
            pctls[i] = *computeSpilledPercentile(ps[i]);
        }

        return pctls;
    }

    if (_accumulatedValues.empty() && _negInfCount == 0 && _posInfCount == 0) {
        return {};
    }

    vector<double> pctls;
    pctls.reserve(ps.size());

    // When sufficiently many percentiles are requested at once, it becomes more efficient to sort
    // the data rather than compute each percentile separately. The tipping point depends on both
    // the size of the data and the number of percentiles, but to keep the model simple for the knob
    // we only consider the latter.
    if (_shouldSort &&
        static_cast<int>(ps.size()) > internalQueryPercentileExprSelectToSortThreshold.load()) {
        std::sort(_accumulatedValues.begin(), _accumulatedValues.end());
        _shouldSort = false;
    }

    for (double p : ps) {
        pctls.push_back(*computePercentile(p));
    }
    return pctls;
}
}  // namespace mongo
