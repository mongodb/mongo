# Design: `$expr` predicates must not push down through `$group` when fields are introduced by the group

## Problem

The pipeline optimiser swaps `$match` before `$group` whenever it can prove the `$match`
predicate depends only on fields that exist on the source documents (the dependency-set
preservation rule originally introduced by SERVER-34741). For ordinary `MatchExpression`
predicates, two follow-on patches (SERVER-91102 and SERVER-102698) tightened the rule by
blocking pushdown for predicates whose semantics differ between source-document evaluation
and post-`$group` evaluation: existence predicates (because `$group` collapses `null` and
missing into a single bucket) and `$type` predicates (because `$group` collapses
numerically-equal values of different numeric types into a single bucket).

`$expr` was not covered by those fixes. Today an `$expr` predicate that mentions a field path
introduced or reshaped by `$group` -- the `_id` group key (or any sub-path of a compound `_id`)
or an accumulator output -- can be pushed before `$group` and evaluated against source
documents that do not have those names, producing silently wrong results. The originating
ticket shows two clear reproductions: `$match: {$expr: {$eq: ["$_id", null]}}` after a group
on a nullable key, and `$match: {$expr: {$eq: ["int", {$type: "$_id"}]}}` after a group on a
numeric key with mixed int/long values. Each query drops rows that should be retained.

## Root cause

The dependency-analysis pass that decides `$expr` pushdown candidacy treats every field path
referenced by the expression as a dependency on the upstream stage's input documents. For
`$match` immediately following `$group`, the upstream input is the *post-group* result stream,
whose fields are `_id` (plus any sub-paths of a compound `_id`) and the accumulator output
names declared in the `$group` spec. The current check does not subtract those names from the
pushdown candidate's dependency set, so the optimiser concludes "the predicate only references
fields the source documents have" and rewrites the pipeline. The two earlier patches built an
analogous block for `MatchExpression`-shaped existence/type predicates, but the `$expr`
codepath bypasses them because it parses its body as an aggregation `Expression`, not a
`MatchExpression`.

## Fix

When computing the pushdown-candidate dependency set for an `$expr` predicate that sits
immediately downstream of `$group`, collect the names produced by the group:

  1. The literal field name `_id`, plus every sub-path of `_id` reachable through compound
     `_id` documents (e.g. `_id.k`, `_id.g`).
  2. Every accumulator output name declared in the `$group` spec (the keys of every
     `AccumulationStatement` other than `_id`).

If the `$expr`'s referenced-field-path set intersects that produced-name set, the predicate
must not be pushed. The fix is local to the dependency-analysis step that the optimiser
consults; the downstream rewrite that performs the actual stage swap is unchanged.

This is conservative on purpose: it errs on the side of correctness, matching the bar set by
SERVER-91102 and SERVER-102698. A future allowlist (already prototyped in a draft PR on the
parent ticket) can re-enable pushdown for the subset of `$expr` shapes whose semantics are
proven to coincide between source-document and post-group evaluation (e.g. `$eq` of a field
path against a non-null literal scalar of a single BSON type, where the field path is *not*
introduced by `$group`). The allowlist is a strict relaxation of the block proposed here, so
shipping the block first is safe and reversible.

## Test

`jstests/aggregation/sources/group/expr_match_no_pushdown_through_group.js` exercises five
cases:

  1. `$match{$expr}` referencing an accumulator output (`total = $sum`) after a `$group` on a
     keyed field. A buggy planner returns zero rows because `total` does not exist on source
     documents; the test asserts the post-group filter result instead.
  2. The null/missing reproduction from the ticket: `$group` collapses null and missing into a
     single `_id: null` bucket and the subsequent `$expr` predicate should retain it.
  3. The `$type` reproduction from the ticket: `$group` collapses int and long into a single
     `_id: 5` bucket of type `int` and the subsequent `$expr` predicate should retain it.
  4. Compound `_id` with both an accumulator output (`total`) and a grouped sub-path (`_id.k`)
     referenced in the same `$expr`, confirming the rule generalises beyond top-level `_id`.
  5. Negative-control: an `$expr` referencing only an accumulator output (`$first: "$region"`)
     produces the correct post-group filter result regardless of any pushdown the planner
     elects to perform on the *non*-introduced fields.

Each case derives its expected output by running the upstream `$group` independently and
filtering its result in JavaScript, so the test pins behaviour to "result is what `$group`
then `$match` would produce" rather than to any particular planner choice.
