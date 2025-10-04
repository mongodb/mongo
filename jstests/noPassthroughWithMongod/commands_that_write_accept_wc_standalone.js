/**
 * This file tests that commands that should accept a writeConcern on a standalone can accept one.
 * This does not test that writes with j: true are actually made durable or that if j: true fails
 * that there is a writeConcern error.
 * @tags: [requires_persistence]
 */

let collName = "leaves";
let coll = db[collName];

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
    req: {applyOps: [{op: "i", ns: coll.getFullName(), o: {_id: 1, type: "willow"}}]},
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
        assert.commandWorked(coll.insert({_id: 1, type: "oak"}));
        assert.commandWorked(coll.insert({_id: 2, type: "maple"}));
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
    cmd.req.writeConcern = {w: 1, j: true};
    jsTest.log("Testing " + tojson(cmd.req));

    coll.drop();
    cmd.setupFunc();
    let res = db.runCommand(cmd.req);
    assert.commandWorked(res);
    assert(!res.writeConcernError, "command had writeConcernError: " + tojson(res));
    cmd.confirmFunc();
}

function testInvalidWriteConcern(wc, cmd) {
    cmd.req.writeConcern = wc;
    jsTest.log("Testing " + tojson(cmd.req));

    coll.drop();
    cmd.setupFunc();
    let res = coll.runCommand(cmd.req);
    // These commands should fail because standalone writeConcerns are found to be invalid at
    // the validation stage when the writeConcern is parsed, before the command is run.
    assert.commandFailed(res);
}

let invalidWriteConcerns = [{w: "invalid"}, {w: 2}, {j: "invalid"}];

commands.forEach(function (cmd) {
    testValidWriteConcern(cmd);
    invalidWriteConcerns.forEach(function (wc) {
        testInvalidWriteConcern(wc, cmd);
    });
});
