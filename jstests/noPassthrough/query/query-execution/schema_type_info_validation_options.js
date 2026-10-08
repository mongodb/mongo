/**
 * Exercises schema metadata rebuilds when collection validation options change. Demonstrates that a
 * query consuming a field's validator-derived type is invalidated across a getMore only when a
 * collMod republishes a *different* inferred type, and survives a collMod that leaves the inferred
 * type unchanged. Also demonstrates that a collMod republishing a different inferred type clears the
 * collection's plan cache so cached plans are rebuilt with the new metadata. This is exercised with
 * the query run under both the classic and SBE engines; in the default configuration (no
 * 'featureFlagSbeFull') the SBE plan cache is disabled, so both cases are backed by the classic plan
 * cache.
 */
import {getEngine, getQueryPlanners} from "jstests/libs/query/analyze_plan.js";

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

// The getMore assertions above cover an *open cursor* that is invalidated mid-query. The plan cache
// is a separate consumer of the validator-derived type: a plan cached under the old inferred type
// must be discarded when a collMod republishes a different type, so the next execution is replanned
// under the new metadata. Exercise that end-to-end with the query executed by both the classic
// engine and SBE. In the default configuration the SBE plan cache is disabled (it requires
// 'featureFlagSbeFull'), so even the SBE-executed query is cached in the classic plan cache; both
// cases below therefore exercise the classic plan cache.

// A collection whose validator proves 'a' is a number that is never an array. A plain '$type'
// predicate still matches arrays of that type, so the '$not: {$type: array}' conjunct is what rules
// arrays out, and the inferred type is only derived from the validator at the 'constraint' level.
// Two single-field indexes force multi-planning (so the winning plan is cached), while the residual
// equality predicate on the unindexed field 'a' is what consumes the inferred type.
function setUpPlanCacheColl(name) {
    const planCacheColl = db[name];
    planCacheColl.drop();
    assert.commandWorked(
        db.createCollection(planCacheColl.getName(), {
            validator: {$and: [{a: {$type: "number"}}, {a: {$not: {$type: "array"}}}]},
            validationLevel: "constraint",
        }),
    );
    assert.commandWorked(planCacheColl.createIndex({b: 1}));
    assert.commandWorked(planCacheColl.createIndex({c: 1}));
    assert.commandWorked(
        planCacheColl.insert([
            {_id: 0, a: 1, b: 1, c: 1}, // matches the query below
            {_id: 1, a: 1, b: 1, c: 2},
            {_id: 2, a: 2, b: 1, c: 1},
            {_id: 3, a: 1, b: 2, c: 1},
        ]),
    );
    return planCacheColl;
}

function numPlanCacheEntries(planCacheColl) {
    return planCacheColl.getPlanCache().list().length;
}

// Changes the collection's validation options so 'a's inferred type changes. Constraint validators
// are immutable, so lower the level first (which alone drops the validator-derived type back to the
// conservative default), then install a different validator. 'a' may now be an array as far as the
// planner knows.
function changeValidatorInferredType(planCacheColl) {
    assert.commandWorked(
        db.runCommand({collMod: planCacheColl.getName(), validationLevel: "strict"}),
    );
    assert.commandWorked(
        db.runCommand({
            collMod: planCacheColl.getName(),
            validator: {a: {$type: "number"}},
            validationLevel: "strict",
        }),
    );
}

// Query executed by the classic engine (backed by the classic plan cache): the classic engine does
// not consume the inferred type, so we assert that the stale entry is dropped and the query is
// replanned (a fresh entry is created).
(function classicCacheEntryRemovedAndReplannedOnValidatorTypeChange() {
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}),
    );
    const planCacheColl = setUpPlanCacheColl("plan_cache_classic");
    const query = {a: 1, b: 1, c: 1};

    // Warm the cache: the first execution multi-plans and creates the entry; running twice promotes
    // it to an active entry.
    assert.eq(1, planCacheColl.find(query).itcount());
    assert.eq(1, planCacheColl.find(query).itcount());
    assert.eq(
        1,
        numPlanCacheEntries(planCacheColl),
        () =>
            "expected a cached plan under the original inferred type: " +
            tojson(planCacheColl.getPlanCache().list()),
    );

    changeValidatorInferredType(planCacheColl);

    // The collMod republished a different inferred type, so it must have cleared the plan cache.
    assert.eq(
        0,
        numPlanCacheEntries(planCacheColl),
        "collMod must clear the plan cache when the validator-derived type changes",
    );

    // Re-running replans under the new metadata and recreates the entry.
    assert.eq(1, planCacheColl.find(query).itcount());
    assert.eq(1, planCacheColl.find(query).itcount());
    assert.eq(
        1,
        numPlanCacheEntries(planCacheColl),
        () =>
            "expected the query to be replanned and recached: " +
            tojson(planCacheColl.getPlanCache().list()),
    );
})();

// Query executed by SBE: this still uses the classic plan cache (the SBE plan cache requires
// 'featureFlagSbeFull', which is off by default), but because SBE *does* consume the inferred type
// (it elides the array traversal 'traverseF' for a predicate on a field the inferred type proves
// can never be an array), beyond the entry being cleared we can observe that the cached plan is
// rebuilt and re-lowered to SBE with the new metadata.
(function sbeCacheEntryRemovedAndReplannedWithNewMetadata() {
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "trySbeEngine"}),
    );
    const planCacheColl = setUpPlanCacheColl("plan_cache_sbe");
    // A '$group' forces the pipeline (including its '$match' prefix) to be lowered to SBE, which is
    // the only engine that consumes the inferred type.
    const pipeline = [{$match: {a: 1, b: 1, c: 1}}, {$group: {_id: null, n: {$sum: 1}}}];

    // The winning SBE plan's stages, asserting the pipeline was lowered to SBE.
    function winningSbeStages() {
        const explain = planCacheColl.explain().aggregate(pipeline);
        assert.eq(getEngine(explain), "sbe", () => "expected SBE engine: " + tojson(explain));
        return getQueryPlanners(explain)[0].winningPlan.slotBasedPlan.stages;
    }

    // Warm the cache: the first execution multi-plans and creates the entry; running twice promotes
    // it to an active entry.
    assert.eq([{_id: null, n: 1}], planCacheColl.aggregate(pipeline).toArray());
    assert.eq([{_id: null, n: 1}], planCacheColl.aggregate(pipeline).toArray());
    assert.eq(
        1,
        numPlanCacheEntries(planCacheColl),
        () =>
            "expected a cached plan under the original inferred type: " +
            tojson(planCacheColl.getPlanCache().list()),
    );

    // Under the "cannot be an array" inferred type, SBE elides 'traverseF' for the 'a' predicate.
    assert(
        !winningSbeStages().includes("traverseF"),
        () =>
            "expected 'traverseF' to be elided under a non-array inferred type: " +
            winningSbeStages(),
    );

    changeValidatorInferredType(planCacheColl);

    // The collMod republished a different inferred type, so it must have cleared the plan cache.
    assert.eq(
        0,
        numPlanCacheEntries(planCacheColl),
        "collMod must clear the plan cache when the validator-derived type changes",
    );

    // Re-running replans under the new metadata and recreates the entry.
    assert.eq([{_id: null, n: 1}], planCacheColl.aggregate(pipeline).toArray());
    assert.eq([{_id: null, n: 1}], planCacheColl.aggregate(pipeline).toArray());
    assert.eq(
        1,
        numPlanCacheEntries(planCacheColl),
        () =>
            "expected the query to be replanned and recached: " +
            tojson(planCacheColl.getPlanCache().list()),
    );

    // Now that 'a' may be an array, the replanned SBE plan must regenerate 'traverseF',
    // demonstrating the query was recompiled with the new inferred metadata rather than reusing the
    // stale plan.
    assert(
        winningSbeStages().includes("traverseF"),
        () =>
            "expected 'traverseF' to be regenerated once 'a' may be an array: " +
            winningSbeStages(),
    );
})();

MongoRunner.stopMongod(mongod);
