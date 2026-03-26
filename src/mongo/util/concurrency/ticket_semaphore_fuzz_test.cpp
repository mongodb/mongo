/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/util/concurrency/ordered_ticket_semaphore.h"
#include "mongo/util/concurrency/unordered_ticket_semaphore.h"
#include "mongo/util/duration.h"

#include <algorithm>
#include <cstdint>
#include <tuple>
#include <vector>

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

namespace mongo {
namespace {

enum class Op : uint8_t { TryAcquire = 0, Release = 1, Resize = 2 };

/**
 * Verifies the permit-accounting invariant after every step of an arbitrary op sequence:
 *
 *   available() + held == initialPermits + cumulative_resize_delta
 *
 * The concurrent overbooking and no-leak invariants are fully covered by
 * ConcurrentAcquireDoesNotOverbookOrLeak in ticket_semaphore_test.cpp and do not need a
 * separate fuzz target.
 */
template <typename SemType>
void RunStateMachine(int initialPermits, std::vector<std::tuple<uint8_t, int8_t>> ops) {
    SemType sem(initialPermits, /*maxWaiters=*/1024);

    int held = 0, cumResize = 0;

    for (auto& [rawOp, rawVal] : ops) {
        switch (static_cast<Op>(rawOp % 3)) {
            case Op::TryAcquire:
                if (sem.tryAcquire())
                    ++held;
                break;
            case Op::Release:
                if (held > 0) {
                    sem.release();
                    --held;
                }
                break;
            case Op::Resize: {
                // Clamp to avoid overflow of the int permit counter.
                const int delta = std::clamp(static_cast<int>(rawVal), -8, 8);
                sem.resize(delta);
                cumResize += delta;
                break;
            }
        }
        ASSERT_EQ(sem.available() + held, initialPermits + cumResize);
        ASSERT_GE(held, 0);
    }

    for (int i = 0; i < held; ++i)
        sem.release();
}

void FuzzUnorderedStateMachine(int initialPermits, std::vector<std::tuple<uint8_t, int8_t>> ops) {
    RunStateMachine<UnorderedTicketSemaphore>(initialPermits, std::move(ops));
}

void FuzzOrderedStateMachine(int initialPermits, std::vector<std::tuple<uint8_t, int8_t>> ops) {
    RunStateMachine<OrderedTicketSemaphore>(initialPermits, std::move(ops));
}

FUZZ_TEST(TicketSemaphoreStateMachineFuzz, FuzzUnorderedStateMachine)
    .WithDomains(fuzztest::InRange(0, 32),
                 fuzztest::VectorOf(fuzztest::TupleOf(fuzztest::Arbitrary<uint8_t>(),
                                                      fuzztest::Arbitrary<int8_t>()))
                     .WithMaxSize(200));

FUZZ_TEST(TicketSemaphoreStateMachineFuzz, FuzzOrderedStateMachine)
    .WithDomains(fuzztest::InRange(0, 32),
                 fuzztest::VectorOf(fuzztest::TupleOf(fuzztest::Arbitrary<uint8_t>(),
                                                      fuzztest::Arbitrary<int8_t>()))
                     .WithMaxSize(200));

}  // namespace
}  // namespace mongo
