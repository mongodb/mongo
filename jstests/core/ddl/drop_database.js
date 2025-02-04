/**
 * Test that a db does not exist after it is dropped.
 *
 * @tags: [
 *   # listDatabases with explicit filter on db names doesn't work with the simulate_atlas_proxy
 *   # override.
 *   simulate_atlas_proxy_incompatible,
 * ]
 */

let testDB = db.getSiblingDB('jstests_dropdb');
const dbName = testDB.getName();
const collNames = ['coll1', 'coll2', 'coll3'];

function listDatabases(options) {
    return assert
        .commandWorked(db.adminCommand(Object.assign({listDatabases: 1, nameOnly: true}, options)))
        .databases;
}

function assertNamespacesDoNotExist() {
    assert.eq(0, listDatabases({filter: {name: dbName}}).length);
    assert.eq(0, testDB.getCollectionNames());
}

function assertNamespacesExist() {
    assert.eq(1,
              listDatabases({filter: {name: dbName}}).length,
              'database ' + dbName + ' not found in ' + tojson(listDatabases()));
    const dbCollections = testDB.getCollectionNames();
    assert.sameMembers(dbCollections, collNames);
}

jsTest.log('dropDatabase cleans data and metadata about itself and its child collections');
for (let i = 0; i < collNames.length; i++) {
    for (let j = 0; j < collNames.length; j++) {
        const collName = collNames[(i + j) % collNames.length];
        assert.commandWorked(testDB[collName].insert({x: 1}));
    }

    assertNamespacesExist();
    assert.commandWorked(testDB.dropDatabase());
    assertNamespacesDoNotExist();
}

jsTest.log('dropDatabase is idempotent');
{
    assert.commandWorked(testDB.dropDatabase());
    assertNamespacesDoNotExist();
}
