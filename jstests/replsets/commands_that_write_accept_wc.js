load('jstests/libs/write_concern_util.js');

/**
 * This file tests that commands that do writes accept a write concern. This file does not test
 * mongos commands or user management commands, both of which are tested separately. This test
 * defines various database commands and what they expect to be true before and after the fact.
 * It then runs the commands with an invalid writeConcern and a valid writeConcern and
 * ensures that they succeed and fail appropriately.
 */

(function() {
    "use strict";
    var replTest = new ReplSetTest({name: 'WCSet', nodes: 3, settings: {chainingAllowed: false}});
    replTest.startSet();
    replTest.initiate();

    var master = replTest.getPrimary();
    var dbName = "wc-test";
    var db = master.getDB(dbName);
    var collName = 'leaves';
    var coll = db[collName];

    function dropTestCollection() {
        coll.drop();
        assert.eq(0, coll.find().itcount(), "test collection not empty");
    }

    dropTestCollection();

    var commands = [];

    commands.push({
        req: {insert: collName, documents: [{type: 'maple'}]},
        setupFunc: function() {},
        confirmFunc: function() {
            assert.eq(coll.count({type: 'maple'}), 1);
        }
    });

    commands.push({
        req: {createIndexes: collName, indexes: [{key: {'type': 1}, name: 'type_index'}]},
        setupFunc: function() {
            coll.insert({type: 'oak'});
            assert.eq(coll.getIndexes().length, 1);
        },
        confirmFunc: function() {
            assert.eq(coll.getIndexes().length, 2);
        }
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
        }
    });

    commands.push({
        req: {applyOps: [{op: "i", ns: coll.getFullName(), o: {_id: 1, type: "willow"}}]},
        setupFunc: function() {
            coll.insert({_id: 1, type: 'oak'});
            assert.eq(coll.count({type: 'willow'}), 0);
        },
        confirmFunc: function() {
            assert.eq(coll.count({type: 'willow'}), 1);
        }
    });

    commands.push({
        req: {aggregate: collName, pipeline: [{$sort: {type: 1}}, {$out: "foo"}]},
        setupFunc: function() {
            coll.insert({_id: 1, type: 'oak'});
            coll.insert({_id: 2, type: 'maple'});
        },
        confirmFunc: function() {
            assert.eq(db.foo.count({type: 'oak'}), 1);
            assert.eq(db.foo.count({type: 'maple'}), 1);
            db.foo.drop();
        }
    });

    commands.push({
        req: {
            mapReduce: collName,
            map: function() {
                this.tags.forEach(function(z) {
                    emit(z, 1);
                });
            },
            reduce: function(key, values) {
                return {count: values.length};
            },
            out: "foo"
        },
        setupFunc: function() {
            coll.insert({x: 1, tags: ["a", "b"]});
            coll.insert({x: 2, tags: ["b", "c"]});
            coll.insert({x: 3, tags: ["c", "a"]});
            coll.insert({x: 4, tags: ["b", "c"]});
        },
        confirmFunc: function() {
            assert.eq(db.foo.findOne({_id: 'a'}).value.count, 2);
            assert.eq(db.foo.findOne({_id: 'b'}).value.count, 3);
            assert.eq(db.foo.findOne({_id: 'c'}).value.count, 3);
            db.foo.drop();
        }
    });

    function testValidWriteConcern(cmd) {
        cmd.req.writeConcern = {w: 'majority', wtimeout: 25000};
        jsTest.log("Testing " + tojson(cmd.req));

        dropTestCollection();
        cmd.setupFunc();
        var res = db.runCommand(cmd.req);
        assert.commandWorked(res);
        assert(!res.writeConcernError,
               'command on a full replicaset had writeConcernError: ' + tojson(res));
        cmd.confirmFunc();
    }

    function testInvalidWriteConcern(cmd) {
        cmd.req.writeConcern = {w: 'invalid'};
        jsTest.log("Testing " + tojson(cmd.req));

        dropTestCollection();
        cmd.setupFunc();
        var res = coll.runCommand(cmd.req);
        assert.commandWorked(res);
        assertWriteConcernError(res);
        cmd.confirmFunc();
    }

    commands.forEach(function(cmd) {
        testValidWriteConcern(cmd);
        testInvalidWriteConcern(cmd);
    });

})();
