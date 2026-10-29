// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status.h"

#include "mongo/base/error_codes.h"

#include <atomic>
#include <cstdint>

#include <benchmark/benchmark.h>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

/**
 * Construct and destroy OK
 */
void BM_StatusCtorDtorOK(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(Status::OK());
    }
}

BENCHMARK(BM_StatusCtorDtorOK);

/**
 * Construct and destroy
 */
void BM_StatusCtorDtor(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(
            Status(ErrorCodes::Error::InternalError, "A reasonably long reason"));
    }
}

BENCHMARK(BM_StatusCtorDtor);


/**
 * Copying an uncontended Status object once.
 */
void BM_StatusRefUnref(benchmark::State& state) {
    static Status s(ErrorCodes::Error::InternalError, "A reasonably long reason");
    for (auto _ : state) {
        benchmark::DoNotOptimize(Status(s));
    }
}

BENCHMARK(BM_StatusRefUnref)->ThreadRange(1, 4);

template <typename D>
struct TestRefCountable {
    friend void intrusive_ptr_add_ref(const TestRefCountable* ptr) {
        ptr->_count.fetch_add(1, std::memory_order_relaxed);
    };

    friend void intrusive_ptr_release(const TestRefCountable* ptr) {
        if (ptr->_count.fetch_sub(1, std::memory_order_acq_rel) == 1)
            delete static_cast<const D*>(ptr);
    };

    mutable std::atomic<uint32_t> _count;  // NOLINT
};

struct Nonpolymorphic : TestRefCountable<Nonpolymorphic> {};

struct Polymorphic : TestRefCountable<Polymorphic> {
    virtual ~Polymorphic() = default;
};

void BM_NonpolymorphicRefUnref(benchmark::State& state) {
    static boost::intrusive_ptr<Nonpolymorphic> s{new Nonpolymorphic{}};
    for (auto _ : state) {
        benchmark::DoNotOptimize(boost::intrusive_ptr<Nonpolymorphic>{s});
    }
}

BENCHMARK(BM_NonpolymorphicRefUnref)->ThreadRange(1, 4);

/**
 * Curious: Does a virtual dtor mean a slower ref/unref cycle?
 * The count is then at offset 8 rather than 0, so maybe!
 */
void BM_PolymorphicRefUnref(benchmark::State& state) {
    static boost::intrusive_ptr<Polymorphic> s{new Polymorphic{}};
    for (auto _ : state) {
        benchmark::DoNotOptimize(boost::intrusive_ptr<Polymorphic>{s});
    }
}

BENCHMARK(BM_PolymorphicRefUnref)->ThreadRange(1, 4);


}  // namespace
}  // namespace mongo
