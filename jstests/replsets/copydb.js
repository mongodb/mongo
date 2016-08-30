// Tests the copydb command in a replica set.
// Ensures that documents and indexes are replicated to secondary.

(function() {
    'use strict';

    var replTest = new ReplSetTest({name: 'copydbTest', nodes: 3});

    replTest.startSet();
    replTest.initiate();
    var primary = replTest.getPrimary();
    var secondary = replTest.liveNodes.slaves[0];

    var sourceDBName = 'copydb-repl-test-source';
    var targetDBName = 'copydb-repl-test-target';

    var primarySourceDB = primary.getDB(sourceDBName);
    assert.commandWorked(primarySourceDB.dropDatabase(),
                         'failed to drop source database ' + sourceDBName + ' on primary');

    var primaryTargetDB = primary.getDB(targetDBName);
    assert.commandWorked(primaryTargetDB.dropDatabase(),
                         'failed to drop target database ' + targetDBName + ' on primary');

    assert.writeOK(primarySourceDB.foo.save({a: 1}),
                   'failed to insert document in source collection');
    assert.commandWorked(primarySourceDB.foo.ensureIndex({a: 1}),
                         'failed to create index in source collection on primary');
    assert.commandWorked(primarySourceDB.runCommand({create: "fooView", viewOn: "foo"}),
                         'failed to create view on source collection on primary');

    assert.eq(1,
              primarySourceDB.foo.find().itcount(),
              'incorrect number of documents in source collection on primary before copy');
    assert.eq(1,
              primarySourceDB.fooView.find().itcount(),
              'incorrect number of documents in source view on primary before copy');
    assert.eq(0,
              primaryTargetDB.foo.find().itcount(),
              'target collection on primary should be empty before copy');

    assert.commandWorked(
        primarySourceDB.copyDatabase(primarySourceDB.getName(), primaryTargetDB.getName()),
        'failed to copy database');

    assert.eq(primarySourceDB.foo.find().itcount(),
              primaryTargetDB.foo.find().itcount(),
              'incorrect number of documents in target collection on primary after copy');

    // Confirm that 'fooView' is still a view namespace after copy.
    let res = primaryTargetDB.runCommand({listCollections: 1, filter: {name: "fooView"}});
    assert.commandWorked(res);
    assert(res.cursor.firstBatch.length === 1);
    assert(res.cursor.firstBatch[0].hasOwnProperty("type"), tojson(res));
    assert.eq("view",
              res.cursor.firstBatch[0].type,
              "Namespace exected to be view: " + tojson(res.cursor.firstBatch[0]));

    assert.eq(primarySourceDB.fooView.find().itcount(),
              primaryTargetDB.fooView.find().itcount(),
              'incorrect number of documents in target view on primary after copy');

    assert.eq(primarySourceDB.foo.getIndexes().length,
              primaryTargetDB.foo.getIndexes().length,
              'incorrect number of indexes in target collection on primary after copy');

    replTest.awaitReplication();

    var secondarySourceDB = secondary.getDB(sourceDBName);

    assert.eq(primarySourceDB.foo.find().itcount(),
              secondarySourceDB.foo.find().itcount(),
              'incorrect number of documents in source collection on secondary after copy');

    assert.eq(primarySourceDB.fooView.find().itcount(),
              secondarySourceDB.fooView.find().itcount(),
              'incorrect number of documents in source view on secondary after copy');

    assert.eq(primarySourceDB.foo.getIndexes().length,
              secondarySourceDB.foo.getIndexes().length,
              'incorrect number of indexes in source collection on secondary after copy');

    var secondaryTargetDB = secondary.getDB(targetDBName);

    assert.eq(primaryTargetDB.foo.find().itcount(),
              secondaryTargetDB.foo.find().itcount(),
              'incorrect number of documents in target collection on secondary after copy');

    assert.eq(primaryTargetDB.fooView.find().itcount(),
              secondaryTargetDB.fooView.find().itcount(),
              'incorrect number of documents in target view on secondary after copy');

    assert.eq(primaryTargetDB.foo.getIndexes().length,
              secondaryTargetDB.foo.getIndexes().length,
              'incorrect number of indexes in target collection on secondary after copy');
}());
