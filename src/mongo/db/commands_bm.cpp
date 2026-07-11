// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/idl/command_generic_argument.h"

#include <string>
#include <vector>

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

static std::vector<std::string> keys{
    "a",  // short not found
    "CrazyFieldDeliberatelyNotFound",
    "$db",
    "$audit",
    "lsid",
    "writeConcern",
    "",
};

void BM_IsGeneric(benchmark::State& state) {
    const auto& key = keys[state.range()];
    state.SetLabel(key.c_str());
    for (auto _ : state) {
        benchmark::DoNotOptimize(mongo::isGenericArgument(key));
    }
}
void BM_IsRequestStripArgument(benchmark::State& state) {
    const auto& key = keys[state.range()];
    state.SetLabel(key.c_str());
    for (auto _ : state) {
        benchmark::DoNotOptimize(mongo::shouldForwardToShards(key));
    }
}

void BM_IsReplyStripArgument(benchmark::State& state) {
    const auto& key = keys[state.range()];
    state.SetLabel(key.c_str());
    for (auto _ : state) {
        benchmark::DoNotOptimize(mongo::shouldForwardFromShards(key));
    }
}

BENCHMARK(BM_IsGeneric)->DenseRange(0, keys.size() - 1);
BENCHMARK(BM_IsRequestStripArgument)->DenseRange(0, keys.size() - 1);
BENCHMARK(BM_IsReplyStripArgument)->DenseRange(0, keys.size() - 1);

}  // namespace
}  // namespace mongo
