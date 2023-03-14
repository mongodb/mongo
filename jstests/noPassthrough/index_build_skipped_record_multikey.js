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

const replTest = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
            },
        },
    ],
    nodeOptions: {setParameter: {logComponentVerbosity: tojson({index: 2})}}
});
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

const secondary = replTest.getSecondary();
const fpSecondaryDrain = configureFailPoint(secondary, 'hangAfterIndexBuildFirstDrain');

// We don't want the primary to observe a non-conforming document, as that would abort the build.
// Hang before collection scan starts.
const fpPrimarySetup = configureFailPoint(primary, 'hangAfterInitializingIndexBuild');

const indexKeyPattern = {
    'a.loc': '2dsphere',
    'a.num': 1
};
const awaitCreateIndex =
    startParallelShell(funWithArgs(function(collName, keyPattern) {
                           assert.commandWorked(db[collName].createIndex(keyPattern));
                       }, coll.getName(), indexKeyPattern), primary.port);
fpSecondaryDrain.wait();

// Two documents are scanned but only one key is inserted.
checkLog.containsJson(secondary, 20391, {namespace: coll.getFullName(), totalRecords: 2});
checkLog.containsJson(secondary, 20685, {namespace: coll.getFullName(), keysInserted: 1});

// Allows 'a.loc' to be indexed as a 2dsphere and flips the index to multikey.
assert.commandWorked(coll.update({_id: 1}, {
    a: {loc: {type: 'Point', coordinates: [-73.88, 40.78]}, num: [1, 1]},
}));

fpSecondaryDrain.off();
fpPrimarySetup.off();
awaitCreateIndex();

// The skipped document is resolved, and causes the index to flip to multikey.
// "Index set to multi key ..."
checkLog.containsJson(
    secondary, 4718705, {namespace: coll.getFullName(), keyPattern: indexKeyPattern});

replTest.stopSet();
})();
