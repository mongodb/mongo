/**
 * Regression test: $_internalSchemaCond is an internal operator used to implement schema
 * conditional logic (e.g. $jsonSchema's "dependencies" keyword). It must reject $text and $near
 * as nested sub-expressions with a clean BadValue error.
 *
 * Before the fix, TEXT inside $_internalSchemaCond was never tagged by rateIndices (because
 * InternalSchemaCondMatchExpression::getCategory() returns kOther), causing a null-pointer
 * dereference in QueryPlanner::plan.
 *
 * @tags: [
 *   multiversion_incompatible,
 * ]
 */
const coll = db[jsTestName()];
coll.drop();
assert.commandWorked(coll.createIndex({a: "text"}));
assert.commandWorked(coll.insert({a: "hello world", loc: [0, 0]}));

// $text nested in the if-arm, then-arm, and else-arm.
assert.commandFailedWithCode(
    db.runCommand({
        find: coll.getName(),
        filter: {$_internalSchemaCond: [{$text: {$search: "hello"}}, {a: 1}, {b: 1}]},
    }),
    ErrorCodes.BadValue,
);

assert.commandFailedWithCode(
    db.runCommand({
        find: coll.getName(),
        filter: {$_internalSchemaCond: [{a: 1}, {$text: {$search: "hello"}}, {b: 1}]},
    }),
    ErrorCodes.BadValue,
);

assert.commandFailedWithCode(
    db.runCommand({
        find: coll.getName(),
        filter: {$_internalSchemaCond: [{a: 1}, {b: 1}, {$text: {$search: "hello"}}]},
    }),
    ErrorCodes.BadValue,
);

// $near nested in the if-arm, then-arm, and else-arm.
assert.commandFailedWithCode(
    db.runCommand({
        find: coll.getName(),
        filter: {$_internalSchemaCond: [{loc: {$near: [0, 0]}}, {a: 1}, {b: 1}]},
    }),
    ErrorCodes.BadValue,
);

assert.commandFailedWithCode(
    db.runCommand({
        find: coll.getName(),
        filter: {$_internalSchemaCond: [{a: 1}, {loc: {$near: [0, 0]}}, {b: 1}]},
    }),
    ErrorCodes.BadValue,
);

assert.commandFailedWithCode(
    db.runCommand({
        find: coll.getName(),
        filter: {$_internalSchemaCond: [{a: 1}, {b: 1}, {loc: {$near: [0, 0]}}]},
    }),
    ErrorCodes.BadValue,
);
