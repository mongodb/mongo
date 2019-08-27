/**
 * After a collection is dropped, ensure that the database is closed if there are no remaining
 * collections in the database.
 *
 * @tags: [requires_replication]
 */
(function() {
'use strict';

const replTest = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}]});

replTest.startSet();
replTest.initiate();

const primary = replTest.getPrimary();
const primaryDB = primary.getDB(jsTestName() + '_mydb');
const collToDrop = primaryDB.getCollection('collToDrop');

jsTestLog('Creating and dropping test collection ' + collToDrop.getFullName() +
          ' to create empty database ' + primaryDB.getName() + '.');
assert.commandWorked(primaryDB.createCollection(collToDrop.getName()));
assert(collToDrop.drop());
replTest.awaitReplication();

jsTestLog('Database ' + primaryDB.getName() + ' is now empty. ' +
          'Running restartCatalog on primary.');
assert.commandWorked(primary.adminCommand({restartCatalog: 1}));

const dbNameUpperCase = primaryDB.getName().toUpperCase();
const primaryDBUpperCase = primary.getDB(dbNameUpperCase);
const coll = primaryDBUpperCase.getCollection('collToCreateAfterRestartCatalog');

// This should fail if the database with the lower case name is still around after restarting
// the catalog. Otherwise, we will get a DatabaseDifferCase error while replicating the
// collection creation on the secondary.
jsTestLog('Creating collection ' + coll.getFullName() + ' after running restartCatalog.');
assert.commandWorked(primaryDBUpperCase.createCollection(coll.getName()));
replTest.awaitReplication();

replTest.stopSet();
})();
