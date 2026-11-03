// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/stats/space_saving.h"

#include "mongo/db/query/compiler/stats/rand_utils.h"
#include "mongo/db/query/compiler/stats/value_utils.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <vector>

#include <benchmark/benchmark.h>
#include <fmt/format.h>

namespace mongo::stats {

namespace {

// Fixed seed so that every configuration runs on the same stream.
constexpr size_t kSeed = 1354754;

// Number of distinct values in the stream, as a fraction of the stream size.
constexpr size_t kNdvPerStreamElem = 100;

/**
 * Returns the output-facing name of a distribution; zipfian configurations are identified by
 * their exponent, encoded as ten times the alpha. Shared by benchmark registration and the
 * title lines printed while benchmarks run.
 */
std::string distributionName(DistrType distr, int64_t zipfAlphaX10) {
    return distr == DistrType::kZipfian ? fmt::format("zipfAlpha:{}", zipfAlphaX10) : "uniform";
}

int64_t asInt64(const SBEValue& value) {
    tassert(
        7586500, "Expected an Int64 value", value.getTag() == sbe::value::TypeTags::NumberInt64);
    return sbe::value::bitcastTo<int64_t>(value.getValue());
}

/**
 * Generates a stream of 'size' Int64 values drawn from 'ndv' distinct values, where the value
 * ranks are distributed according to 'distr'. For the zipfian distribution, 'zipfAlpha' is the
 * exponent of the rank frequency law (P(rank) proportional to rank^-zipfAlpha), delegated to
 * absl::zipf_distribution via ZipfianDistr. Returns the stream of SBE values together with the
 * raw values for computing the true top-k.
 */
std::pair<std::vector<SBEValue>, std::vector<int64_t>> genStream(
    size_t size, size_t ndv, DistrType distr, double zipfAlpha, size_t seed) {
    std::mt19937_64 gen{seed};
    std::unique_ptr<RandomDistribution> rankDistr;
    switch (distr) {
        case DistrType::kUniform:
            rankDistr = std::make_unique<UniformDistr>(0, ndv - 1);
            break;
        case DistrType::kZipfian:
            rankDistr = std::make_unique<ZipfianDistr>(0, ndv - 1, zipfAlpha);
            break;
        default:
            MONGO_UNREACHABLE;
    }

    std::vector<int64_t> rawStream;
    rawStream.reserve(size);
    std::generate_n(std::back_inserter(rawStream), size, [&] { return (*rankDistr)(gen); });

    std::vector<SBEValue> stream;
    stream.reserve(size);
    for (int64_t value : rawStream) {
        stream.emplace_back(SBEValue{makeInt64Value(value)});
    }
    return {std::move(stream), std::move(rawStream)};
}

/**
 * Computes the exact top-k most frequent values of the stream (ties broken by value order
 * ascending). Not part of the timed region.
 */
stdx::unordered_set<int64_t> computeTrueTopK(const std::vector<int64_t>& rawStream, size_t k) {
    stdx::unordered_map<int64_t, size_t> counts;
    for (int64_t value : rawStream) {
        ++counts[value];
    }

    std::vector<std::pair<int64_t, size_t>> freqs;
    freqs.reserve(counts.size());
    for (const auto& [value, count] : counts) {
        freqs.emplace_back(value, count);
    }
    std::sort(freqs.begin(), freqs.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) {
            return a.second > b.second;
        }
        return a.first < b.first;
    });

    stdx::unordered_set<int64_t> trueTopK;
    for (size_t i = 0; i < k && i < freqs.size(); ++i) {
        trueTopK.insert(freqs[i].first);
    }
    return trueTopK;
}

/**
 * Benchmarks MostCommonValues::make() over a synthetic stream: sketch construction, the ingest
 * of all stream elements, and getTopK(). The true top-k computation and the accuracy metrics are
 * excluded from the timing. Precision — the fraction of returned entries in the true
 * top-k — is reported in the label.
 */
void BM_SpaceSaving(benchmark::State& state) {
    const size_t size = static_cast<size_t>(state.range(0));
    const size_t k = static_cast<size_t>(state.range(1));
    const auto distr = static_cast<DistrType>(state.range(2));
    // Zipfian exponent encoded as ten times the alpha to fit an integer benchmark argument.
    const double zipfAlpha = static_cast<double>(state.range(3)) / 10.0;
    const size_t ndv = static_cast<size_t>(state.range(4));

    // Benchmarks run in registration order, which is grouped by distribution, so this prints one
    // title line at the start of each distribution group in the console output.
    static std::string lastDistrName;
    const std::string distrName = distributionName(distr, state.range(3));
    if (distrName != lastDistrName) {
        fmt::print("\n=== Distribution: {} ===\n", distrName);
        lastDistrName = distrName;
    }

    auto [stream, rawStream] = genStream(size, ndv, distr, zipfAlpha, kSeed);

    // Exact answer, not timed.
    const auto trueTopK = computeTrueTopK(rawStream, k);

    // The result is deterministic, so the last timed iteration's result is used for accuracy.
    MostCommonValues mcv;
    for (auto _ : state) {
        mcv = MostCommonValues::make(stream, k);
        benchmark::DoNotOptimize(mcv);
    }
    state.SetItemsProcessed(state.iterations() * size);

    // Accuracy of the returned top-k against the true top-k.
    size_t overlap = 0;
    size_t maxErr = 0;
    for (const auto& entry : mcv._entries) {
        if (trueTopK.count(asInt64(entry.value)) > 0) {
            ++overlap;
        }
        maxErr = std::max(maxErr, entry.err);
    }
    const double precision = static_cast<double>(overlap) / mcv._entries.size();
    const double cumulativeLowBoundFreqPct =
        100.0 * mcv.getCumulativeLowBoundFrequency() / mcv._inputSize;
    state.SetLabel(
        fmt::format("precision: {:.3f}, cumLowBoundFreq%: {:.1f}, "
                    "guaranteedTopK: {}, maxErr: {}",
                    precision,
                    cumulativeLowBoundFreqPct,
                    mcv.getGuaranteedTopK(),
                    maxErr));
}

// Registers one benchmark per configuration, with the distribution as the outermost dimension so
// all zipfian configurations are grouped together. The NDV candidates for a (size, k) pair are
// 2 * k, and size / kNdvPerStreamElem when strictly larger than 2 * k; the number of distinct
// values cannot be expressed as a fixed cross-product axis because it depends on k, hence the
// explicit registration.
void registerSpaceSavingBenchmarks() {
    const std::vector<std::pair<int64_t /*DistrType*/, int64_t /*zipfAlpha, times 10*/>>
        distributionConfigs = {
            {static_cast<int64_t>(DistrType::kUniform), /*zipfAlpha (unused)*/ 20},
            {static_cast<int64_t>(DistrType::kZipfian), 15},
            {static_cast<int64_t>(DistrType::kZipfian), 20}};
    for (const auto& [distr, zipfAlpha] : distributionConfigs) {
        const std::string distrName = distributionName(static_cast<DistrType>(distr), zipfAlpha);
        for (int64_t size : {10'000, 100'000}) {
            for (int64_t k : {10, 32, 100}) {
                std::vector<int64_t> ndvCandidates = {2 * k};
                if (size / static_cast<int64_t>(kNdvPerStreamElem) > 2 * k) {
                    ndvCandidates.push_back(size / static_cast<int64_t>(kNdvPerStreamElem));
                }
                for (int64_t ndv : ndvCandidates) {
                    benchmark::RegisterBenchmark(
                        fmt::format("BM_SpaceSaving/distribution:{}/size:{}/numCounters:{}/ndv:{}",
                                    distrName,
                                    size,
                                    k,
                                    ndv)
                            .c_str(),
                        BM_SpaceSaving)
                        ->Args({size, k, distr, zipfAlpha, ndv});
                }
            }
        }
    }

    // Uniform distribution with ndv equal to the collection size: the stream is nearly
    // all-distinct, so the sketch operates in its eviction-heavy regime. Registered only
    // for size 10000.
    for (int64_t k : {10, 32, 100}) {
        benchmark::RegisterBenchmark(
            fmt::format("BM_SpaceSaving/distribution:uniform/size:10000/numCounters:{}/ndv:10000",
                        k)
                .c_str(),
            BM_SpaceSaving)
            ->Args({10000, k, static_cast<int64_t>(DistrType::kUniform), 20, 10000});
    }
}

const bool kSpaceSavingBenchmarksRegistered = [] {
    registerSpaceSavingBenchmarks();
    return true;
}();

}  // namespace

}  // namespace mongo::stats
