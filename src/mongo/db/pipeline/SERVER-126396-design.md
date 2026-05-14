# Design note: single-component system variable in $lookup join predicate

## Symptom

`tassert(16409, "FieldPath::tail() called on single element path")` (server abort) when a $lookup
join predicate references a bare system variable -- `$$NOW`, `$$ROOT`, `$$CURRENT` -- inside a
$expr equality. Repro is in the ticket and pinned by `lookup_single_component_sysvar_predicate.js`.

## Root cause

`PredicateExtractor::extractExpressionCompare()` in
`src/mongo/db/query/compiler/optimizer/join/predicate_extractor.cpp` accepts any $eq whose two
operands are `ExpressionFieldPath` and unconditionally calls
`getFieldPathWithoutCurrentPrefix()` on each:

```cpp
auto leftPathId  = _pathResolver.resolve(left->getFieldPathWithoutCurrentPrefix());
auto rightPathId = _pathResolver.resolve(right->getFieldPathWithoutCurrentPrefix());
```

`getFieldPathWithoutCurrentPrefix()` is defined as `return _fieldPath.tail();`
(`src/mongo/db/pipeline/expression.h`). `FieldPath::tail()` tasserts (16409) when
`getPathLength() <= 1`. Bare system-variable references (`$$NOW`, `$$ROOT`, `$$CURRENT`) are stored
as `ExpressionFieldPath` whose `_fieldPath` is a single component ("NOW" / "ROOT" / "CURRENT"), so
the call aborts.

`$$NOW` reaches this site because it is preserved as an `ExpressionFieldPath` rather than being
constant-folded unless `featureFlagSbeFull` is on. The same path is reached when the system
variable appears in a $lookup `let` + sub-pipeline `$expr`, since the join optimizer walks both
sides of any $eq inside `$expr`.

## Fix sketch

In `extractExpressionCompare()`, gate the `tail()` call on path length before resolving. A bare
single-component `ExpressionFieldPath` is by definition either a system variable (`$$NOW`,
`$$ROOT`, `$$CURRENT`) or an un-prefixed user variable, none of which is a valid equijoin
candidate: there is no field path to push down. The proper behaviour is to mark the predicate
non-extractable and let the planner fall back to evaluating the $expr at the executor layer.

Concrete edit, in both surface sites that call `getFieldPathWithoutCurrentPrefix()` directly on a
visitor-supplied `ExpressionFieldPath`:

```cpp
if (left->getFieldPath().getPathLength() <= 1 ||
    right->getFieldPath().getPathLength() <= 1) {
    _expressionIsFullyAbsorbed = false;
    return;
}
```

The same guard belongs in `JoinPredicateExpr::make()` and in the `localCollectionFieldPath()`
helper (lines 359-367), both of which call `getFieldPathWithoutCurrentPrefix()` on caller-supplied
`ExpressionFieldPath` pointers. A small `static bool isSingleComponentSysvar(const
ExpressionFieldPath*)` helper (returns `expr->isVariableReference() &&
expr->getFieldPath().getPathLength() <= 1`) keeps the three call sites uniform and aligns naming
with the existing `isReferenceToLocalCollectionField()` / `isReferenceToForeignCollectionField()`
predicates.

## Verification

- jstest `lookup_single_component_sysvar_predicate.js` runs four pipelines exercising `$$NOW`,
  `$$ROOT`, `$$CURRENT` in both the downstream-$match shape and the `let` + sub-pipeline shape.
  Each pipeline must complete cleanly or return a non-tassert error; the post-condition `{ping:
  1}` confirms the server is still alive.
- Unit coverage: extend `predicate_extractor_test.cpp` with an `ExpressionCompare` whose operand
  is a single-component `ExpressionFieldPath` and assert the extractor leaves it un-absorbed.

## Out of scope

Constant-folding `$$NOW` ahead of the join optimizer (the `featureFlagSbeFull` path already does
this) is a separate optimisation. The guard above is sufficient to eliminate the crash regardless
of feature-flag state.
