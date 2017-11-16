/**
 * SERVER-32001: Test that indexing paths for non-unique, partial, unique, partial&unique
 * crud operations correctly handle WriteConflictExceptions.
 */
(function() {
    "strict";

    var conn = MongoRunner.runMongod();
    var testDB = conn.getDB("test");

    var t = testDB.jstests_parallel_allops;
    t.drop();

    t.createIndex({x: 1, _id: 1}, {partialFilterExpression: {_id: {$lt: 500}}, unique: true});
    t.createIndex({y: -1, _id: 1}, {unique: true});
    t.createIndex({x: -1}, {partialFilterExpression: {_id: {$gte: 500}}, unique: false});
    t.createIndex({y: 1}, {unique: false});

    var _id = {
        "#RAND_INT": [0, 1000]
    };
    var ops = [
        {op: "remove", ns: t.getFullName(), query: {_id}},
        {op: "update", ns: t.getFullName(), query: {_id}, update: {$inc: {x: 1}}, upsert: true},
        {op: "update", ns: t.getFullName(), query: {_id}, update: {$inc: {y: 1}}, upsert: true},
    ];

    var seconds = 5;
    var parallel = 5;
    var host = testDB.getMongo().host;

    var benchArgs = {ops, seconds, parallel, host};

    assert.commandWorked(testDB.adminCommand({
        configureFailPoint: 'WTWriteConflictExceptionForReads',
        mode: {activationProbability: 0.01}
    }));
    assert.commandWorked(testDB.adminCommand(
        {configureFailPoint: 'WTWriteConflictException', mode: {activationProbability: 0.01}}));
    res = benchRun(benchArgs);
    printjson({res});

    assert.commandWorked(
        testDB.adminCommand({configureFailPoint: 'WTWriteConflictException', mode: "off"}));
    assert.commandWorked(testDB.adminCommand(
        {configureFailPoint: 'WTWriteConflictExceptionForReads', mode: "off"}));
    res = t.validate();
    assert(res.valid, tojson(res));
})();
