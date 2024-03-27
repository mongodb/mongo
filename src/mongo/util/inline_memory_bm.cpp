/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/util/inline_memory.h"

#include <absl/container/inlined_vector.h>
#include <benchmark/benchmark.h>
#include <boost/container/small_vector.hpp>
#include <list>
#include <vector>
#include <version>

#if __cpp_lib_memory_resource >= 201603L
#include <memory_resource>
#define MONGO_HAS_STD_MEMORY_RESOURCE
#endif

namespace mongo::inline_memory {
namespace {

#ifdef MONGO_HAS_STD_MEMORY_RESOURCE
template <typename T, size_t inlineElementCount>
class PmrMemoryBase {
public:
    std::pmr::polymorphic_allocator<void> makeAllocator() {
        return std::pmr::polymorphic_allocator<void>{&_monotonic};
    }

private:
    static const size_t sz = sizeof(detail::FakeListNode<T>);
    static const size_t al = alignof(detail::FakeListNode<T>);

    alignas(al) std::array<std::byte, sz> _buf;
    std::pmr::monotonic_buffer_resource _monotonic{_buf.data(), _buf.size()};
};

template <typename T,
          size_t inlineElementCount,
          typename MemoryBase = PmrMemoryBase<T, inlineElementCount>,
          typename ListBase = std::pmr::list<T>>
class PmrInlineList : MemoryBase, public ListBase {
public:
    PmrInlineList() : ListBase{this->makeAllocator()} {}
};

template <typename T,
          size_t inlineElementCount,
          typename MemoryBase = PmrMemoryBase<T, inlineElementCount>,
          typename VectorBase = std::pmr::vector<T>>
class PmrInlineVector : MemoryBase, public VectorBase {
public:
    PmrInlineVector() : VectorBase{this->makeAllocator()} {}
};
#endif  // MONGO_HAS_STD_MEMORY_RESOURCE

template <typename T>
void BM_Fill(benchmark::State& state) {
    for (auto _ : state) {
        T sequence;
        for (auto i = state.range(0); i--;) {
            sequence.emplace_back();
        }
        benchmark::DoNotOptimize(sequence);
    }
}

template <typename T>
void BM_Walk(benchmark::State& state) {
    T sequence;
    for (auto i = state.range(0); i--;)
        sequence.emplace_back();
    for (auto _ : state) {
        benchmark::DoNotOptimize(&sequence);
        for (auto&& e : sequence) {
            benchmark::DoNotOptimize(&e);
        }
    }
}

using IntArray100 = std::array<int, 100>;

template <typename T>
using InlineList1 = List<T, 1>;
template <typename T>
using InlineList100 = List<T, 100>;
template <typename T>
using InlineList1000 = List<T, 1000>;

#ifdef MONGO_HAS_STD_MEMORY_RESOURCE
template <typename T>
using PmrInlineList1 = PmrInlineList<T, 1>;
template <typename T>
using PmrInlineList100 = PmrInlineList<T, 100>;
template <typename T>
using PmrInlineList1000 = PmrInlineList<T, 1000>;

template <typename T>
using PmrInlineVector1 = PmrInlineVector<T, 1>;
template <typename T>
using PmrInlineVector100 = PmrInlineVector<T, 100>;
template <typename T>
using PmrInlineVector1000 = PmrInlineVector<T, 1000>;

#define FOREACH_PMR_LIST_TEMPLATE(X, T) \
    X(PmrInlineList1<T>)                \
    X(PmrInlineList100<T>)              \
    X(PmrInlineList1000<T>)             \
    /**/
#define FOREACH_PMR_VECTOR_TEMPLATE(X, T) \
    X(PmrInlineVector1<T>)                \
    X(PmrInlineVector100<T>)              \
    X(PmrInlineVector1000<T>)             \
    /**/

#else  // MONGO_HAS_STD_MEMORY_RESOURCE
#define FOREACH_PMR_LIST_TEMPLATE(X, T)
#define FOREACH_PMR_VECTOR_TEMPLATE(X, T)
#endif  // MONGO_HAS_STD_MEMORY_RESOURCE

template <typename T>
using InlineVector1 = Vector<T, 1>;
template <typename T>
using InlineVector100 = Vector<T, 100>;
template <typename T>
using InlineVector1000 = Vector<T, 1000>;

template <typename T>
using AbslInlinedVector1 = absl::InlinedVector<T, 1>;
template <typename T>
using AbslInlinedVector100 = absl::InlinedVector<T, 100>;
template <typename T>
using AbslInlinedVector1000 = absl::InlinedVector<T, 1000>;

template <typename T>
using BoostSmallVector1 = boost::container::small_vector<T, 1>;
template <typename T>
using BoostSmallVector100 = boost::container::small_vector<T, 100>;
template <typename T>
using BoostSmallVector1000 = boost::container::small_vector<T, 1000>;

#define FOREACH_TEMPLATE(X, T)        \
    X(std::list<T>)                   \
    X(InlineList1<T>)                 \
    X(InlineList100<T>)               \
    X(InlineList1000<T>)              \
    FOREACH_PMR_LIST_TEMPLATE(X, T)   \
    X(std::vector<T>)                 \
    X(InlineVector1<T>)               \
    X(InlineVector100<T>)             \
    X(InlineVector1000<T>)            \
    X(AbslInlinedVector1<T>)          \
    X(AbslInlinedVector100<T>)        \
    X(AbslInlinedVector1000<T>)       \
    X(BoostSmallVector1<T>)           \
    X(BoostSmallVector100<T>)         \
    X(BoostSmallVector1000<T>)        \
    FOREACH_PMR_VECTOR_TEMPLATE(X, T) \
    /**/

auto applyArgs = [](benchmark::internal::Benchmark* bm) {
    bm->Args({0})->Args({1})->Args({1000});
};

#define FOREACH_BM(T)                                 \
    BENCHMARK_TEMPLATE(BM_Fill, T)->Apply(applyArgs); \
    BENCHMARK_TEMPLATE(BM_Walk, T)->Apply(applyArgs); \
    /**/

#define GENERATE_BENCHMARKS()                 \
    FOREACH_TEMPLATE(FOREACH_BM, int)         \
    FOREACH_TEMPLATE(FOREACH_BM, IntArray100) \
    /**/

GENERATE_BENCHMARKS()

}  // namespace
}  // namespace mongo::inline_memory
