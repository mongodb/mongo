/**
 * Tests that index builds are executed as deprioritizable background operations by verifying that
 * the totalDeprioritizations counter in serverStatus increases during an index build on both
 * primary and secondary.
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";
import {
    getTotalDeprioritizationCount,
    insertTestDocuments,
} from "jstests/noPassthrough/admission/execution_control/libs/execution_control_helper.js";

const dbName = jsTestName();

describe("index builds are deprioritizable", function () {
    let replTest, primary, secondary, primaryDb, coll, secondaryDB;

    before(function () {
        replTest = new ReplSetTest({
            nodes: 2,
            nodeOptions: {
                setParameter: {
                    executionControlDeprioritizationGate: true,
                    // Disable heuristic deprioritization so the counter only
                    // reflects background tasks deprioritizations.
                    executionControlHeuristicDeprioritization: false,
                },
            },
        });
        replTest.startSet();
        replTest.initiate();
        primary = replTest.getPrimary();
        secondary = replTest.getSecondary();
        primaryDb = primary.getDB(dbName);
        coll = primaryDb.coll;
        secondaryDB = secondary.getDB(dbName);

        insertTestDocuments(coll, 1000, {
            docGenerator: (i) => ({_id: i, x: `value_${i}`}),
        });
    });

    after(function () {
        replTest.stopSet();
    });

    it("incremented totalDeprioritizations counter on primary and secondary during index build", function () {
        const primaryBefore = getTotalDeprioritizationCount(primary);
        const secondaryBefore = getTotalDeprioritizationCount(secondary);

        assert.commandWorked(coll.createIndex({x: 1}));
        IndexBuildTest.waitForIndexBuildToStop(primaryDb);
        IndexBuildTest.waitForIndexBuildToStop(secondaryDB);

        assert.gt(
            getTotalDeprioritizationCount(primary),
            primaryBefore,
            "totalDeprioritizations should increase on primary during index build",
        );
        assert.gt(
            getTotalDeprioritizationCount(secondary),
            secondaryBefore,
            "totalDeprioritizations should increase on secondary during index build",
        );
    });
});
