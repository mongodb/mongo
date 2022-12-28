/**
 * Test dropDatabase command in a sharded cluster.
 */

(function() {
"use strict";

var st = new ShardingTest({shards: 2});

var configDB = st.s.getDB("config");
const dbNamePrefix = "testDropDB";
var dbCounter = 0;

function getNewDb() {
    return st.s.getDB(dbNamePrefix + "_" + dbCounter++);
}

function getPrefixRegExp(prefix) {
    return RegExp("^" + RegExp.escape(prefix) + ".*");
}

function getDbPrefixRegExp(dbName) {
    return getPrefixRegExp(dbName + ".");
}

function listDatabases(options) {
    return assert.commandWorked(st.s.adminCommand(Object.assign({listDatabases: 1}, options)))
        .databases;
}

function assertDatabaseExists(dbName) {
    // Check that listDatabase return the db
    // TODO SERVER-54377 listDatabases doesn't show shareded DBs without collections
    assert.gte(1, listDatabases({nameOnly: true, filter: {name: dbName}}).length);
    // Database entry exists
    assert.eq(1, configDB.databases.countDocuments({_id: dbName}));
}

function assertDatabaseDropped(dbName) {
    // Check that listDatabase doesn't return the db
    assert.eq(0, listDatabases({nameOnly: true, filter: {name: dbName}}).length);
    // No more database entry
    assert.eq(0, configDB.databases.countDocuments({_id: dbName}));
    // No more tags for this database
    assert.eq(0, configDB.tags.countDocuments({ns: getDbPrefixRegExp(dbName)}));

    // Check dropped collections.
    assert.eq(0, configDB.collections.countDocuments({_id: getDbPrefixRegExp(dbName)}));
}

jsTest.log("Test that dropping admin/config DB is illegal");
{
    assert.commandFailedWithCode(st.s.getDB('admin').dropDatabase(), ErrorCodes.IllegalOperation);
    assert.commandFailedWithCode(st.s.getDB('config').dropDatabase(), ErrorCodes.IllegalOperation);
}

jsTest.log("Test dropping unexistent database");
{
    const db = getNewDb();
    // Dropping a database that doesn't exist will result in an info field in the response.
    const res = assert.commandWorked(db.dropDatabase());
    assertDatabaseDropped(db.getName());
}

jsTest.log("Test dropping unsharded database");
{
    const db = getNewDb();
    // Create the database
    assert.commandWorked(db.foo.insert({}));
    assertDatabaseExists(db.getName());
    // Drop the database
    assert.commandWorked(db.dropDatabase());
    assertDatabaseDropped(db.getName());

    // Test drop database idempotency
    assert.commandWorked(db.dropDatabase());
    assertDatabaseDropped(db.getName());
}

jsTest.log("Test dropping unsharded database with multiple collections");
{
    const db = getNewDb();
    // Create 3 unsharded collection
    const colls = Array.from({length: 3}, (_, i) => db['unshardedColl_' + i]);
    colls.forEach(coll => coll.insert({}));

    // Create the database
    assertDatabaseExists(db.getName());
    // Drop the database
    assert.commandWorked(db.dropDatabase());
    assertDatabaseDropped(db.getName());
}

jsTest.log("Test dropping sharded database");
{
    const db = getNewDb();
    // Create the database
    st.s.adminCommand({enableSharding: db.getName()});
    assertDatabaseExists(db.getName());
    // Drop the database
    assert.commandWorked(db.dropDatabase());
    assertDatabaseDropped(db.getName());

    // Test drop database idempotency
    assert.commandWorked(db.dropDatabase());
    assertDatabaseDropped(db.getName());
}

jsTest.log("Test dropping database that contains regex characters");
// Original bugs [SERVER-4954, SERVER-4955]
{
    const db = st.s.getDB("onlysmallcaseletters");
    assert.commandWorked(
        st.s.adminCommand({enablesharding: db.getName(), primaryShard: st.shard1.shardName}));
    st.shardColl(db['data'], {num: 1});
    assertDatabaseExists(db.getName());

    const specialDB = st.s.getDB("[a-z]+");
    const specialColl = db['special'];
    assert.commandWorked(st.s.adminCommand(
        {enablesharding: specialDB.getName(), primaryShard: st.shard0.shardName}));
    assertDatabaseExists(specialDB.getName());
    st.shardColl(specialColl, {num: 1});
    assert(specialColl.exists());

    // Drop special database
    assert.commandWorked(specialDB.dropDatabase());
    assertDatabaseDropped(specialDB.getName());

    // The normal database still exists
    assertDatabaseExists(db.getName());

    // Drop also the normal database
    assert.commandWorked(db.dropDatabase());
    assertDatabaseDropped(db.getName());
}

jsTest.log("Test dropping sharded database with multiple collections");
{
    const db = getNewDb();
    const unshardedColls = Array.from({length: 3}, (_, i) => db['unshardedColl_' + i]);
    unshardedColls.forEach(coll => coll.insert({}));
    const shardedColls = Array.from({length: 3}, (_, i) => db['shardedColl_' + i]);
    shardedColls.forEach(coll => st.shardColl(coll, {_id: 1}));
    // Create the database
    st.s.adminCommand({enableSharding: db.getName()});
    assertDatabaseExists(db.getName());
    // Drop the database
    assert.commandWorked(db.dropDatabase());
    assertDatabaseDropped(db.getName());
}

jsTest.log(
    "Tests that dropping a database also removes the zones associated with the collections in the database.");
{
    const db = getNewDb();
    const coll = db['sharededColl'];
    const zoneName = 'zone';

    assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: zoneName}));
    st.shardColl(coll, {x: 1});
    assertDatabaseExists(db.getName());
    assert.commandWorked(st.s.adminCommand(
        {updateZoneKeyRange: coll.getFullName(), min: {x: 0}, max: {x: 10}, zone: zoneName}));

    assert.eq(1, configDB.tags.countDocuments({ns: getDbPrefixRegExp(db.getName())}));

    // Drop the database
    assert.commandWorked(db.dropDatabase());
    assertDatabaseDropped(db.getName());
}

{
    const db = getNewDb();
    const conn = st.rs0.getPrimary().getDB(db.getName());
    const fcvDoc = conn.adminCommand({getParameter: 1, featureCompatibilityVersion: 1});
    if (MongoRunner.compareBinVersions(fcvDoc.featureCompatibilityVersion.version, '6.3') >= 0) {
        jsTest.log(
            "Tests that dropping a database also removes all associated zones for nonexisting and " +
            "unsharded collections.");
        {
            const coll = db['shardedColl'];
            const unshardedColl = db['unshardedColl'];
            const zoneName = 'zone';

            st.shardColl(coll, {x: 1});
            assertDatabaseExists(db.getName());

            // Create an unsharded collection
            assert.commandWorked(unshardedColl.insert({x: 3}));

            // Append zones for a sharded collection, unsharded collection, a nonexisting collection
            // and a collection from another database.
            st.addShardTag(st.shard0.shardName, zoneName);
            assert.commandWorked(st.s.adminCommand({
                updateZoneKeyRange: coll.getFullName(),
                min: {x: 0},
                max: {x: 10},
                zone: zoneName
            }));
            assert.commandWorked(st.s.adminCommand({
                updateZoneKeyRange: unshardedColl.getFullName(),
                min: {x: 10},
                max: {x: 15},
                zone: zoneName
            }));
            assert.commandWorked(st.s.adminCommand({
                updateZoneKeyRange: db.getName() + '.nonexisting',
                min: {x: 15},
                max: {x: 20},
                zone: zoneName
            }));
            assert.commandWorked(st.s.adminCommand(
                {updateZoneKeyRange: 'otherDb.coll', min: {x: 20}, max: {x: 25}, zone: zoneName}));

            // Assert that has been added some entries on 'config.tags'
            assert.eq(3, configDB.tags.countDocuments({ns: getDbPrefixRegExp(db.getName())}));

            // Drop the database
            assert.commandWorked(db.dropDatabase());
            assertDatabaseDropped(db.getName());

            // Assert that there are no zones left for database
            assert.eq(0, configDB.tags.countDocuments({ns: getDbPrefixRegExp(db.getName())}));

            // Assert that there is one zone from another database that has not been deleted.
            assert.eq(1, configDB.tags.countDocuments({ns: getDbPrefixRegExp('otherDb')}));
        }
    }
}

{
    const db = st.s.getDB("db[a-z]+");
    const conn = st.rs0.getPrimary().getDB(db.getName());
    const fcvDoc = conn.adminCommand({getParameter: 1, featureCompatibilityVersion: 1});
    if (MongoRunner.compareBinVersions(fcvDoc.featureCompatibilityVersion.version, '6.3') >= 0) {
        jsTest.log(
            "Tests that dropping a database with regex characters also removes all associated " +
            "zones for nonexisting and unsharded collections.");
        {
            const coll = db['shardedColl'];
            const unshardedColl = db['unshardedColl'];
            const zoneName = 'zone';

            st.shardColl(coll, {x: 1});
            assertDatabaseExists(db.getName());

            // Create an unsharded collection
            assert.commandWorked(unshardedColl.insert({x: 3}));

            // Append zones for a sharded collection, unsharded collection, a nonexisting collection
            // and a collection from another database.
            st.addShardTag(st.shard0.shardName, zoneName);
            assert.commandWorked(st.s.adminCommand({
                updateZoneKeyRange: coll.getFullName(),
                min: {x: 0},
                max: {x: 10},
                zone: zoneName
            }));
            assert.commandWorked(st.s.adminCommand({
                updateZoneKeyRange: unshardedColl.getFullName(),
                min: {x: 10},
                max: {x: 15},
                zone: zoneName
            }));
            assert.commandWorked(st.s.adminCommand({
                updateZoneKeyRange: db.getName() + '.nonexisting',
                min: {x: 15},
                max: {x: 20},
                zone: zoneName
            }));
            assert.commandWorked(st.s.adminCommand({
                updateZoneKeyRange: 'otherDbRegex.coll',
                min: {x: 20},
                max: {x: 25},
                zone: zoneName
            }));

            // Assert that has been added some entries on 'config.tags'
            assert.eq(3, configDB.tags.countDocuments({ns: getDbPrefixRegExp(db.getName())}));

            // Drop the database
            assert.commandWorked(db.dropDatabase());
            assertDatabaseDropped(db.getName());

            // Assert that there are no zones left for database
            assert.eq(0, configDB.tags.countDocuments({ns: getDbPrefixRegExp(db.getName())}));

            // Assert that there is one zone from another database that has not been deleted.
            assert.eq(1, configDB.tags.countDocuments({ns: getDbPrefixRegExp('otherDbRegex')}));
        }
    }
}

jsTest.log("Tests that dropping a database doesn't affects other database with the same prefix.");
// Original bug SERVER-3471
{
    // Create 3 DBs
    //  - SomePrefix
    //  - SomePrefix_A
    //  - SomePrefix_b
    const dbPrefix = dbNamePrefix + "Prefix";
    const databases =
        [st.s.getDB(dbPrefix), st.s.getDB(dbPrefix + '_A'), st.s.getDB(dbPrefix + '_B')];
    // Assert all database have the same prefix
    assert.containsPrefix(dbNamePrefix, databases.map(db => db.getName()));

    const collPrefix = "coll_";
    const numColls = 3;
    const numDocs = 3;

    // Create 3 colls with 3 documents on each database
    databases.forEach(db => {
        for (var collID = 0; collID < numColls; collID++) {
            var coll = db[collPrefix + collID];
            // Create 3 documents for each collection
            for (var docID = 0; docID < numDocs; docID++) {
                coll.insert({_id: docID});
            }
            // shard the collection
            st.shardColl(coll, {_id: 1});
        }
        assertDatabaseExists(db.getName());
    });

    // Insert a document to an unsharded collection and make sure that the document is there.
    const unshardedColl = databases[0]['unshardedColl'];
    assert.commandWorked(unshardedColl.insert({dummy: 1}));
    assert.eq(1, unshardedColl.countDocuments({dummy: 1}));

    // Drop the non-suffixed db
    assert.commandWorked(databases[0].dropDatabase());
    assertDatabaseDropped(databases[0].getName());
    assert.eq(0, unshardedColl.countDocuments({}));

    // Ensure that the others databases exists and still contains all the collections
    databases.slice(1).forEach(db => {
        assertDatabaseExists(db.getName());
        assert.eq(numColls,
                  configDB.collections.countDocuments({_id: getDbPrefixRegExp(db.getName())}));

        // Assert that all the collections still have all the documents
        for (var collID = 0; collID < numColls; collID++) {
            assert.eq(numDocs, db[collPrefix + collID].countDocuments({}));
        }
    });
}

jsTest.log(
    "Test that dropping a non-sharded database, relevant events are properly logged on CSRS");
{
    // Create a non-sharded database
    const db = getNewDb();
    assert.commandWorked(db.foo.insert({}));

    // Drop the database
    assert.commandWorked(db.dropDatabase());

    // Verify that the drop database start event has been logged
    const startLogCount =
        configDB.changelog.countDocuments({what: 'dropDatabase.start', ns: db.getName()});
    assert.gte(1, startLogCount, "dropDatabase start event not found in changelog");

    // Verify that the drop database end event has been logged
    const endLogCount = configDB.changelog.countDocuments({what: 'dropDatabase', ms: db.getName()});
    assert.gte(1, endLogCount, "dropDatabase end event not found in changelog");
}

jsTest.log("Test that dropping a sharded database, relevant events are properly logged on CSRS");
{
    // Create a sharded database
    const db = getNewDb();
    st.s.adminCommand({enableSharding: db.getName()});

    // Drop the database
    assert.commandWorked(db.dropDatabase());

    // Verify that the drop database start event has been logged
    const startLogCount =
        configDB.changelog.countDocuments({what: 'dropDatabase.start', ns: db.getName()});
    assert.gte(1, startLogCount, "dropDatabase start event not found in changelog");

    // Verify that the drop database end event has been logged
    const endLogCount = configDB.changelog.countDocuments({what: 'dropDatabase', ns: db.getName()});
    assert.gte(1, endLogCount, "dropDatabase end event not found in changelog");
}

st.stop();
})();
