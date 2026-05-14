# SERVER-65418 — Release resources before write-conflict backoff

## Problem
`writeConflictRetry` calls `logAndBackoff()` before each retry. Past attempt 3
the helper sleeps. The retrying op keeps its **global write ticket**, document
and collection locks across the sleep. Under a single-doc conflict storm the
ticket pool drains to retriers that are not doing work, and unrelated incoming
writes stall on ticket acquisition.

## Repro
`jstests/noPassthrough/admission/write_conflict_backoff_holds_resources.js`
hammers `_id:"hot"` from N shells while a "victim" writer touches an unrelated
`_id`; the victim's p95 latency is the leak signal.

## Fix
Around the `logAndBackoff()` sleep, when not inside a `WriteUnitOfWork`:

```cpp
Locker::LockSnapshot ls;
const bool released = opCtx->lockState()->saveLockStateAndUnlock(&ls);
ON_BLOCK_EXIT([&] { if (released) opCtx->lockState()->restoreLockState(opCtx, ls); });
ticketHolder->release();   // surrender the write ticket too
logAndBackoff(...);
ticketHolder->acquire(opCtx);
```

Constraints:
1. **Skip release inside a WUOW** — locks/tickets are part of the active txn.
2. **Preserve interrupt semantics** — `restoreLockState` must propagate
   `OperationContext::checkForInterrupt()` exactly as before.
3. **Idempotent on early return** — `ON_BLOCK_EXIT` covers the throw path so a
   killed op does not leak a half-released snapshot.
4. **Ordering** — release locks *then* ticket; reacquire ticket *then* locks
   (mirrors the normal admission pipeline; avoids ticket-while-locked
   reordering that the deadlock detector keys off).

## Open questions
- Should the ticket release be conditional on backoff attempt count, or always
  applied past attempt 3? Latter is simpler; former preserves cache warmth.
- Does the fix interact with the prepare-conflict retry path (different sleep
  loop, same resource-holding shape)? Likely yes — fold into a shared helper.

## Out of scope (parent epic SPM-4627 / PROGRAM-114)
Pessimistic-CC opt-in surfaced in the latest customer comment (Magenta load
test) is a separate API change; this ticket only fixes the resource-leak
during the existing optimistic retry loop.
