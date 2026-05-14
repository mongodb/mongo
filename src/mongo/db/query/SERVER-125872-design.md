# SERVER-125872 — Bounded plan-cache hash for nested $jsonSchema

## Problem

Hashing the query solution for a deeply-nested `$jsonSchema` query is exponential in
the nesting depth. A `$jsonSchema` 16 levels deep takes 6-8 minutes of pure CPU to
hash; each additional level roughly doubles the work.

The work is CPU-bound and the plan-enumeration phase does not call
`checkForInterrupt()`, so the operation is unresponsive to both `killOp()` and
`maxTimeMS` for the duration. A small, parser-accepted query body submitted by a
read-only tenant therefore starves every other tenant sharing the host.

## Root cause

`MatchExpressionHashVisitor` is the visitor wired into the outer `tree_walker::walk()`
that produces the canonical hash of a `MatchExpression`. For most node types the
visitor folds local fields into the running hash and lets the outer walker descend
into children. Several `InternalSchema*` visitors did not follow that pattern: they
re-entered the hash machinery for each of their own children, e.g.

```cpp
void visit(const InternalSchemaObjectMatchExpression* expr) final {
    hashCombineCommonProperties(expr);
    combine(MatchExpressionHasher{}(expr->getChild(0)));  // <-- the bug
}
```

`MatchExpressionHasher{}(child)` spawns a fresh `tree_walker::walk()` over the
subtree the outer walker is already about to descend into. Every `InternalSchema*`
node along a nested chain therefore doubles the work below it: hashing an
`InternalSchemaObjectMatchExpression` chain of depth `n` does on the order of `2^n`
node visits instead of `n`.

The same shape exists on `InternalSchemaAllElemMatchFromIndexMatchExpression`,
`InternalSchemaAllowedPropertiesMatchExpression`, and any other visitor that
explicitly recurses through children rather than letting the outer walker do it.

## Fix

Drop the inner `combine(MatchExpressionHasher{}(child))` calls from every affected
`InternalSchema*` visitor. The outer `tree_walker::walk()` already descends into
each child exactly once and runs the visitor on it; the inner call is redundant in
addition to being exponential.

The hash of a query solution is not stable across `mongod` restarts and is not
exposed as a persistent identifier, so removing redundant contributions to the
combined hash is safe — it changes the hash value for affected expressions but
preserves the invariant that `equivalent()` expressions hash equal, which is the
only contract `MatchExpressionEq` / unordered-container callers rely on.

A heavier alternative — memoizing per `MatchExpression*` pointer in a visited set
shared across visitors, or rewriting `MatchExpressionHasher` as a flat iterative
pass — would also work, but is unnecessary once the redundant recursion is removed.
The outer walker already provides the right shape; the visitors just need to stop
re-entering it.

Independently, `QueryPlanner::plan()` should grow a `checkForInterrupt()` call so
future planner regressions stay responsive to `killOp()` and `maxTimeMS`. That work
is tracked separately; this design only addresses the algorithmic blowup.

## Test

`jstests/noPassthrough/query/plan_cache/plan_cache_internal_schema_hash_bounded.js`
builds a 30-level `$jsonSchema` over an empty collection and asserts that both
`explain` and `find` return within a 5-second `maxTimeMS` budget and a 5-second
client-side wall-clock budget, both with cold and warm plan caches. It additionally
exercises an `$_internalSchemaAllElemMatchFromIndex` chain at the same depth so a
fix that only touches `InternalSchemaObjectMatchExpression` cannot silently leave
sibling visitors regressed. On the buggy code path the test wedges for minutes; on
the fixed path each command returns in sub-millisecond planner time.
