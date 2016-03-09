// Tests tracing where a document was inserted
load('jstests/libs/trace_missing_docs.js');

(function() {
    'use strict';

    var testDocMissing = function(useReplicaSet) {
        var options = {
            rs: useReplicaSet,
            shardOptions: {master: "", oplogSize: 10},
            rsOptions: {nodes: 1, oplogSize: 10}
        };

        var st = new ShardingTest({shards: 2, mongos: 1, other: options});

        var mongos = st.s0;
        var coll = mongos.getCollection("foo.bar");
        var admin = mongos.getDB("admin");
        var shards = mongos.getCollection("config.shards").find().toArray();

        assert.commandWorked(admin.runCommand({enableSharding: coll.getDB() + ""}));
        st.ensurePrimaryShard(coll.getDB() + "", shards[0]._id);

        coll.ensureIndex({sk: 1});
        assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {sk: 1}}));

        assert.writeOK(coll.insert({_id: 12345, sk: 67890, hello: "world"}));
        assert.writeOK(coll.update({_id: 12345}, {$set: {baz: 'biz'}}));
        assert.writeOK(coll.update({sk: 67890}, {$set: {baz: 'boz'}}));

        assert.commandWorked(admin.runCommand(
            {moveChunk: coll + "", find: {sk: 0}, to: shards[1]._id, _waitForDelete: true}));

        st.printShardingStatus();

        var ops = traceMissingDoc(coll, {_id: 12345, sk: 67890});

        assert.eq(ops[0].op, 'i');
        assert.eq(ops.length, 5);

        jsTest.log("DONE! " + (useReplicaSet ? "(using rs)" : "(using master/slave)"));

        st.stop();
    };

    testDocMissing(true);
    testDocMissing(false);

})();
