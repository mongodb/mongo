
/**
 * Test shard targeting for queries on a collection with a non-simple collation and a hashed shard
 * key.
 * @tags: [
 *   requires_find_command
 * ]
 */
(function() {
const st = new ShardingTest({mongos: 1, config: 1, shards: 2, rs: {nodes: 1}});

function shardCollectionWithSplitsAndMoves(
    ns, shardKeyPattern, collation, splitPoints, chunksToMove) {
    const collection = st.s.getCollection(ns);
    const db = collection.getDB();

    assert.commandWorked(db.runCommand({create: collection.getName(), collation: collation}));

    st.ensurePrimaryShard(db.getName(), st.shard0.shardName);
    assert.commandWorked(st.s.adminCommand({enableSharding: db.getName()}));

    assert.commandWorked(st.s.adminCommand({
        shardCollection: collection.getFullName(),
        key: shardKeyPattern,
        collation: {locale: "simple"}
    }));

    for (let splitPoint of splitPoints) {
        assert.commandWorked(
            st.s.adminCommand({split: collection.getFullName(), middle: splitPoint}));
    }

    for (let {query, shard} of chunksToMove) {
        assert.commandWorked(st.s.adminCommand({
            moveChunk: collection.getFullName(),
            find: query,
            to: shard,
        }));
    }

    return collection;
}

function findQueryWithCollation(collection, query, collation) {
    let cursor = collection.find(query);
    if (collation) {
        cursor = cursor.collation(collation);
    }
    return cursor.toArray();
}

{
    jsTestLog(
        "Test find command in an _id:hashed sharded collection with simple default collation.");

    const collection = shardCollectionWithSplitsAndMoves(
        "test.id_hashed_sharding_with_simple_collation",
        {_id: "hashed"},
        {locale: "simple"},
        [{_id: convertShardKeyToHashed("A")}, {_id: convertShardKeyToHashed("a")}],
        [
            {query: {_id: "A"}, shard: st.shard0.shardName},
            {query: {_id: "a"}, shard: st.shard1.shardName}
        ]);

    const docs = [{_id: "A"}];
    assert.commandWorked(collection.insert(docs));

    // Check default collation, simple collation, non-simple collation.
    assert.eq([],
              findQueryWithCollation(
                  st.s.getCollection(collection.getFullName()), {_id: "a"}, undefined));
    assert.eq([],
              findQueryWithCollation(
                  st.s.getCollection(collection.getFullName()), {_id: "a"}, {locale: "simple"}));
    assert.eq(
        docs,
        findQueryWithCollation(
            st.s.getCollection(collection.getFullName()), {_id: "a"}, {locale: "en", strength: 2}));
}

{
    jsTestLog(
        "Test find command in an _id:hashed sharded collection with non-simple default collation.");

    const collection = shardCollectionWithSplitsAndMoves(
        "test.id_hashed_sharding_with_default_collation",
        {_id: "hashed"},
        {locale: "en", strength: 2},
        [{_id: convertShardKeyToHashed("A")}, {_id: convertShardKeyToHashed("a")}],
        [
            {query: {_id: "A"}, shard: st.shard0.shardName},
            {query: {_id: "a"}, shard: st.shard1.shardName}
        ]);

    const docs = [{_id: "A"}];
    assert.commandWorked(collection.insert(docs));

    // Check default collation, simple collation, non-simple collation.
    assert.eq(docs,
              findQueryWithCollation(
                  st.s.getCollection(collection.getFullName()), {_id: "a"}, undefined));
    assert.eq([],
              findQueryWithCollation(
                  st.s.getCollection(collection.getFullName()), {_id: "a"}, {locale: "simple"}));
    assert.eq(
        docs,
        findQueryWithCollation(
            st.s.getCollection(collection.getFullName()), {_id: "a"}, {locale: "en", strength: 2}));
}

{
    jsTestLog("Test an _id:1 sharded collection with non-simple default collation.");

    const collection = st.s.getCollection("test.id_range_sharding_with_default_collation");
    const db = collection.getDB();
    assert.commandWorked(
        db.runCommand({create: collection.getName(), collation: {locale: "en", strength: 2}}));

    st.ensurePrimaryShard(db.getName(), st.shard0.shardName);
    assert.commandWorked(st.s.adminCommand({enableSharding: db.getName()}));

    const res = assert.commandFailedWithCode(st.s.adminCommand({
        shardCollection: collection.getFullName(),
        key: {_id: 1},
        collation: {locale: "simple"}
    }),
                                             ErrorCodes.BadValue);
    assert(/The _id index must have the same collation as the collection/.test(res.errmsg),
           `expected shardCollection command to fail due to required collation for _id index: ${
               tojson(res)}`);
}

{
    jsTestLog("Test find command in a hashed sharded collection with simple default collation.");
    const collection = shardCollectionWithSplitsAndMoves(
        "test.non_id_hashed_sharding_with_simple_collation",
        {notUnderscoreId: "hashed"},
        {locale: "simple"},
        [
            {notUnderscoreId: convertShardKeyToHashed("A")},
            {notUnderscoreId: convertShardKeyToHashed("a")}
        ],
        [
            {query: {notUnderscoreId: "A"}, shard: st.shard0.shardName},
            {query: {notUnderscoreId: "a"}, shard: st.shard1.shardName}
        ]);

    const docs = [{_id: 0, notUnderscoreId: "A"}];
    assert.commandWorked(collection.insert(docs));

    // Check default collation, simple collation, non-simple collation.
    assert.eq([],
              findQueryWithCollation(
                  st.s.getCollection(collection.getFullName()), {notUnderscoreId: "a"}, undefined));
    assert.eq([],
              findQueryWithCollation(st.s.getCollection(collection.getFullName()),
                                     {notUnderscoreId: "a"},
                                     {locale: "simple"}));
    assert.eq(docs,
              findQueryWithCollation(st.s.getCollection(collection.getFullName()),
                                     {notUnderscoreId: "a"},
                                     {locale: "en", strength: 2}));
}

{
    jsTestLog(
        "Test find command in a hashed sharded collection with non-simple default collation.");

    const collection = shardCollectionWithSplitsAndMoves(
        "test.non_id_hashed_sharding_with_non_simple_collation",
        {notUnderscoreId: "hashed"},
        {locale: "en", strength: 2},
        [
            {notUnderscoreId: convertShardKeyToHashed("A")},
            {notUnderscoreId: convertShardKeyToHashed("a")}
        ],
        [
            {query: {notUnderscoreId: "A"}, shard: st.shard0.shardName},
            {query: {notUnderscoreId: "a"}, shard: st.shard1.shardName}
        ]);

    const docs = [{_id: 0, notUnderscoreId: "A"}];
    assert.commandWorked(collection.insert(docs));

    // Check default collation, simple collation, non-simple collation.
    assert.eq(docs,
              findQueryWithCollation(
                  st.s.getCollection(collection.getFullName()), {notUnderscoreId: "a"}, undefined));
    assert.eq([],
              findQueryWithCollation(st.s.getCollection(collection.getFullName()),
                                     {notUnderscoreId: "a"},
                                     {locale: "simple"}));
    assert.eq(docs,
              findQueryWithCollation(st.s.getCollection(collection.getFullName()),
                                     {notUnderscoreId: "a"},
                                     {locale: "en", strength: 2}));
}

{
    jsTestLog(
        "Test findAndModify command in an _id:hashed sharded collection with simple default collation.");

    const collection = shardCollectionWithSplitsAndMoves(
        "test.id_hashed_sharding_find_and_modify_simple_collation",
        {_id: "hashed"},
        {locale: "simple"},
        [{_id: convertShardKeyToHashed("A")}, {_id: convertShardKeyToHashed("a")}],
        [
            {query: {_id: "A"}, shard: st.shard0.shardName},
            {query: {_id: "a"}, shard: st.shard1.shardName}
        ]);

    const docs = [{_id: "A", count: 0}];
    assert.commandWorked(collection.insert(docs));

    const mongosCollection = st.s.getCollection(collection.getFullName());

    // Check findAndModify results with the default, simple, and non-simple collation. Currently,
    // due to findAndModify's assumption that _id is uniquely targetable, we do not do a scatter
    // gather to check every shard for a match. findAndModify's current behavior will target the
    // first shard in which the max key of a chunk is greater than the query's shard key. In this
    // case, because we're using hashed sharding, hash('a') is less than hash('A'), which means when
    // we query for {_id: "a"} we will target the shard containing the chunk for "a", likewise if we
    // query for {_id: "A"} we will only target the shard containing the chunk for "A".
    assert.lt(convertShardKeyToHashed("a"), convertShardKeyToHashed("A"));
    assert.eq(null,
              mongosCollection.findAndModify({query: {_id: "a"}, update: {$inc: {count: 1}}}));
    assert.eq(null,
              mongosCollection.findAndModify(
                  {query: {_id: "a"}, update: {$inc: {count: 1}}, collation: {locale: "simple"}}));
    assert.eq(null, mongosCollection.findAndModify({
        query: {_id: "a"},
        update: {$inc: {count: 1}},
        collation: {locale: "en", strength: 2}
    }));
    assert.eq({_id: "A", count: 0},
              mongosCollection.findAndModify({query: {_id: "A"}, update: {$inc: {count: 1}}}));
    assert.eq({_id: "A", count: 1},
              mongosCollection.findAndModify(
                  {query: {_id: "A"}, update: {$inc: {count: 1}}, collation: {locale: "simple"}}));
    assert.eq({_id: "A", count: 2}, mongosCollection.findAndModify({
        query: {_id: "A"},
        update: {$inc: {count: 1}},
        collation: {locale: "en", strength: 2}
    }));
}

{
    jsTestLog(
        "Test findAndModify command in an _id:hashed sharded collection with non-simple default collation.");

    const collection = shardCollectionWithSplitsAndMoves(
        "test.id_hashed_sharding_find_and_modify_with_non_simple_collation",
        {_id: "hashed"},
        {locale: "en", strength: 2},
        [{_id: convertShardKeyToHashed("A")}, {_id: convertShardKeyToHashed("a")}],
        [
            {query: {_id: "A"}, shard: st.shard0.shardName},
            {query: {_id: "a"}, shard: st.shard1.shardName}
        ]);

    const docs = [{_id: "A", count: 0}];
    assert.commandWorked(collection.insert(docs));

    const mongosCollection = st.s.getCollection(collection.getFullName());

    // Check findAndModify results with the default, simple, and non-simple collation. Currently,
    // due to findAndModify's assumption that _id is uniquely targetable, we do not do a scatter
    // gather to check every shard for a match. findAndModify's current behavior will target the
    // first shard in which the max key of a chunk is greater than the query's shard key. In this
    // case, because we're using hashed sharding, hash('a') is less than hash('A'), which means when
    // we query for {_id: "a"} we will target the shard containing the chunk for "a", likewise if we
    // query for {_id: "A"} we will only target the shard containing the chunk for "A".
    assert.lt(convertShardKeyToHashed("a"), convertShardKeyToHashed("A"));
    assert.eq(null,
              mongosCollection.findAndModify({query: {_id: "a"}, update: {$inc: {count: 1}}}));
    assert.eq(null,
              mongosCollection.findAndModify(
                  {query: {_id: "a"}, update: {$inc: {count: 1}}, collation: {locale: "simple"}}));
    assert.eq(null, mongosCollection.findAndModify({
        query: {_id: "a"},
        update: {$inc: {count: 1}},
        collation: {locale: "en", strength: 2}
    }));
    assert.eq({_id: "A", count: 0},
              mongosCollection.findAndModify({query: {_id: "A"}, update: {$inc: {count: 1}}}));
    assert.eq({_id: "A", count: 1},
              mongosCollection.findAndModify(
                  {query: {_id: "A"}, update: {$inc: {count: 1}}, collation: {locale: "simple"}}));
    assert.eq({_id: "A", count: 2}, mongosCollection.findAndModify({
        query: {_id: "A"},
        update: {$inc: {count: 1}},
        collation: {locale: "en", strength: 2}
    }));
}

{
    jsTestLog(
        "Test findAndModify command in a hashed sharded collection with simple default collation.");
    const collection = shardCollectionWithSplitsAndMoves(
        "test.non_id_hashed_sharding_find_and_modify_with_simple_collation",
        {notUnderscoreId: "hashed"},
        {locale: "simple"},
        [
            {notUnderscoreId: convertShardKeyToHashed("A")},
            {notUnderscoreId: convertShardKeyToHashed("a")}
        ],
        [
            {query: {notUnderscoreId: "A"}, shard: st.shard0.shardName},
            {query: {notUnderscoreId: "a"}, shard: st.shard1.shardName}
        ]);

    const docs = [{_id: 0, notUnderscoreId: "A", count: 0}];
    assert.commandWorked(collection.insert(docs));

    const mongosCollection = st.s.getCollection(collection.getFullName());

    // Check findAndModify results with the default, simple, and non-simple collation. Currently,
    // due to findAndModify's assumption that _id is uniquely targetable, we do not do a scatter
    // gather to check every shard for a match. findAndModify's current behavior will target the
    // first shard in which the max key of a chunk is greater than the query's shard key. In this
    // case, because we're using hashed sharding, hash('a') is less than hash('A'), which means when
    // we query for {notUnderscoeId: "a"} we will target the shard containing the chunk for "a",
    // likewise if we query for {notUnderscoreId: "A"} we will only target the shard containing the
    // chunk for "A".
    assert.lt(convertShardKeyToHashed("a"), convertShardKeyToHashed("A"));
    assert.eq(null,
              mongosCollection.findAndModify(
                  {query: {notUnderscoreId: "a"}, update: {$inc: {count: 1}}}));
    assert.eq(null, mongosCollection.findAndModify({
        query: {notUnderscoreId: "a"},
        update: {$inc: {count: 1}},
        collation: {locale: "simple"}
    }));
    assert.eq(null, mongosCollection.findAndModify({
        query: {notUnderscoreId: "a"},
        update: {$inc: {count: 1}},
        collation: {locale: "en", strength: 2}
    }));
    assert.eq({_id: 0, notUnderscoreId: "A", count: 0},
              mongosCollection.findAndModify(
                  {query: {notUnderscoreId: "A"}, update: {$inc: {count: 1}}}));
    assert.eq({_id: 0, notUnderscoreId: "A", count: 1}, mongosCollection.findAndModify({
        query: {notUnderscoreId: "A"},
        update: {$inc: {count: 1}},
        collation: {locale: "simple"}
    }));
    assert.eq({_id: 0, notUnderscoreId: "A", count: 2}, mongosCollection.findAndModify({
        query: {notUnderscoreId: "A"},
        update: {$inc: {count: 1}},
        collation: {locale: "en", strength: 2}
    }));
}

{
    jsTestLog(
        "Test findAndModify command in a hashed sharded collection with non-simple default collation.");

    const collection = shardCollectionWithSplitsAndMoves(
        "test.non_id_hashed_sharding_find_and_modify_with_non_simple_collation",
        {notUnderscoreId: "hashed"},
        {locale: "en", strength: 2},
        [
            {notUnderscoreId: convertShardKeyToHashed("A")},
            {notUnderscoreId: convertShardKeyToHashed("a")}
        ],
        [
            {query: {notUnderscoreId: "A"}, shard: st.shard0.shardName},
            {query: {notUnderscoreId: "a"}, shard: st.shard1.shardName}
        ]);

    const docs = [{_id: 0, notUnderscoreId: "A", count: 0}];
    assert.commandWorked(collection.insert(docs));

    const mongosCollection = st.s.getCollection(collection.getFullName());

    // Check findAndModify results with the default, simple, and non-simple collation. Currently,
    // due to findAndModify's assumption that _id is uniquely targetable, we do not do a scatter
    // gather to check every shard for a match. findAndModify's current behavior will target the
    // first shard in which the max key of a chunk is greater than the query's shard key. In this
    // case, because we're using hashed sharding, hash('a') is less than hash('A'), which means when
    // we query for {notUnderscoreId: "a"} we will target the shard containing the chunk for "a",
    // likewise if we query for {notUnderscoreId: "A"} we will only target the shard containing the
    // chunk for "A".
    assert.lt(convertShardKeyToHashed("a"), convertShardKeyToHashed("A"));
    assert.eq(null,
              mongosCollection.findAndModify(
                  {query: {notUnderscoreId: "a"}, update: {$inc: {count: 1}}}));
    assert.eq(null, mongosCollection.findAndModify({
        query: {notUnderscoreId: "a"},
        update: {$inc: {count: 1}},
        collation: {locale: "simple"}
    }));
    assert.eq(null, mongosCollection.findAndModify({
        query: {notUnderscoreId: "a"},
        update: {$inc: {count: 1}},
        collation: {locale: "en", strength: 2}
    }));
    assert.eq({_id: 0, notUnderscoreId: "A", count: 0},
              mongosCollection.findAndModify(
                  {query: {notUnderscoreId: "A"}, update: {$inc: {count: 1}}}));
    assert.eq({_id: 0, notUnderscoreId: "A", count: 1}, mongosCollection.findAndModify({
        query: {notUnderscoreId: "A"},
        update: {$inc: {count: 1}},
        collation: {locale: "simple"}
    }));
    assert.eq({_id: 0, notUnderscoreId: "A", count: 2}, mongosCollection.findAndModify({
        query: {notUnderscoreId: "A"},
        update: {$inc: {count: 1}},
        collation: {locale: "en", strength: 2}
    }));
}

st.stop();
})();