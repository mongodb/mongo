/**
 * Runs initial sync on a node with many databases.
 */

(function() {

var name = 'initial_sync_many_dbs';
var num_dbs = 32;
var max_colls = 32;
var num_docs = 2;
var replSet = new ReplSetTest({
    name: name,
    nodes: 1,
});
replSet.startSet();
replSet.initiate();

var primary = replSet.getPrimary();
jsTestLog('Seeding primary with ' + num_dbs + ' databases with up to ' + max_colls +
          ' collections each. Each collection will contain ' + num_docs + ' documents');
for (var i = 0; i < num_dbs; i++) {
    var dbname = name + '_db' + i;
    for (var j = 0; j < (i % max_colls + 1); j++) {
        var collname = name + '_coll' + j;
        var coll = primary.getDB(dbname)[collname];
        for (var k = 0; k < num_docs; k++) {
            assert.commandWorked(coll.insert({_id: k}));
        }
    }
}

// Add a secondary that will initial sync from the primary.
jsTestLog('Adding node to replica set to trigger initial sync process');
replSet.add();
replSet.reInitiate();

replSet.awaitSecondaryNodes(30 * 60 * 1000);
var secondary = replSet.getSecondary();
jsTestLog('New node has transitioned to secondary. Checking collection sizes');
for (var i = 0; i < num_dbs; i++) {
    var dbname = name + '_db' + i;
    for (var j = 0; j < (i % max_colls + 1); j++) {
        var collname = name + '_coll' + j;
        var coll = secondary.getDB(dbname)[collname];
        assert.eq(
            num_docs,
            coll.find().itcount(),
            'collection size inconsistent with primary after initial sync: ' + coll.getFullName());
    }
}

replSet.stopSet();
})();
