/**
 * Confirms that aborting a background index builds on a secondary before the primary commits
 * results in a consistent state with no crashing.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_build.js";

// This test triggers an unclean shutdown (an fassert), which may cause inaccurate fast counts.
TestData.skipEnforceFastCountOnValidate = true;

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            // Lower priority than the primary, but allow the secondary to vote and participate in
            // commitQuorum.
            rsConfig: {
                priority: 0,
            },
            slowms: 30000,  // Don't log slow operations on secondary. See SERVER-44821.
        },
        {
            // The arbiter prevents the primary from stepping down due to lack of majority in the
            // case where the secondary is restarting due to the (expected) unclean shutdown. Note
            // that the arbiter doesn't participate in the commitQuorum.
            rsConfig: {
                arbiterOnly: true,
            },
        },
    ]
});
const nodes = rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let primaryDB = primary.getDB('test');
let primaryColl = primaryDB.getCollection('test');

assert.commandWorked(primaryColl.insert({a: 1}));

let secondary = rst.getSecondary();
IndexBuildTest.pauseIndexBuilds(secondary);

const createIdx = IndexBuildTest.startIndexBuild(
    primary, primaryColl.getFullName(), {a: 1}, {}, ErrorCodes.IndexBuildAborted);

// When the index build starts, find its op id.
let secondaryDB = secondary.getDB(primaryDB.getName());
const opId =
    IndexBuildTest.waitForIndexBuildToScanCollection(secondaryDB, primaryColl.getName(), "a_1");

IndexBuildTest.assertIndexBuildCurrentOpContents(secondaryDB, opId, (op) => {
    jsTestLog('Inspecting db.currentOp() entry for index build: ' + tojson(op));
    assert.eq(primaryColl.getFullName(),
              op.ns,
              'Unexpected ns field value in db.currentOp() result for index build: ' + tojson(op));
});

// Kill the index build on the secondary. With the feature flag enabled, this should signal the
// primary to abort the index build.
assert.commandWorked(secondaryDB.killOp(opId));

// Expect the secondary to successfully prevent the primary from committing the index build.
checkLog.containsJson(secondary, 20655);

primary = rst.getPrimary();
rst.awaitSecondaryNodes();
primaryDB = primary.getDB('test');
primaryColl = primaryDB.getCollection('test');

secondary = rst.getSecondary();
secondaryDB = secondary.getDB(primaryDB.getName());
const secondaryColl = secondaryDB.getCollection(primaryColl.getName());

// Wait for the index build to complete on all nodes.
rst.awaitReplication();

// Expect successful createIndex command invocation in parallel shell. A new index should be present
// on the primary and secondary.
createIdx();

// Check that index was aborted by the killOp().
IndexBuildTest.assertIndexes(primaryColl, 1, ['_id_']);
IndexBuildTest.assertIndexes(secondaryColl, 1, ['_id_']);

rst.stopSet();
