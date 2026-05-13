/**
 * Tests that resumable index builds that have been initialized, but not yet begun the collection
 * scan, write their state to disk upon clean shutdown and are resumed from the same phase to
 * completion when the node is started back up.
 *
 * @tags: [
 *   requires_index_build_resumability,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ResumableIndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const dbName = "test";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const runTests = function (docs, indexSpecsFlat, collNameSuffix) {
    const coll = rst
        .getPrimary()
        .getDB(dbName)
        .getCollection(jsTestName() + collNameSuffix);
    assert.commandWorked(coll.insert(docs));

    const runTest = function (indexSpecs) {
        ResumableIndexBuildTest.run(
            rst,
            dbName,
            coll.getName(),
            indexSpecs,
            [{name: "hangIndexBuildBeforeWaitingUntilMajorityOpTime", logIdWithBuildUUID: 4940901}],
            {},
            ["initialized"],
            [{numScannedAfterResume: 1}],
        );
    };

    runTest([[indexSpecsFlat[0]]]);
    runTest([[indexSpecsFlat[0]], [indexSpecsFlat[1]]]);
    runTest([indexSpecsFlat]);
};

runTests({a: 1, b: 1}, [{a: 1}, {b: 1}], "");
runTests({a: [1, 2], b: [1, 2]}, [{a: 1}, {b: 1}], "_multikey");
runTests({a: [1, 2], b: {c: [3, 4]}, d: ""}, [{"$**": 1}, {d: 1}], "_wildcard");

const primary = rst.getPrimary();
if (!FeatureFlagUtil.isPresentAndEnabled(primary.getDB(dbName), "PrimaryDrivenIndexBuilds")) {
    const completedBuilds = checkLog.getFilteredLogMessages(primary, 20663, {
        namespace: function (ns) {
            return ns && ns.startsWith(dbName + ".");
        },
    });
    assert.gt(completedBuilds.length, 0, "Expected at least one resumable build-completion log on the primary");
    for (const entry of completedBuilds) {
        assert.gt(
            entry.attr.numIndexesBefore,
            0,
            "numIndexesBefore should be > 0 after resume rebuild: " + tojson(entry),
        );
    }
}

rst.stopSet();
