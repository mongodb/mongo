/**
 * This file tests that commands that do writes accept a write concern. This file does not test
 * mongos commands or user management commands, both of which are tested separately. This test
 * defines various database commands and what they expect to be true before and after the fact.
 * It then runs the commands with an invalid writeConcern and a valid writeConcern and
 * ensures that they succeed and fail appropriately.
 *
 * @tags: [
 *   requires_scripting,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

let replTest = new ReplSetTest({
    name: "WCSet",
    // Set priority of secondaries to zero to prevent spurious elections.
    nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
    settings: {chainingAllowed: false},
});
replTest.startSet();
replTest.initiate();

let primary = replTest.getPrimary();
let dbName = "wc-test";
var db = primary.getDB(dbName);
let collName = "leaves";
let coll = db[collName];

function dropTestCollection() {
    replTest.awaitReplication();
    coll.drop();
    assert.eq(0, coll.find().itcount(), "test collection not empty");
}

dropTestCollection();

let commands = [];

commands.push({
    req: {insert: collName, documents: [{type: "maple"}]},
    setupFunc: function () {},
    confirmFunc: function () {
        assert.eq(coll.count({type: "maple"}), 1);
    },
});

commands.push({
    req: {createIndexes: collName, indexes: [{key: {"type": 1}, name: "type_index"}]},
    setupFunc: function () {
        assert.commandWorked(coll.insert({type: "oak"}));
        assert.eq(coll.getIndexes().length, 1);
    },
    confirmFunc: function () {
        assert.eq(coll.getIndexes().length, 2);
    },
});

commands.push({
    req: {
        update: collName,
        updates: [
            {
                q: {type: "oak"},
                u: [{$set: {type: "ginkgo"}}],
            },
        ],
        writeConcern: {w: "majority"},
    },
    setupFunc: function () {
        assert.commandWorked(coll.insert({type: "oak"}));
        assert.eq(coll.count({type: "ginkgo"}), 0);
        assert.eq(coll.count({type: "oak"}), 1);
    },
    confirmFunc: function () {
        assert.eq(coll.count({type: "ginkgo"}), 1);
        assert.eq(coll.count({type: "oak"}), 0);
    },
});

commands.push({
    req: {
        findAndModify: collName,
        query: {type: "oak"},
        update: {$set: {type: "ginkgo"}},
        writeConcern: {w: "majority"},
    },
    setupFunc: function () {
        assert.commandWorked(coll.insert({type: "oak"}));
        assert.eq(coll.count({type: "ginkgo"}), 0);
        assert.eq(coll.count({type: "oak"}), 1);
    },
    confirmFunc: function () {
        assert.eq(coll.count({type: "ginkgo"}), 1);
        assert.eq(coll.count({type: "oak"}), 0);
    },
});

commands.push({
    req: {
        findAndModify: collName,
        query: {type: "oak"},
        update: [{$set: {type: "ginkgo"}}],
        writeConcern: {w: "majority"},
    },
    setupFunc: function () {
        assert.commandWorked(coll.insert({type: "oak"}));
        assert.eq(coll.count({type: "ginkgo"}), 0);
        assert.eq(coll.count({type: "oak"}), 1);
    },
    confirmFunc: function () {
        assert.eq(coll.count({type: "ginkgo"}), 1);
        assert.eq(coll.count({type: "oak"}), 0);
    },
});

commands.push({
    req: {applyOps: [{op: "u", ns: coll.getFullName(), o: {_id: 1, type: "willow"}, o2: {_id: 1}}]},
    setupFunc: function () {
        assert.commandWorked(coll.insert({_id: 1, type: "oak"}));
        assert.eq(coll.count({type: "willow"}), 0);
    },
    confirmFunc: function () {
        assert.eq(coll.count({type: "willow"}), 1);
    },
});

commands.push({
    req: {aggregate: collName, pipeline: [{$sort: {type: 1}}, {$out: "foo"}], cursor: {}},
    setupFunc: function () {
        coll.insert({_id: 1, type: "oak"});
        coll.insert({_id: 2, type: "maple"});
    },
    confirmFunc: function () {
        assert.eq(db.foo.count({type: "oak"}), 1);
        assert.eq(db.foo.count({type: "maple"}), 1);
        db.foo.drop();
    },
});

commands.push({
    req: {
        mapReduce: collName,
        map: function () {
            this.tags.forEach(function (z) {
                emit(z, 1);
            });
        },
        reduce: function (key, values) {
            // We may be re-reducing values that have already been partially reduced. In that case,
            // we expect to see an object like {count: <count>} in the array of input values.
            const numValues = values.reduce(function (acc, currentValue) {
                if (typeof currentValue === "object") {
                    return acc + currentValue.count;
                } else {
                    return acc + 1;
                }
            }, 0);
            return {count: numValues};
        },
        out: "foo",
    },
    setupFunc: function () {
        assert.commandWorked(coll.insert({x: 1, tags: ["a", "b"]}));
        assert.commandWorked(coll.insert({x: 2, tags: ["b", "c"]}));
        assert.commandWorked(coll.insert({x: 3, tags: ["c", "a"]}));
        assert.commandWorked(coll.insert({x: 4, tags: ["b", "c"]}));
    },
    confirmFunc: function () {
        assert.eq(db.foo.findOne({_id: "a"}).value.count, 2);
        assert.eq(db.foo.findOne({_id: "b"}).value.count, 3);
        assert.eq(db.foo.findOne({_id: "c"}).value.count, 3);
        db.foo.drop();
    },
});

function testValidWriteConcern(cmd) {
    cmd.req.writeConcern = {w: "majority", wtimeout: ReplSetTest.kDefaultTimeoutMS};
    jsTest.log("Testing " + tojson(cmd.req));

    dropTestCollection();
    cmd.setupFunc();
    let res = db.runCommand(cmd.req);
    assert.commandWorked(res);
    assert(!res.writeConcernError, "command on a full replicaset had writeConcernError: " + tojson(res));
    cmd.confirmFunc();
}

function testInvalidWriteConcern(cmd) {
    cmd.req.writeConcern = {w: "invalid"};
    jsTest.log("Testing " + tojson(cmd.req));

    dropTestCollection();
    cmd.setupFunc();
    let res = coll.runCommand(cmd.req);
    assert.commandFailedWithCode(res, ErrorCodes.UnknownReplWriteConcern);
    cmd.confirmFunc();
}

commands.forEach(function (cmd) {
    testValidWriteConcern(cmd);
    testInvalidWriteConcern(cmd);
});

replTest.stopSet();
