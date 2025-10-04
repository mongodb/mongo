/**
 * Tests that building sparse compound geo indexes using the hybrid method preserves multikey
 * information.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
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
const nodes = rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB("test");
const coll = testDB.getCollection("test");

assert.commandWorked(testDB.createCollection(coll.getName()));

// Insert document into collection to avoid optimization for index creation on an empty collection.
// This allows us to pause index builds on the collection using a fail point.
assert.commandWorked(coll.insert({a: 1}));

IndexBuildTest.pauseIndexBuilds(primary);

const createIdx = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1, b: "2dsphere"}, {sparse: true});
IndexBuildTest.waitForIndexBuildToScanCollection(testDB, coll.getName(), "a_1_b_2dsphere");

assert.commandWorked(coll.insert({a: [1, 2]}));

IndexBuildTest.resumeIndexBuilds(primary);

// Wait for the index build to finish.
createIdx();
IndexBuildTest.assertIndexes(coll, 2, ["_id_", "a_1_b_2dsphere"]);

let res = assert.commandWorked(coll.validate({full: true}));
assert(res.valid, "validation failed on primary: " + tojson(res));

rst.stopSet();
