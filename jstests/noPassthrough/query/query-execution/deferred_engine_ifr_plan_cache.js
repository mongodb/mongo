/**
 * Verifies plan cache behavior in the presence of QEMP IFR flag changes.
 *
 * @tags: []
 */

import {getQueryPlanner} from "jstests/libs/query/analyze_plan.js";
import {after, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {runWithKnobs} from "jstests/libs/query/knob_utils.js";

// Setup mongod and data.
const conn = MongoRunner.runMongod({
    setParameter: {
        featureFlagGetExecutorDeferredEngineChoice: true,
        featureFlagSbeEqLookupUnwindLocalIxscanFetch: true,
        featureFlagSbeNonLeadingMatch: true,
    },
});
assert.neq(conn, null, "mongod failed to start up");
const db = conn.getDB(jsTestName());

const localColl = db.local;
const foreignColl = db.foreign;

localColl.drop();
for (let i = 0; i < 5; i++) {
    assert.commandWorked(localColl.insert({a: i, b: i + 1}));
}
assert.commandWorked(localColl.createIndex({a: 1}));
assert.commandWorked(localColl.createIndex({b: 1}));

foreignColl.drop();
for (let i = 0; i < 5; i++) {
    assert.commandWorked(foreignColl.insert({b: i}));
}

// {a:0,b:1} is answerable by either index on localColl, so multi-planning fires and writes a
// cache entry.
const pipeline = [
    {$match: {a: 0, b: 1}},
    {$lookup: {from: foreignColl.getName(), localField: "a", foreignField: "b", as: "res"}},
    {$unwind: "$res"},
    {$match: {"res.b": {$gt: -1}}},
];

describe("Test plan cache", function () {
    // LU access-plan and NLS flags are not encoded in the plan cache key, so flipping
    // and rerunning the query shouldn't add entries to the plan cache.
    for (const flag of [
        "featureFlagSbeEqLookupUnwindLocalIxscanFetch",
        "featureFlagSbeNonLeadingMatch",
    ]) {
        it(`${flag} flip`, function () {
            const k1 = localColl.getPlanCache().list()[0].planCacheKey;
            runWithKnobs(
                db,
                () => {
                    localColl.aggregate(pipeline).toArray();

                    // Inspect explain.
                    const explain = localColl.explain().aggregate(pipeline);
                    assert.eq(getQueryPlanner(explain).planCacheKey, k1);

                    // Inspect plan cache.
                    const entries = localColl.getPlanCache().list();
                    assert.eq(entries.length, 1);
                    assert.eq(entries[0].planCacheKey, k1);
                },
                {[flag]: false},
            );
        });
    }

    // The new get executor encodes plan cache keys differently, so we expect an additional
    // plan cache entry to be created after flipping the flag and rerunning the query.
    it("featureFlagGetExecutorDeferredEngineChoice flip", function () {
        const k1 = localColl.getPlanCache().list()[0].planCacheKey;
        runWithKnobs(
            db,
            () => {
                localColl.aggregate(pipeline).toArray();

                // Inspect explain.
                const explain = localColl.explain().aggregate(pipeline);
                const k2 = getQueryPlanner(explain).planCacheKey;
                assert.neq(k2, k1);

                // Inspect plan cache.
                const entries = localColl.getPlanCache().list();
                assert.eq(entries.length, 2);
                assert(entries.map((e) => e.planCacheKey).includes(k1));
                assert(entries.map((e) => e.planCacheKey).includes(k2));
            },
            {featureFlagGetExecutorDeferredEngineChoice: false},
        );

        // Now that we rolled back the feature flag, assert that we still use the previous cache
        // entry.
        localColl.aggregate(pipeline).toArray();

        // Inspect explain.
        const explain = localColl.explain().aggregate(pipeline);
        assert.eq(getQueryPlanner(explain).planCacheKey, k1);

        // Inspect plan cache.
        const entries = localColl.getPlanCache().list();
        assert.eq(entries.length, 2);
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    beforeEach(function () {
        // Warmup cache.
        localColl.getPlanCache().clear();
        for (let i = 0; i < 3; i++) {
            localColl.aggregate(pipeline).toArray();
        }
        const entries = localColl.getPlanCache().list();
        assert.eq(entries.length, 1, "expected one cache entry after warmup");
        assert(entries[0].isActive, "expected active cache entry after warmup");
    });
});
