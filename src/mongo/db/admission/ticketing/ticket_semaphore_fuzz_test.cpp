// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/admission/ticketing/ordered_ticket_semaphore.h"
#include "mongo/db/admission/ticketing/unordered_ticket_semaphore.h"
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
