/**
 * Tests that an applyOps command with a container insert and delete can be applied.
 *
 * @tags: [requires_replication]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({
    nodes: 1,
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const dbName = jsTestName();
const collName = "coll";
const primaryDB = primary.getDB(dbName);

// TODO(SERVER-109349): Remove this check when the feature flag is removed.
if (!FeatureFlagUtil.isPresentAndEnabled(primaryDB, "PrimaryDrivenIndexBuilds")) {
    jsTestLog(
        "Skipping container_operations.js because featureFlagPrimaryDrivenIndexBuilds is disabled");
    rst.stopSet();
    quit();
}

const ciInnerOp = {
    op: "ci",
    ns: `${dbName}.${collName}`,
    container: "testContainer",
    o: {
        k: BinData(0, "QQ=="),  // 'A'
        v: BinData(0, "Qg=="),  // 'B'
    },
};

const cdInnerOp = {
    op: "cd",
    ns: `${dbName}.${collName}`,
    container: "testContainer",
    o: {
        k: BinData(0, "QQ=="),  // 'A'
    },
};

const applyOpsCmd = {
    applyOps: [ciInnerOp, cdInnerOp],
};

jsTest.log.info(`Applying applyOps: ${tojson(applyOpsCmd)}`);
assert.commandWorked(primaryDB.runCommand(applyOpsCmd));

// TODO (SERVER-107047): Assert that any observable side effects from container ops are present,
// once implemented.

rst.stopSet();
