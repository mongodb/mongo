var testCRUDAndAgg = function(db) {
    assert.commandWorked(db.foo.insert({x: 1}));
    assert.commandWorked(db.foo.insert({x: -1}));
    assert.commandWorked(db.foo.update({x: 1}, {$set: {y: 1}}));
    assert.commandWorked(db.foo.update({x: -1}, {$set: {y: 1}}));
    var doc1 = db.foo.findOne({x: 1});
    assert.eq(1, doc1.y);
    var doc2 = db.foo.findOne({x: -1});
    assert.eq(1, doc2.y);

    assert.commandWorked(db.foo.remove({x: 1}, true));
    assert.commandWorked(db.foo.remove({x: -1}, true));
    assert.eq(null, db.foo.findOne());
};

var testDDLOps = function(st) {
    var shard0Name = st.shard0.shardName;
    var shard1Name = st.shard1.shardName;
    var db = st.s.getDB("sharded");
    var configDB = st.s.getDB("config");
    assert.commandWorked(db.foo.insert({x: 1}));

    // moveChunk
    var shard0NumChunks = configDB.chunks.find({shard: shard0Name}).toArray().length;
    var shard1NumChunks = configDB.chunks.find({shard: shard1Name}).toArray().length;

    assert.commandWorked(
        st.s.adminCommand({moveChunk: "sharded.foo", find: {x: 1}, to: shard0Name}));

    var newShard0NumChunks = configDB.chunks.find({shard: shard0Name}).toArray().length;
    var newShard1NumChunks = configDB.chunks.find({shard: shard1Name}).toArray().length;
    assert.eq(newShard0NumChunks, shard0NumChunks + 1);
    assert.eq(newShard1NumChunks, shard1NumChunks - 1);

    assert.commandWorked(
        st.s.adminCommand({moveChunk: "sharded.foo", find: {x: 1}, to: shard1Name}));

    // shardCollection
    assert.eq(null, configDB.collections.findOne({_id: "sharded.apple"}));
    assert.commandWorked(st.s.adminCommand({shardCollection: "sharded.apple", key: {_id: 1}}));
    assert.eq(1, configDB.collections.find({_id: "sharded.apple"}).toArray().length);

    // renameCollection
    assert.commandWorked(st.s.adminCommand(
        {renameCollection: "sharded.apple", to: "sharded.pear", dropTarget: true}));
    assert.eq(null, configDB.collections.findOne({_id: "sharded.apple"}));
    assert.eq(1, configDB.collections.find({_id: "sharded.pear"}).toArray().length);

    // drop a collection
    assert.commandWorked(db.runCommand({drop: "pear"}));
    assert.eq(null, configDB.collections.findOne({_id: "sharded.pear"}));

    // movePrimary
    assert(configDB.databases.findOne({_id: "sharded", primary: shard0Name}));

    assert.commandWorked(st.s.adminCommand({movePrimary: "sharded", to: shard1Name}));
    assert.eq(null, configDB.databases.findOne({_id: "sharded", primary: shard0Name}));
    assert(configDB.databases.findOne({_id: "sharded", primary: shard1Name}));

    assert.commandWorked(st.s.adminCommand({movePrimary: "sharded", to: shard0Name}));
    assert.eq(null, configDB.databases.findOne({_id: "sharded", primary: shard1Name}));
    assert(configDB.databases.findOne({_id: "sharded", primary: shard0Name}));

    assert.commandWorked(db.foo.remove({x: 1}, true));
    assert.eq(null, db.foo.findOne());
};
