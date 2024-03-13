/*
 * Test the rename command against unsplittable collections.
 * @tags: [
 *   assumes_balancer_off,
 *   # Needed to run createUnsplittableCollection
 *   featureFlagAuthoritativeShardCollection,
 * ]
 */

function checkRenameSucceeded(configDb, nssFrom, nssTo, expectedUuid, shard) {
    const collEntryFrom = configDb.collections.findOne({_id: nssFrom});
    assert(collEntryFrom === null) << tojson(collEntryFrom);

    const collEntryTo = configDb.collections.findOne({_id: nssTo});
    assert(collEntryTo !== null);
    assert.eq(collEntryTo._id, nssTo);
    assert.eq(collEntryTo.unsplittable, true);
    assert.eq(collEntryTo.key, {_id: 1});
    assert.eq(collEntryTo.uuid, expectedUuid);

    let chunks = configDb.chunks.find({uuid: expectedUuid}).toArray();
    assert.eq(chunks.length, 1);
    assert.eq(chunks[0].shard, shard);
}

function getUuid(configDb, nss) {
    return configDb.collections.findOne({_id: nss}).uuid;
}

/**
 * Launch a rename test. This function executes:
 *     1. Create FROM collection as an unsplittable collection on the given shard.
 *     2. If `collToShouldExist` is true, create TO collection as an unsplittable collection on the
 *        given shard.
 *     3. Rename FROM collection to `dbTo` + "." + `collNameTo` namespace.
 *     4. Check that rename has succeeded.
 */
function testRenameUnsplittableCollection(configDb,
                                          dbFrom,
                                          collNameFrom,
                                          dbTo,
                                          collNameTo,
                                          shardName,
                                          collToShouldExist = false,
                                          collToShardName = "") {
    const nssFrom = dbFrom.getName() + "." + collNameFrom;
    const nssTo = dbTo.getName() + "." + collNameTo;

    const dropTarget = (collToShouldExist ? true : false);

    // Print descriptive test message
    let msg = "Running test: rename collection `" + nssFrom + "` located on shard `" + shardName +
        "` to `" + nssTo + "` with dropTarget=`" + dropTarget + "`."
    if (collToShouldExist) {
        msg += " Target collection exists on shard `" + collToShardName + "`."
    }
    else {msg += " Target collection doesn't exist."} jsTestLog(msg);

    // Create collFrom collection
    assert.commandWorked(
        dbFrom.runCommand({createUnsplittableCollection: collNameFrom, dataShard: shardName}));
    const coll = dbFrom[collNameFrom];
    const uuidFrom = getUuid(configDb, nssFrom);

    // Create collTo collection if requested
    if (collToShouldExist) {
        assert.neq("", collToShardName);
        assert.commandWorked(dbTo.runCommand(
            {createUnsplittableCollection: collNameTo, dataShard: collToShardName}));
    }

    // Rename collection
    assert.commandWorked(dbFrom.adminCommand(
        {renameCollection: coll.getFullName(), to: nssTo, dropTarget: dropTarget}));

    const resUuid = getUuid(configDb, nssTo);
    if (dbFrom.getName() === dbTo.getName()) {
        assert.eq(uuidFrom, resUuid);
    } else {
        assert.neq(uuidFrom, resUuid);
    }

    // Check result
    checkRenameSucceeded(configDb, dbFrom + "." + collNameFrom, nssTo, resUuid, shardName);
}

// Test setup
const st = new ShardingTest({shards: 2});
const mongos = st.s;
const db = mongos.getDB(jsTestName());
const configDb = mongos.getDB("config");
const primaryShard = st.shard0.shardName;
const nonPrimaryShard = st.shard1.shardName;
const anotherDb = mongos.getDB("dbTo");

// Ensure the db primary is shard0. This will be expected later on.
st.s.adminCommand({enableSharding: db.getName(), primaryShard: primaryShard});
st.s.adminCommand({enableSharding: anotherDb.getName(), primaryShard: primaryShard});

// 1. Rename collection  test:located on the primary shard
testRenameUnsplittableCollection(configDb, db, "collFrom1", db, "collTo1", primaryShard);

// 2. Rename collection  test:located on the primary shard when target exists
testRenameUnsplittableCollection(configDb,
                                 db,
                                 "collFrom2",
                                 db,
                                 "collTo2",
                                 primaryShard,
                                 true /*collToShouldExist*/,
                                 primaryShard);

// 3. Rename collection  test:not located on the primary shard
testRenameUnsplittableCollection(configDb, db, "collFrom3", db, "collTo3", nonPrimaryShard)

// 4. Rename collection  test:not located on the primary shard when target exists
testRenameUnsplittableCollection(configDb,
                                 db,
                                 "collFrom4",
                                 db,
                                 "collTo4",
                                 nonPrimaryShard,
                                 true /*collToShouldExist*/,
                                 nonPrimaryShard);

// 5. Rename collection  test:when target exists on another shard
testRenameUnsplittableCollection(configDb,
                                 db,
                                 "collFrom5",
                                 db,
                                 "collTo5",
                                 nonPrimaryShard,
                                 true /*collToShouldExist*/,
                                 primaryShard);

// 6. Rename collection  test:located on the primary shard across DBs
testRenameUnsplittableCollection(configDb, db, "collFrom6", anotherDb, "collTo6", primaryShard);

// 7. Rename collection  test:not located on the primary shard across DBs
testRenameUnsplittableCollection(configDb, db, "collFrom7", anotherDb, "collTo7", nonPrimaryShard);

// 8. Rename collection  test:located on the primary shard across DBs when target exists
testRenameUnsplittableCollection(configDb,
                                 db,
                                 "collFrom8",
                                 anotherDb,
                                 "collTo8",
                                 primaryShard,
                                 true /*collToShouldExist*/,
                                 primaryShard);

// 9. Rename collection  test:not located on the primary shard across DBs when target exists
testRenameUnsplittableCollection(configDb,
                                 db,
                                 "collFrom9",
                                 anotherDb,
                                 "collTo9",
                                 nonPrimaryShard,
                                 true /*collToShouldExist*/,
                                 nonPrimaryShard);

// 10. Rename collection  test:not located on the primary shard across DBs when target exists
testRenameUnsplittableCollection(configDb,
                                 db,
                                 "collFrom10",
                                 anotherDb,
                                 "collTo10",
                                 primaryShard,
                                 true /*collToShouldExist*/,
                                 nonPrimaryShard);
st.stop();
