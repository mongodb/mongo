// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/plan_insert_listener.h"

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source.h"

#include <memory>

namespace mongo {
namespace {

// LocalCappedInsertNotifier version-handshake tests.
//
// These tests pin down the exact state-machine behavior that makes it safe to *defer* the
// creation of the capped insert notifier in PlanExecutorImpl. The classic plan executor no longer
// builds the notifier eagerly on every _getNextImpl()/getNextBatch() call; it builds it lazily on
// the first EOF that actually needs to wait, and then reuses the same object across subsequent EOFs
// within the same call.
//
// The correctness of that deferral rests on two properties of LocalCappedInsertNotifier:
//   1. A freshly created notifier's first wait never blocks (because _lastEOFVersion starts at
//      ~0, which can never equal a real version). This is why creating the notifier late -- only
//      after a productive scan reaches EOF -- cannot cause a lost wakeup: the first EOF re-scans
//      instead of blocking.
//   2. Blocking only happens on a *second* consecutive EOF, and only if the same object is
//      reused so that _lastEOFVersion carries the version snapshot from the first EOF. Recreating
//      the notifier on each EOF would reset _lastEOFVersion to ~0 and spin forever.
//

class LocalCappedInsertNotifierTest : public ServiceContextTest {};

// A brand-new notifier must return immediately from its first wait, even against a version that
// has never advanced and a deadline arbitrarily far in the future. If this blocked, the test
// would hang.
TEST_F(LocalCappedInsertNotifierTest, FirstWaitDoesNotBlockOnFreshNotifier) {
    auto opCtx = makeOperationContext();
    auto capped = std::make_shared<CappedInsertNotifier>();
    insert_listener::LocalCappedInsertNotifier notifier(capped);

    notifier.prepareForWait(opCtx.get());

    // Deadline is Date_t::max(): the only way this returns is via the version mismatch, not a
    // timeout.
    notifier.waitUntil(opCtx.get(), Date_t::max());
    notifier.doneWaiting(opCtx.get());

    // Reaching here (without hanging) proves the first wait did not block.
    SUCCEED();
}

// After the first EOF cycle records the current version, a second wait with no intervening
// insert must block until the deadline. This is what makes reusing the same object matter: the
// version snapshot from the first cycle is what enables real blocking on the second.
TEST_F(LocalCappedInsertNotifierTest, SecondWaitBlocksUntilDeadlineWithoutInsert) {
    auto opCtx = makeOperationContext();
    auto capped = std::make_shared<CappedInsertNotifier>();
    insert_listener::LocalCappedInsertNotifier notifier(capped);

    // First EOF cycle: returns immediately, records version.
    notifier.prepareForWait(opCtx.get());
    notifier.waitUntil(opCtx.get(), Date_t::max());
    notifier.doneWaiting(opCtx.get());

    // Second EOF cycle with no insert: must wait out the deadline rather than returning at once.
    auto* clock = opCtx->getServiceContext()->getPreciseClockSource();
    const auto deadline = clock->now() + Milliseconds(75);
    notifier.prepareForWait(opCtx.get());
    const auto before = clock->now();
    notifier.waitUntil(opCtx.get(), deadline);
    const auto elapsed = clock->now() - before;

    ASSERT_GTE(elapsed, Milliseconds(50))
        << "Second wait returned early (" << elapsed << "); version handshake did not block.";
}

// An insert (notifyAll) that lands between prepareForWait and waitUntil on the second cycle must
// wake the waiter immediately. This mirrors the real blocking window that deferred creation is
// required to preserve.
TEST_F(LocalCappedInsertNotifierTest, InsertBetweenPrepareAndWaitWakesSecondWait) {
    auto opCtx = makeOperationContext();
    auto capped = std::make_shared<CappedInsertNotifier>();
    insert_listener::LocalCappedInsertNotifier notifier(capped);

    // First EOF cycle.
    notifier.prepareForWait(opCtx.get());
    notifier.waitUntil(opCtx.get(), Date_t::max());
    notifier.doneWaiting(opCtx.get());

    // Second cycle: an insert bumps the version after we snapshot it, so the wait returns at once
    // despite an infinite deadline.
    notifier.prepareForWait(opCtx.get());
    capped->notifyAll();
    notifier.waitUntil(opCtx.get(), Date_t::max());
    notifier.doneWaiting(opCtx.get());

    // Reaching here (without hanging) proves that none of the waits blocked.
    SUCCEED();
}

// Documents why the notifier must be reused rather than recreated per EOF: a fresh object always
// short-circuits its first wait, so recreating it on every EOF would spin without ever blocking.
// Here two independent notifiers over the same underlying CappedInsertNotifier each return
// immediately from their first wait even though the version never advances.
TEST_F(LocalCappedInsertNotifierTest, FreshNotifierAlwaysShortCircuitsFirstWait) {
    auto opCtx = makeOperationContext();
    auto capped = std::make_shared<CappedInsertNotifier>();

    for (int i = 0; i < 3; ++i) {
        insert_listener::LocalCappedInsertNotifier notifier(capped);
        notifier.prepareForWait(opCtx.get());
        notifier.waitUntil(opCtx.get(), Date_t::max());
        notifier.doneWaiting(opCtx.get());
    }

    // No hang across repeated fresh instances confirms the "first wait never blocks" property.
    SUCCEED();
}

// A killed notifier must wake any wait immediately, regardless of version, so a dropped or
// invalidated collection cannot leave a tailing cursor blocked forever.
TEST_F(LocalCappedInsertNotifierTest, KillWakesBlockingWait) {
    auto opCtx = makeOperationContext();
    auto capped = std::make_shared<CappedInsertNotifier>();
    insert_listener::LocalCappedInsertNotifier notifier(capped);

    // Advance past the first non-blocking cycle.
    notifier.prepareForWait(opCtx.get());
    notifier.waitUntil(opCtx.get(), Date_t::max());
    notifier.doneWaiting(opCtx.get());

    // Kill before the second wait: the wait must return without honoring the infinite deadline.
    notifier.prepareForWait(opCtx.get());
    capped->kill();
    notifier.waitUntil(opCtx.get(), Date_t::max());

    // Reaching here (without hanging) proves that the wait did not block.
    SUCCEED();
}

}  // namespace
}  // namespace mongo
