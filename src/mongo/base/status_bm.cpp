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
