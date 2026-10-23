/**
 * Exercises schema metadata rebuilds when collection validation options change. Demonstrates that a
 * query consuming a field's validator-derived type is invalidated across a getMore only when a
 * collMod republishes a *different* inferred type, and survives a collMod that leaves the inferred
 * type unchanged.
 */

const mongod = MongoRunner.runMongod({
    setParameter: {
        featureFlagQueryTypeInference: true,
        internalQueryExecYieldIterations: 1,
        internalQueryExecYieldPeriodMS: 60000,
        // SBE's filter generator is the only consumer of the inferred type, so the query below is
        // only schema-dependent when it is lowered to SBE.
        internalQueryFrameworkControl: "trySbeEngine",
    },
});
const db = mongod.getDB(jsTestName());
const coll = db.schema;

assert.commandWorked(
    db.createCollection(coll.getName(), {
        validator: {a: {$type: "string"}},
        validationLevel: "constraint",
        validationAction: "error",
    }),
);

assert.commandWorked(coll.insert({_id: 0, a: "initial"}));
assert.eq(1, coll.find({a: "initial"}).itcount());

// The validator is immutable at 'constraint', so replacing it requires lowering the level first.
// Both steps must rebuild the collection's schema metadata.
assert.commandWorked(db.runCommand({collMod: coll.getName(), validationLevel: "strict"}));
assert.commandWorked(
    db.runCommand({
        collMod: coll.getName(),
        validator: {a: {$type: "number"}},
        validationLevel: "strict",
    }),
);

// Upgrading to 'constraint' scans for compliance, so every document must satisfy the new validator.
assert.commandWorked(coll.remove({_id: 0}));
assert.commandWorked(coll.insert({_id: 1, a: 1}));
assert.commandWorked(
    db.runCommand({collMod: coll.getName(), prepareConstraintValidationLevel: true}),
);
assert.commandWorked(db.runCommand({collMod: coll.getName(), validationLevel: "constraint"}));
assert.commandFailedWithCode(
    coll.insert({_id: 2, a: "wrong type"}),
    ErrorCodes.DocumentValidationFailure,
);
assert.eq(1, coll.find({a: 1}).itcount());

// Relaxing the validation level must rebuild conservative metadata. 'strict' still validates every
// insert, so 'off' is needed to store a document the validator rejects.
// Disabling validation must rebuild conservative metadata.
assert.commandWorked(
    db.runCommand({
        collMod: coll.getName(),
        validationLevel: "off",
    }),
);
assert.commandWorked(coll.insert({_id: 3, a: "legacy"}));
assert.eq(1, coll.find({a: "legacy"}).itcount());

// Remove documents that don't satisfy the validator before preparing to return to constraint.
assert.commandWorked(coll.deleteMany({}));
assert.commandWorked(
    db.runCommand({
        collMod: coll.getName(),
        validationLevel: "strict",
    }),
);
assert.commandWorked(
    db.runCommand({collMod: coll.getName(), prepareConstraintValidationLevel: true}),
);
assert.commandWorked(
    db.runCommand({
        collMod: coll.getName(),
        validationLevel: "constraint",
    }),
);

// Returning to constraint must rebuild the validator-derived metadata again.
assert.commandFailedWithCode(
    coll.insert({_id: 4, a: "wrong again"}),
    ErrorCodes.DocumentValidationFailure,
);
assert.commandWorked(coll.insert({_id: 4, a: 3}));
assert.eq(1, coll.find({a: 3}).itcount());

// The assertions above show that document validation behaves correctly after each collMod, but the
// inferred schema metadata that queries consume changes behind the scenes. Demonstrate that it is
// specifically the validator-derived type (not merely a collMod version bump) that changes: a query
// consuming 'a's inferred type is invalidated across a getMore only when a collMod republishes a
// *different* inferred type.
//
// A query is schema-dependent only when the validator proves 'a' can never be an array: a plain
// '$type' predicate still matches arrays of that type, so the '$not: {$type: array}' conjunct is
// what rules arrays out. Transition the validator to that form (constraint validators are immutable,
// so lower the level before replacing it, then upgrade back).
assert.commandWorked(db.runCommand({collMod: coll.getName(), validationLevel: "strict"}));
assert.commandWorked(
    db.runCommand({
        collMod: coll.getName(),
        validator: {$and: [{a: {$type: "number"}}, {a: {$not: {$type: "array"}}}]},
        validationLevel: "strict",
    }),
);
assert.commandWorked(
    db.runCommand({collMod: coll.getName(), prepareConstraintValidationLevel: true}),
);
assert.commandWorked(db.runCommand({collMod: coll.getName(), validationLevel: "constraint"}));

// Insert more matching documents so a batchSize-1 cursor stays open across the collMod.
assert.commandWorked(
    coll.insert([
        {_id: 5, a: 5},
        {_id: 6, a: 6},
    ]),
);

const kSchemaChangedMsg = "collection schema metadata changed during yield";

// Opens an SBE cursor whose '$match: {a: {$type: ...}}' consumes 'a's inferred type, runs
// 'betweenBatchesFn', then asserts whether the getMore survives. Internal yields are disabled so the
// only metadata check happens on the getMore restore, isolating schema invalidation from the
// generic catalog-version invalidation that any collMod would trigger during an internal yield.
function assertGetMoreAcrossCollMod(betweenBatchesFn, {expectKilled}) {
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            internalQueryExecYieldIterations: -1,
            internalQueryExecYieldPeriodMS: 60 * 60 * 1000,
        }),
    );
    try {
        const open = assert.commandWorked(
            db.runCommand({
                aggregate: coll.getName(),
                pipeline: [{$match: {a: {$type: "number"}}}],
                cursor: {batchSize: 1},
            }),
        );
        assert.neq(NumberLong(0), open.cursor.id, open);

        betweenBatchesFn();

        const getMore = db.runCommand({getMore: open.cursor.id, collection: coll.getName()});
        if (expectKilled) {
            assert.commandFailedWithCode(getMore, ErrorCodes.QueryPlanKilled);
            assert(getMore.errmsg.includes(kSchemaChangedMsg), getMore);
        } else {
            assert.commandWorked(getMore);
        }
    } finally {
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalQueryExecYieldIterations: 1,
                internalQueryExecYieldPeriodMS: 60000,
            }),
        );
    }
}

// A collMod that leaves the inferred type unchanged (only 'validationAction' changes) republishes
// identical metadata, so the cursor survives: the checker compares inferred types, not a version.
assertGetMoreAcrossCollMod(
    () =>
        assert.commandWorked(
            db.runCommand({collMod: coll.getName(), validationAction: "errorAndLog"}),
        ),
    {expectKilled: false},
);

// Replacing the validator with one that infers a different type for 'a' republishes different
// metadata, so the cursor that consumed the old type is killed on the getMore restore.
assertGetMoreAcrossCollMod(
    () =>
        assert.commandWorked(
            db.runCommand({
                collMod: coll.getName(),
                validator: {a: {$type: "string"}},
                validationLevel: "strict",
            }),
        ),
    {expectKilled: true},
);

MongoRunner.stopMongod(mongod);
