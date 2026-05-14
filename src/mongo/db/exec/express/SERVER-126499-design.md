# SERVER-126499 — Design: Express write path on DBRef-shaped `_id` literals

## Problem

The Express update/delete fast path tasserts when given an exact-`_id` query
whose value is an object literal whose first field name begins with `$`. The
canonical example is a DBRef literal: `{_id: {$eq: {$ref: "foo", $id: <oid>}}}`.

- Update site: `plan_executor_express.cpp` ~line 714, `tassert(9248801)`.
- Delete site: `plan_executor_express.cpp` ~line 780, `tassert(9248804)`.

## Root cause

Two checks disagree about what counts as an "exact match on `_id`":

1. `isSimpleIdQuery()` (eligibility check) accepts `{_id: {$eq: <object>}}` as
   an exact-`_id` query regardless of the inner object's shape.
2. `getQueryFilterMaybeUnwrapEq()` flattens `{_id: {$eq: <v>}}` into
   `{_id: <v>}` before re-validating with `isExactMatchOnId()`.
3. `isExactMatchOnId()` rejects an `_id` value whose first sub-field name
   starts with `$`, on the assumption that such a name is an operator. A
   literal DBRef value satisfies that pattern but is plain data, so the second
   check fires a `tassert` instead of a user-visible error.

The eligibility check (1) is the authoritative one for the write fast path —
parsing has already classified the query as an exact match. The re-check (3)
exists to protect against unexpected shapes reaching the executor after the
`$eq` unwrap, but it is too coarse: it conflates "first sub-field starts with
`$`" with "this is operator syntax", when in fact the unwrapped value is a
literal that was already validated upstream.

## Proposed fix

Two ordered options, each minimally invasive:

### Option A — relax `isExactMatchOnId` for already-unwrapped values

Tighten `isExactMatchOnId()` so it accepts DBRef-shaped literals. Concretely:
when the `_id` value is an object whose first sub-field name starts with `$`,
inspect the full sub-field set. If every sub-field name begins with `$` and
none of them is a known MatchExpression operator (`$eq`, `$gt`, `$in`, …), the
value is a literal and the function returns `true`. This preserves the
existing rejection of `{_id: {$gt: 1}}` while admitting `{$ref, $id, $db}` and
similar BSON-encoded literal shapes (`$date`, `$oid`, `$numberLong` when used
as data, etc.).

Pros: localized, no change to call sites. Cons: requires maintaining the list
of known operators near `isExactMatchOnId`, which couples it to
`MatchExpressionParser`. Mitigation: pull the operator set from
`MatchExpressionParser`'s public registry rather than hard-coding.

### Option B — bypass the post-unwrap `tassert` and fall back to the classic path

Replace the two `tassert` sites with an `if (!isExactMatchOnId(queryFilter))`
that returns `nullptr` from `makeExpressExecutorFor{Update,Delete}`. The
caller in `get_executor.cpp` already has a fallback for "Express declines";
it will rebuild the query through the classic update/delete executor stack,
which handles literal objects correctly.

Pros: zero behavior change for queries Express already accepts; no operator
registry coupling. Cons: silently widens the set of inputs Express rejects,
which may mask future eligibility bugs.

### Recommendation

Ship Option A. Option B is the safety net if A turns up additional
literal shapes during review. Both options leave `isSimpleIdQuery()` (the
eligibility gate) untouched — the bug is in the re-validation, not in
eligibility.

## Test plan

- `jstests/core/express/express_id_eq_dbref_crash.js` (this changeset) pins
  the crash-free contract for update, delete, and findAndModify with a
  DBRef-shaped `_id` literal.
- Extend `plan_executor_express_test.cpp` with a unit case feeding the
  DBRef literal directly into `isExactMatchOnId` once the fix lands.
- No replication or sharding-specific behavior is exercised; the existing
  `assumes_unsharded_collection` tag on the jstest is sufficient.
