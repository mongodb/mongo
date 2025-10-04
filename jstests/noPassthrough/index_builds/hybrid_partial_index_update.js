/**
 * Tests that building partial indexes using the hybrid method preserves multikey information.
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

// Create a partial index for documents where 'a', the field in the filter expression,
// is equal to 1.
const partialIndex = {
    a: 1,
};
const createIdx = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), partialIndex, {
    partialFilterExpression: {a: 1},
});
IndexBuildTest.waitForIndexBuildToScanCollection(testDB, coll.getName(), "a_1");

assert.commandWorked(coll.insert({_id: 0, a: 1}));

// Update the document so that it no longer meets the partial index criteria.
assert.commandWorked(coll.update({_id: 0}, {$set: {a: 0}}));

IndexBuildTest.resumeIndexBuilds(primary);

// Wait for the index build to finish.
createIdx();
IndexBuildTest.assertIndexes(coll, 2, ["_id_", "a_1"]);

let res = assert.commandWorked(coll.validate({full: true}));
assert(res.valid, "validation failed on primary: " + tojson(res));

rst.stopSet();
