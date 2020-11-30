/**
 * This file tests that commands that do writes accept a write concern in a sharded cluster. This
 * test defines various database commands and what they expect to be true before and after the fact.
 * It then runs the commands with an invalid writeConcern and a valid writeConcern and
 * ensures that they succeed and fail appropriately. This only tests functions that aren't run
 * on config servers.
 *
 * This test is labeled resource intensive because its total io_write is 58MB compared to a median
 * of 5MB across all sharding tests in wiredTiger.
 * @tags: [
 *   resource_intensive,
 * ]
 */
load('jstests/libs/write_concern_util.js');

(function() {
"use strict";
var st = new ShardingTest({
    // Set priority of secondaries to zero to prevent spurious elections.
    shards: {
        rs0: {
            nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
            settings: {chainingAllowed: false}
        },
        rs1: {
            nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
            settings: {chainingAllowed: false}
        }
    },
    configReplSetTestOptions: {settings: {chainingAllowed: false}},
    mongos: 1,
});

var mongos = st.s;
var dbName = "wc-test-shards";
var db = mongos.getDB(dbName);
var collName = 'leaves';
var coll = db[collName];

function dropTestDatabase() {
    db.runCommand({dropDatabase: 1});
    db.extra.insert({a: 1});
    coll = db[collName];
    st.ensurePrimaryShard(db.toString(), st.shard0.shardName);
    assert.eq(0, coll.find().itcount(), "test collection not empty");
    assert.eq(1, db.extra.find().itcount(), "extra collection should have 1 document");
}

var commands = [];

// Tests a runOnAllShardsCommand against a sharded collection.
commands.push({
    req: {createIndexes: collName, indexes: [{key: {'type': 1}, name: 'sharded_type_index'}]},
    setupFunc: function() {
        shardCollectionWithChunks(st, coll);
        coll.insert({type: 'oak', x: -3});
        coll.insert({type: 'maple', x: 23});
        assert.eq(coll.getIndexes().length, 2);
    },
    confirmFunc: function() {
        assert.eq(coll.getIndexes().length, 3);
    },
    admin: false,
    isExpectedToWriteOnWriteConcernFailure: true,
});

// Tests a runOnAllShardsCommand.
commands.push({
    req: {createIndexes: collName, indexes: [{key: {'type': 1}, name: 'type_index'}]},
    setupFunc: function() {
        coll.insert({type: 'oak'});
        st.ensurePrimaryShard(db.toString(), st.shard0.shardName);
        assert.eq(coll.getIndexes().length, 1);
    },
    confirmFunc: function() {
        assert.eq(coll.getIndexes().length, 2);
    },
    admin: false,
    isExpectedToWriteOnWriteConcernFailure: true,
});

// Tests a batched write command.
commands.push({
    req: {insert: collName, documents: [{x: -3, type: 'maple'}, {x: 23, type: 'maple'}]},
    setupFunc: function() {
        shardCollectionWithChunks(st, coll);
    },
    confirmFunc: function() {
        assert.eq(coll.count({type: 'maple'}), 2);
    },
    admin: false,
    isExpectedToWriteOnWriteConcernFailure: true,
});

commands.push({
    req: {renameCollection: "renameCollWC.leaves", to: 'renameCollWC.pine_needles'},
    setupFunc: function() {
        db = db.getSiblingDB("renameCollWC");
        // Ensure that database is created.
        db.leaves.insert({type: 'oak'});
        st.ensurePrimaryShard(db.toString(), st.shard0.shardName);
        db.leaves.drop();
        db.pine_needles.drop();
        db.leaves.insert({type: 'oak'});
        assert.eq(db.leaves.count(), 1);
        assert.eq(db.pine_needles.count(), 0);
    },
    confirmFunc: function() {
        assert.eq(db.leaves.count(), 0);
        assert.eq(db.pine_needles.count(), 1);
    },
    admin: true,
    isExpectedToWriteOnWriteConcernFailure: true,
});

commands.push({
    req: {
        update: collName,
        updates: [{
            q: {type: 'oak'},
            u: [{$set: {type: 'ginkgo'}}],
        }],
        writeConcern: {w: 'majority'}
    },
    setupFunc: function() {
        coll.insert({type: 'oak'});
        assert.eq(coll.count({type: 'ginkgo'}), 0);
        assert.eq(coll.count({type: 'oak'}), 1);
    },
    confirmFunc: function() {
        assert.eq(coll.count({type: 'ginkgo'}), 1);
        assert.eq(coll.count({type: 'oak'}), 0);
    },
    admin: false,
    isExpectedToWriteOnWriteConcernFailure: true,
});

commands.push({
    req: {
        findAndModify: collName,
        query: {type: 'oak'},
        update: {$set: {type: 'ginkgo'}},
        writeConcern: {w: 'majority'}
    },
    setupFunc: function() {
        coll.insert({type: 'oak'});
        assert.eq(coll.count({type: 'ginkgo'}), 0);
        assert.eq(coll.count({type: 'oak'}), 1);
    },
    confirmFunc: function() {
        assert.eq(coll.count({type: 'ginkgo'}), 1);
        assert.eq(coll.count({type: 'oak'}), 0);
    },
    admin: false,
    isExpectedToWriteOnWriteConcernFailure: true,
});

commands.push({
    req: {
        findAndModify: collName,
        query: {type: 'oak'},
        update: [{$set: {type: 'ginkgo'}}],
        writeConcern: {w: 'majority'}
    },
    setupFunc: function() {
        coll.insert({type: 'oak'});
        assert.eq(coll.count({type: 'ginkgo'}), 0);
        assert.eq(coll.count({type: 'oak'}), 1);
    },
    confirmFunc: function() {
        assert.eq(coll.count({type: 'ginkgo'}), 1);
        assert.eq(coll.count({type: 'oak'}), 0);
    },
    admin: false,
    isExpectedToWriteOnWriteConcernFailure: true,
});

// Aggregate run against an unsharded collection.
commands.push({
    req: {aggregate: collName, pipeline: [{$sort: {type: 1}}, {$out: "foo"}], cursor: {}},
    setupFunc: function() {
        coll.insert({_id: 1, x: 3, type: 'oak'});
        coll.insert({_id: 2, x: 13, type: 'maple'});
    },
    confirmFunc: function(writesExpected = true) {
        if (writesExpected) {
            assert.eq(db.foo.find({type: 'oak'}).itcount(), 1);
            assert.eq(db.foo.find({type: 'maple'}).itcount(), 1);
            db.foo.drop();
        } else {
            assert.eq(0, db.foo.find().itcount());
        }
    },
    admin: false,
    // Aggregation pipelines with a $out stage are expected not to write on write concern error.
    isExpectedToWriteOnWriteConcernFailure: false,
});

// Aggregate that only matches one shard.
commands.push({
    req: {
        aggregate: collName,
        pipeline: [{$match: {x: -3}}, {$match: {type: {$exists: 1}}}, {$out: "foo"}],
        cursor: {}
    },
    setupFunc: function() {
        shardCollectionWithChunks(st, coll);
        coll.insert({_id: 1, x: -3, type: 'oak'});
        coll.insert({_id: 2, x: -4, type: 'maple'});
    },
    confirmFunc: function(writesExpected = true) {
        if (writesExpected) {
            assert.eq(db.foo.find().itcount(), 1);
            assert.eq(db.foo.find({type: 'oak'}).itcount(), 1);
            assert.eq(db.foo.find({type: 'maple'}).itcount(), 0);
            db.foo.drop();
        } else {
            assert.eq(0, db.foo.find().itcount());
        }
    },
    admin: false,
    // Aggregation pipelines with a $out stage are expected not to write on write concern error.
    isExpectedToWriteOnWriteConcernFailure: false,
});

// Aggregate that must go to multiple shards.
commands.push({
    req: {
        aggregate: collName,
        pipeline: [{$match: {type: {$exists: 1}}}, {$sort: {type: 1}}, {$out: "foo"}],
        cursor: {}
    },
    setupFunc: function() {
        shardCollectionWithChunks(st, coll);
        coll.insert({_id: 1, x: -3, type: 'oak'});
        coll.insert({_id: 2, x: 23, type: 'maple'});
    },
    confirmFunc: function(writesExpected = true) {
        if (writesExpected) {
            assert.eq(db.foo.find().itcount(), 2);
            assert.eq(db.foo.find({type: 'oak'}).itcount(), 1);
            assert.eq(db.foo.find({type: 'maple'}).itcount(), 1);
            db.foo.drop();
        } else {
            assert.eq(0, db.foo.find().itcount());
        }
    },
    admin: false,
    // Aggregation pipelines with a $out stage are expected not to write on write concern error.
    isExpectedToWriteOnWriteConcernFailure: false,
});

const map = function() {
    if (!this.tags) {
        return;
    }
    this.tags.forEach(function(z) {
        emit(z, 1);
    });
};

const reduce = function(key, values) {
    var count = 0;
    values.forEach(function(v) {
        count = count + v;
    });
    return count;
};

// MapReduce on an unsharded collection.
commands.push({
    req: {mapReduce: collName, map: map, reduce: reduce, out: "foo"},
    setupFunc: function() {
        coll.insert({x: -3, tags: ["a", "b"]});
        coll.insert({x: -7, tags: ["b", "c"]});
        coll.insert({x: 23, tags: ["c", "a"]});
        coll.insert({x: 27, tags: ["b", "c"]});
    },
    confirmFunc: function(writesExpected = true) {
        if (writesExpected) {
            assert.eq(db.foo.findOne({_id: 'a'}).value, 2);
            assert.eq(db.foo.findOne({_id: 'b'}).value, 3);
            assert.eq(db.foo.findOne({_id: 'c'}).value, 3);
            db.foo.drop();
        } else {
            assert.eq(0, db.foo.find().itcount());
        }
    },
    admin: false,
    // MapReduce with a replace output action is not expected to write on write concern error.
    isExpectedToWriteOnWriteConcernFailure: false,
});

// MapReduce on an unsharded collection with an output to a sharded collection.
commands.push({
    req: {mapReduce: collName, map: map, reduce: reduce, out: {merge: "foo", sharded: true}},
    setupFunc: function() {
        db.adminCommand({enablesharding: db.toString()});
        assert.commandWorked(db.foo.createIndex({_id: "hashed"}));
        st.shardColl(db.foo, {_id: "hashed"}, false);
        coll.insert({x: -3, tags: ["a", "b"]});
        coll.insert({x: -7, tags: ["b", "c"]});
        coll.insert({x: 23, tags: ["c", "a"]});
        coll.insert({x: 27, tags: ["b", "c"]});
    },
    confirmFunc: function(writesExpected = true) {
        if (writesExpected) {
            assert.eq(db.foo.findOne({_id: 'a'}).value, 2);
            assert.eq(db.foo.findOne({_id: 'b'}).value, 3);
            assert.eq(db.foo.findOne({_id: 'c'}).value, 3);
            db.foo.drop();
        } else {
            assert.eq(0, db.foo.find().itcount());
        }
    },
    admin: false,
    isExpectedToWriteOnWriteConcernFailure: true,
});

// MapReduce on a sharded collection.
commands.push({
    req: {mapReduce: collName, map: map, reduce: reduce, out: "foo"},
    setupFunc: function() {
        shardCollectionWithChunks(st, coll);
        coll.insert({x: -3, tags: ["a", "b"]});
        coll.insert({x: -7, tags: ["b", "c"]});
        coll.insert({x: 23, tags: ["c", "a"]});
        coll.insert({x: 27, tags: ["b", "c"]});
    },
    confirmFunc: function(writesExpected = true) {
        if (writesExpected) {
            assert.eq(db.foo.findOne({_id: 'a'}).value, 2);
            assert.eq(db.foo.findOne({_id: 'b'}).value, 3);
            assert.eq(db.foo.findOne({_id: 'c'}).value, 3);
            db.foo.drop();
        } else {
            assert.eq(0, db.foo.find().itcount());
        }
    },
    admin: false,
    // MapReduce with a replace output action expected not to write on write concern error.
    isExpectedToWriteOnWriteConcernFailure: false,
});

// MapReduce from a sharded collection with merge to a sharded collection.
commands.push({
    req: {mapReduce: collName, map: map, reduce: reduce, out: {merge: "foo", sharded: true}},
    setupFunc: function() {
        shardCollectionWithChunks(st, coll);
        assert.commandWorked(db.foo.createIndex({_id: "hashed"}));
        st.shardColl(db.foo, {_id: "hashed"}, false);
        coll.insert({x: -3, tags: ["a", "b"]});
        coll.insert({x: -7, tags: ["b", "c"]});
        coll.insert({x: 23, tags: ["c", "a"]});
        coll.insert({x: 27, tags: ["b", "c"]});
    },
    confirmFunc: function(writesExpected = true) {
        if (writesExpected) {
            assert.eq(db.foo.findOne({_id: 'a'}).value, 2);
            assert.eq(db.foo.findOne({_id: 'b'}).value, 3);
            assert.eq(db.foo.findOne({_id: 'c'}).value, 3);
            db.foo.drop();
        } else {
            assert.eq(0, db.foo.find().itcount());
        }
    },
    admin: false,
    // When output action is "merge" writes to the output collection are expected to succeed,
    // regardless of writeConcern error.
    isExpectedToWriteOnWriteConcernFailure: true,
});

// MapReduce on an unsharded collection, output to different database.
commands.push({
    req: {mapReduce: collName, map: map, reduce: reduce, out: {replace: "foo", db: "other"}},
    setupFunc: function() {
        db.getSiblingDB("other").createCollection("foo");
        coll.insert({x: -3, tags: ["a", "b"]});
        coll.insert({x: -7, tags: ["b", "c"]});
        coll.insert({x: 23, tags: ["c", "a"]});
        coll.insert({x: 27, tags: ["b", "c"]});
    },
    confirmFunc: function(writesExpected = true) {
        const otherDB = db.getSiblingDB("other");
        if (writesExpected) {
            assert.eq(otherDB.foo.findOne({_id: 'a'}).value, 2);
            assert.eq(otherDB.foo.findOne({_id: 'b'}).value, 3);
            assert.eq(otherDB.foo.findOne({_id: 'c'}).value, 3);
            otherDB.foo.drop();
        } else {
            assert.eq(0, otherDB.foo.find().itcount());
        }
    },
    admin: false,
    // MapReduce with a replace output action is not expected to write on write concern error.
    isExpectedToWriteOnWriteConcernFailure: false,
});

// MapReduce on a sharded collection, output to a different database.
commands.push({
    req: {mapReduce: collName, map: map, reduce: reduce, out: {replace: "foo", db: "other"}},
    setupFunc: function() {
        db.getSiblingDB("other").createCollection("foo");
        shardCollectionWithChunks(st, coll);
        coll.insert({x: -3, tags: ["a", "b"]});
        coll.insert({x: -7, tags: ["b", "c"]});
        coll.insert({x: 23, tags: ["c", "a"]});
        coll.insert({x: 27, tags: ["b", "c"]});
    },
    confirmFunc: function(writesExpected = true) {
        const otherDB = db.getSiblingDB("other");
        if (writesExpected) {
            assert.eq(otherDB.foo.findOne({_id: 'a'}).value, 2);
            assert.eq(otherDB.foo.findOne({_id: 'b'}).value, 3);
            assert.eq(otherDB.foo.findOne({_id: 'c'}).value, 3);
            otherDB.foo.drop();
        } else {
            assert.eq(0, otherDB.foo.find().itcount());
        }
    },
    admin: false,
    // MapReduce with a replace output action expected not to write on write concern error.
    isExpectedToWriteOnWriteConcernFailure: false,
});

// MapReduce on an unsharded collection with merge to a sharded collection in a different db.
commands.push({
    req: {
        mapReduce: collName,
        map: map,
        reduce: reduce,
        out: {merge: "foo", db: "other", sharded: true}
    },
    setupFunc: function() {
        const otherDB = db.getSiblingDB("other");
        otherDB.createCollection("foo");
        otherDB.adminCommand({enablesharding: otherDB.toString()});
        assert.commandWorked(otherDB.foo.createIndex({_id: "hashed"}));
        st.shardColl(otherDB.foo, {_id: "hashed"}, false);
        coll.insert({x: -3, tags: ["a", "b"]});
        coll.insert({x: -7, tags: ["b", "c"]});
        coll.insert({x: 23, tags: ["c", "a"]});
        coll.insert({x: 27, tags: ["b", "c"]});
    },
    confirmFunc: function(writesExpected = true) {
        const otherDB = db.getSiblingDB("other");
        if (writesExpected) {
            assert.eq(otherDB.foo.findOne({_id: 'a'}).value, 2);
            assert.eq(otherDB.foo.findOne({_id: 'b'}).value, 3);
            assert.eq(otherDB.foo.findOne({_id: 'c'}).value, 3);
            otherDB.foo.drop();
        } else {
            assert.eq(0, otherDB.foo.find().itcount());
        }
    },
    admin: false,
    isExpectedToWriteOnWriteConcernFailure: true,
});

// MapReduce from a sharded collection with merge to a sharded collection in different db.
commands.push({
    req: {
        mapReduce: collName,
        map: map,
        reduce: reduce,
        out: {merge: "foo", db: "other", sharded: true}
    },
    setupFunc: function() {
        shardCollectionWithChunks(st, coll);
        const otherDB = db.getSiblingDB("other");
        otherDB.createCollection("foo");
        assert.commandWorked(otherDB.foo.createIndex({_id: "hashed"}));
        st.shardColl(otherDB.foo, {_id: "hashed"}, false);
        coll.insert({x: -3, tags: ["a", "b"]});
        coll.insert({x: -7, tags: ["b", "c"]});
        coll.insert({x: 23, tags: ["c", "a"]});
        coll.insert({x: 27, tags: ["b", "c"]});
    },
    confirmFunc: function(writesExpected = true) {
        const otherDB = db.getSiblingDB("other");
        if (writesExpected) {
            assert.eq(otherDB.foo.findOne({_id: 'a'}).value, 2);
            assert.eq(otherDB.foo.findOne({_id: 'b'}).value, 3);
            assert.eq(otherDB.foo.findOne({_id: 'c'}).value, 3);
            otherDB.foo.drop();
        } else {
            assert.eq(0, otherDB.foo.find().itcount());
        }
    },
    admin: false,
    // When output action is "merge" writes to the output collection are expected to succeed,
    // regardless of writeConcern error.
    isExpectedToWriteOnWriteConcernFailure: true,
});

function testValidWriteConcern(cmd) {
    cmd.req.writeConcern = {w: 'majority', wtimeout: 5 * 60 * 1000};
    jsTest.log("Testing " + tojson(cmd.req));

    dropTestDatabase();
    cmd.setupFunc();
    var res = runCommandCheckAdmin(db, cmd);
    assert.commandWorked(res);
    assert(!res.writeConcernError,
           'command on a full cluster had writeConcernError: ' + tojson(res));
    cmd.confirmFunc();
}

function testInvalidWriteConcern(cmd) {
    cmd.req.writeConcern = {w: 'invalid'};
    jsTest.log("Testing " + tojson(cmd.req));

    dropTestDatabase();
    cmd.setupFunc();
    var res = runCommandCheckAdmin(db, cmd);

    // When a cluster aggregate command with a write stage fails due to write concern error it will
    // return a regular error response with "ok: 0" rather than "ok: 1" with a `writeConcernError`
    // sub-object detailing the failure. This is motivated by the fact that the number of documents
    // written is bounded only by the number of documents produced by the pipeline, which could lead
    // a writeConcernError object to exceed maximum BSON size.
    if (cmd.req.aggregate !== undefined || cmd.req.mapReduce !== undefined) {
        assert.commandFailedWithCode(
            res, [ErrorCodes.WriteConcernFailed, ErrorCodes.UnknownReplWriteConcern]);

        cmd.confirmFunc(cmd.isExpectedToWriteOnWriteConcernFailure);
    } else if (cmd.req.renameCollection !== undefined &&
               jsTestOptions().mongosBinVersion != 'last-lts') {
        // The renameCollection spans multiple nodes and potentially performs writes to the config
        // server, so the user-specified write concern has no effect.
        assert.commandWorked(res);
        assert(!res.writeConcernError,
               'command on a full cluster had writeConcernError: ' + tojson(res));
        cmd.confirmFunc();

    } else {
        assert.commandWorkedIgnoringWriteConcernErrors(res);
        assertWriteConcernError(res, ErrorCodes.UnknownReplWriteConcern);
        const writesExpected = true;
        cmd.confirmFunc(writesExpected);
    }
}

commands.forEach(function(cmd) {
    testValidWriteConcern(cmd);
    testInvalidWriteConcern(cmd);
});

st.stop();
})();
