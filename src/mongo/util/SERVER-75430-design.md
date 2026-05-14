# SERVER-75430: Interruptible Atomic Wait on `OperationContext`

## Symptom

Several hot paths inside `TicketPool` / `PriorityTicketHolder` queue waiters on a
raw atomic flag via a futex. `OperationContext`'s interrupt mechanism is itself
just an atomic (`_killCode`), and the two atomics are not coupled, so a thread
sleeping on the ticket atomic does not observe `killOp`, `shutdown`, or
`maxTimeMS` until something else wakes it. The current workaround is a 500 ms
periodic re-queue (`priority_ticketholder.cpp:96-99`): the waiter wakes itself,
calls `checkForInterrupt()`, and re-enters the wait if no interrupt fired.

This is fine under steady-state. Under heavy contention -- many client threads,
sustained queue depths beyond pool capacity -- every operation queues for longer
than 500 ms, so every operation pays the wake/check/re-queue tax. The wake
storm consumes CPU that the pool was meant to throttle, queue depths grow, and
the system enters a metastable failure mode in which:

- `killOp` against a queued op can stall up to 500 ms before unblocking.
- `shutdown` walks every queued op via `markKilled`; with a long queue this is
  multiplied by 500 ms per waiter that happens to be mid-sleep on the timer.
- Wake-up storms thrash the scheduler and worsen the very contention that
  caused the queueing in the first place.

## Fix sketch

Add `OperationContext::waitForAtomicOrInterrupt(WaitableAtomic& atomic,
Predicate pred)` that couples both wake sources into a single sleep:

1. Fast path: evaluate `pred()`; if true, return `Status::OK()` without
   sleeping.
2. Register interest in the kill bit. On Linux this is a `futex(FUTEX_WAIT)`
   on a vector of two 32-bit words ([`futex(2)` waitv variant][1]) -- one is
   the caller-supplied atomic, the other is `_killCode` re-read as a `uint32_t`
   (all `ErrorCodes::Error` values fit). On other platforms, fall back to a
   `condition_variable` keyed off both atomics; the predicate must close over
   both.
3. Slow path: sleep until the kernel wakes the thread because *either* word
   changed. Wake source attribution is `pred()` vs `isKillPending()`.
4. On wake, if `isKillPending()` is true, return the kill status; otherwise
   return `Status::OK()`.

Critically, callers route `OperationContext::sleepFor` / `sleepUntil` (not the
raw `condition_variable::wait_for`) so the substrate's existing deadline
machinery, baton interruption, and `_killCode` stamping all keep working. The
500 ms re-queue is removed.

## Liveness guarantee

`InterruptIsResponsive` (the TLA+ liveness property pinned in
[`WaitForAtomicOrInterrupt.tla`](../tla_plus/Concurrency/WaitForAtomicOrInterrupt/WaitForAtomicOrInterrupt.tla)):
within a bounded number of scheduler steps after `markKilled` flips the kill
bit, every thread that was `Waiting` on this primitive returns. The bug config
(`InterruptIsResponsive = FALSE`) produces a TLC counter-example -- the
metastable wake-storm in prose form.

## Scope

- v1 covers the `TicketPool` / `PriorityTicketHolder` queue.
- Future callers: `WaitableMutex` (`util/concurrency/`) and any other hand-rolled
  futex sleep that today polls `checkForInterrupt`.
- Out of scope: changes to `waitForConditionOrInterruptNoAssertUntil` behaviour
  for callers that already participate in network I/O via the baton; that path
  is correct already.

[1]: https://man7.org/linux/man-pages/man2/futex.2.html
