/**
 * Tests that the V3 explain verbosity modes are rejected for rooted $or queries.
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";

const collName = jsTestName();

describe("V3 explain verbosity on rooted $or queries", function () {
    let originalPlanOrChildrenIndependently;

    before(function () {
        // Guarantee subplanning is enabled regardless of the environment.
        originalPlanOrChildrenIndependently = assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryPlanOrChildrenIndependently: true}),
        ).was;

        const coll = db[collName];
        coll.drop();
        assert.commandWorked(
            coll.insert([
                {a: 1, b: 1},
                {a: 2, b: 2},
                {a: 2, b: 3},
            ]),
        );
        assert.commandWorked(coll.createIndex({a: 1}));
        assert.commandWorked(coll.createIndex({b: 1}));
    });

    after(function () {
        // Restore the knob to its original value.
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalQueryPlanOrChildrenIndependently: originalPlanOrChildrenIndependently,
            }),
        );
        db[collName].drop();
    });

    // Error codes thrown by the subplanning branches when a V3 verbosity is requested. There are two
    // sites (the standard find path and the deferred-engine-choice path); either may fire depending on
    // the active query execution engine.
    const kV3OnRootedOrErrorCodes = [13145000, 13145001];

    // V3 modes: must be rejected.
    const v3Verbosities = ["planSummary", "plannerChoice", "plannerStats", "execStats"];

    for (const verbosity of v3Verbosities) {
        it(`rejects V3 verbosity '${verbosity}'`, function () {
            assert.commandFailedWithCode(
                db.runCommand({
                    explain: {
                        find: collName,
                        filter: {$or: [{a: 2}, {b: 3}]},
                    },
                    verbosity,
                }),
                kV3OnRootedOrErrorCodes,
            );
        });
    }
});
