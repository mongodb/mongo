# SERVER-122109 — Selective PIT restore must filter post-checkpoint DDL ops

## Problem

Selective point-in-time backup/restore copies data files only for the user-selected
namespaces at the checkpoint timestamp `Tbackup`, then captures the full (non-selective)
oplog from `Tbackup` onward. On restore, the selected data files are placed on disk
and the captured oplog is replayed end-to-end against the recovered node. The replay
relies on a NamespaceNotFound relaxation in `OplogApplication::checkOnOplogFailureForRecovery`
(see `src/mongo/db/repl/oplog.cpp:1649`) so that CRUD ops against unrestored
collections silently no-op.

That contract holds for CRUD `i`/`u`/`d` ops because every such op targets a namespace
that already needs to exist. It breaks for `c` (command) DDL ops that *create* a
namespace: `create`, `createIndexes`, `renameCollection` (target), and database-level
view creation. These commands have no precondition that the namespace already exists —
they succeed and write a fresh catalog entry. The result, exactly as the ticket
describes:

> A replica set has collections a, b, c, d. The user selects only a and b. After
> Tbackup a new collection e is created. After restore the cluster has a, b and e.

Collection e was never selected. It only survives because the oplog applier replays
its `create` non-selectively.

## Root cause

`OplogApplierImpl::_applyOplogBatch` and `applyOperation_inlock` apply every entry in
the captured oplog. The selective-restore branch in
`OplogApplication::checkOnOplogFailureForRecovery` only relaxes *errors* (it converts
NamespaceNotFound into a logged no-op); it does not *gate* application. For CRUD ops
gating happens implicitly via the error path. For `c` DDL ops the equivalent gate
does not exist because the op never errors on its own target's absence — that is the
whole point of `create`.

The buggy commands are `create` (collection and view), `createIndexes` (would land
an index on a resurrected collection), `renameCollection` (when either endpoint
straddles the selected namespace set), and `collMod`/`dropIndexes`/`drop` (no-op on
absent ns today, but worth gating for audit-trail clarity).

## Fix

Extend the oplog filter to apply at the `c`-op classification stage, before the
command is dispatched to its handler. The smallest correct change:

1. Plumb the user-selected namespace allowlist (already loaded for the data-file copy
   step) into the recovery `OplogApplier` via a new
   `storageGlobalParams.restoreNamespaces` set, populated alongside the existing
   `storageGlobalParams.restore` flag at startup.
2. In `applyOperation_inlock`, when `mode == kRecovering && storageGlobalParams.restore`
   and the entry's op type is `c`, resolve the target namespace from the command
   object (`extractNs(...)` already exists at line 1695) and check membership against
   the allowlist. On miss, log at `LOGV2_DEBUG(level=1)` mirroring the existing
   NamespaceNotFound relaxation and return `Status::OK()` without dispatching.
3. For `renameCollection` specifically, treat the op as in-scope iff *either* the
   source or the target is in the allowlist — out-of-scope only when both endpoints
   are unselected. Filtering when only the source is selected would otherwise
   silently lose data the user explicitly asked for.
4. Match the same allowlist against `i`/`u`/`d` ops as a defense-in-depth so we no
   longer depend on NamespaceNotFound firing — useful when an unrelated bug
   accidentally re-creates an unrestored namespace.

## Test surface

`jstests/replsets/selective_pit_restore_post_checkpoint_filter.js` (this ticket)
pins the contract: post-restore `keep.coll` carries pre- and post-checkpoint writes;
post-restore `drop.coll` does not exist in the catalog. Today the second assertion
fails; after the filter lands it passes.

## Out of scope

- The shard-catalog flavour of selective restore (sharded clusters) — same root
  cause but a separate test surface owned by Sharding.
- Multi-tenant tenant filtering — already gated upstream by tenant id.
