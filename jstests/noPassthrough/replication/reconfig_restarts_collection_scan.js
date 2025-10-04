/**
 * Forced re-configs of a replica set will restart the collection scan phase of any in-progress
 * index builds as the collection scan cursor gets interrupted with
 * ReadConcernMajorityNotAvailableYet.
 *
 * @tags: [
 *     requires_replication,
 *     # The inMemory storage engine does not throw ReadConcernMajorityNotAvailableYet on a
 *     # re-config.
 *     requires_persistence,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
                votes: 0,
            },
        },
    ],
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB("test");
const collName = jsTestName();
const coll = testDB.getCollection(collName);

for (let i = 0; i < 10; i++) {
    assert.commandWorked(coll.insert({a: i}));
}

// Hang the collection scan phase of the index build when it's halfway finished.
let fp = configureFailPoint(primary, "hangIndexBuildDuringCollectionScanPhaseAfterInsertion", {fieldsToMatch: {a: 5}});

const awaitCreateIndex = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1});
fp.wait();

// Add a new node to the set.
const newNode = rst.add({rsConfig: {}});
const newNodeObj = {
    _id: 2,
    host: newNode.host,
    priority: 0,
    votes: 0,
};
let config = assert.commandWorked(primary.adminCommand({replSetGetConfig: 1})).config;
config.version++;
config.members.push(newNodeObj);

// Forced re-config will clear the majority committed snapshot.
assert.commandWorked(primary.getDB("admin").runCommand({replSetReconfig: config, force: true}));

fp.off();
checkLog.containsJson(primary, 5470300, {
    error: function (error) {
        return error.code === ErrorCodes.ReadConcernMajorityNotAvailableYet;
    },
}); // Collection scan restarted.
checkLog.containsJson(primary, 20391, {totalRecords: 10}); // Collection scan complete.

awaitCreateIndex();

IndexBuildTest.assertIndexes(coll, 2, ["_id_", "a_1"]);

rst.stopSet();
