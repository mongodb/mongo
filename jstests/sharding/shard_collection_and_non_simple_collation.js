/**
 * Tests the interaction between shard keys and indexes with simple vs non-simple collation.
 *
 * @tags: [
 *  multiversion_incompatible,
 *  requires_sharding,
 * ]
 */
const st = new ShardingTest({shards: 1});
const db = st.s.getDB("test");
const collName = "coll";
const collName2 = "coll2";
const shardKey = {
    a: 1
};

{
    jsTestLog(
        "creates a collection implicitly when creating a unique index with simple collation and shards the collection successfully");
    const coll = db[collName];
    const coll2 = db[collName2];

    // Create unique index with simple collation (default).
    assert.commandWorked(coll.createIndex(shardKey, {unique: true, collation: {locale: "simple"}}));

    // Shard the collection - should succeed because the index has simple collation.
    assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: shardKey}));

    // Shard another collection with explicit simple collation in the shardCollection command.
    assert.commandWorked(
        st.s.adminCommand({
            shardCollection: coll2.getFullName(),
            key: shardKey,
            unique: true,
            collation: {locale: "simple"},
        }),
    );
    db.dropDatabase();
}

{
    jsTestLog(
        "creates a collection implicitly when creating a unique index with non-simple collation and fails to shard the collection");
    const coll = db[collName];

    // Create unique index with non-simple collation.
    assert.commandWorked(
        coll.createIndex(
            shardKey, {unique: true, collation: {locale: "en_US", strength: 2}, name: "a_1_enUS"}),
    );

    // Attempt to shard the collection - should fail because the index has non-simple
    // collation.
    assert.commandFailedWithCode(
        st.s.adminCommand({shardCollection: coll.getFullName(), key: shardKey}),
        ErrorCodes.InvalidOptions,
    );
    db.dropDatabase();
}

{
    jsTestLog(
        "shards a collection and successfully creates a non-simple collation index with the same key format");
    const coll = db[collName];

    // Shard the collection first.
    assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: shardKey}));

    // Create a non-unique index with non-simple collation - should succeed.
    assert.commandWorked(
        coll.createIndex(shardKey, {collation: {locale: "en_US", strength: 2}, name: "a_1_enUS"}));
    db.dropDatabase();
}

{
    jsTestLog(
        "shards a collection and fails to create a unique non-simple collation index with the same key format");
    const coll = db[collName];

    // Shard the collection first.
    assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: shardKey}));

    // Attempt to create a unique index with non-simple collation - should fail.
    assert.commandFailedWithCode(
        coll.createIndex(
            shardKey, {unique: true, collation: {locale: "en_US", strength: 2}, name: "a_1_enUS"}),
        ErrorCodes.CannotCreateIndex,
    );
    db.dropDatabase();
}

{
    jsTestLog(
        "shards a collection and fails to use collMod to change prepareUnique with non-simple collation");
    const coll = db[collName];

    // Shard the collection.
    assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: shardKey}));

    assert.commandWorked(
        coll.createIndex(shardKey, {collation: {locale: "en_US", strength: 2}, name: "a_1_enUS"}));

    // Attempt to use collMod to change prepareUnique and collation to non-simple - should
    // fail.
    assert.commandFailedWithCode(
        db.runCommand({
            collMod: collName,
            index: {keyPattern: shardKey, name: "a_1_enUS", prepareUnique: true},
        }),
        ErrorCodes.InvalidOptions,
    );
}

st.stop();
