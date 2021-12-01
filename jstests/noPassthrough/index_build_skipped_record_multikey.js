/**
 * Builds an index including a 2dsphere on a collection containing a document which does not conform
 * to the 2dsphere requirements. After this document has been skipped during the collection scan
 * phase but before the index is committed, updates this document to conform to the 2dsphere as well
 * as to flip the index to multikey.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
(function() {
'use strict';

load('jstests/libs/fail_point_util.js');
load('jstests/libs/parallel_shell_helpers.js');

const replTest = new ReplSetTest({nodes: 1});
replTest.startSet();
replTest.initiate();

const primary = replTest.getPrimary();
const coll = primary.getDB('test')[jsTestName()];

assert.commandWorked(coll.insert([
    {
        _id: 0,
        a: {loc: {type: 'Point', coordinates: [-73.97, 40.77]}, num: 0},
    },
    {
        _id: 1,
        a: {
            // Cannot be indexed as a 2dsphere.
            loc: {
                type: 'Polygon',
                coordinates: [
                    // One square.
                    [[9, 9], [9, 11], [11, 11], [11, 9], [9, 9]],
                    // Another disjoint square.
                    [[0, 0], [0, 1], [1, 1], [1, 0], [0, 0]]
                ]
            },
            num: 1,
        }
    }
]));

const fp = configureFailPoint(primary, 'hangAfterIndexBuildFirstDrain');
const awaitCreateIndex = startParallelShell(
    funWithArgs(function(collName) {
        assert.commandWorked(db[collName].createIndex({'a.loc': '2dsphere', 'a.num': 1}));
    }, coll.getName()), primary.port);
fp.wait();

// Two documents are scanned but only one key is inserted.
checkLog.containsJson(primary, 20391, {namespace: coll.getFullName(), totalRecords: 2});
checkLog.containsJson(primary, 20685, {namespace: coll.getFullName(), keysInserted: 1});

// Allows 'a.loc' to be indexed as a 2dsphere and flips the index to multikey.
assert.commandWorked(coll.update({_id: 1}, {
    a: {loc: {type: 'Point', coordinates: [-73.88, 40.78]}, num: [1, 1]},
}));

fp.off();
awaitCreateIndex();

// The skipped document is resolved before the index is committed.
checkLog.containsJson(primary, 23883, {index: 'a.loc_2dsphere_a.num_1', numResolved: 1});

replTest.stopSet();
})();
