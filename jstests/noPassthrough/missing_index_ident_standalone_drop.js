/**
 * Tests that if a node is in a state where the catalog entry for an index references a missing
 * ident, the node can be started as a standalone and the index can be dropped.
 *
 * @tags: [
 *     requires_persistence,
 *     requires_replication,
 * ]
 */
(function() {
'use strict';

load('jstests/noPassthrough/libs/missing_index_ident.js');

const {replTest, dbpath} = MissingIndexIdent.run();

const standalone = MongoRunner.runMongod({
    dbpath: dbpath,
    noCleanData: true,
});
const coll = standalone.getDB('test')[jsTestName()];

IndexBuildTest.assertIndexes(coll, 2, ['_id_', 'a_1']);
assert.commandWorked(standalone.getDB('test')[jsTestName()].dropIndex('a_1'));
IndexBuildTest.assertIndexes(coll, 1, ['_id_']);

// Completing drop for index table immediately.
checkLog.containsJson(standalone, 6361201, {
    index: 'a_1',
    namespace: coll.getFullName(),
});

MongoRunner.stopMongod(standalone);
replTest.start(0, undefined, true /* restart */);
IndexBuildTest.assertIndexes(replTest.getPrimary().getDB('test')[jsTestName()], 1, ['_id_']);

replTest.stopSet();
})();
