// Tests that commands properly handle their underlying plan executor being killed.
(function() {
    'use strict';
    var dbpath = MongoRunner.dataPath + jsTest.name();
    resetDbpath(dbpath);
    var mongod = MongoRunner.runMongod({dbpath: dbpath});
    var db = mongod.getDB("test");
    var collName = jsTest.name();
    var coll = db.getCollection(collName);
    coll.drop();
    assert.writeOK(coll.insert({}));

    // Enable a failpoint that causes plan executors to be killed immediately.
    assert.commandWorked(coll.getDB().adminCommand({
        configureFailPoint: "planExecutorAlwaysDead",
        namespace: coll.getFullName(),
        mode: "alwaysOn"
    }));

    var res;

    // aggregate command errors if plan executor is killed.
    res = db.runCommand({aggregate: collName, pipeline: []});
    assert.commandFailed(res);
    assert(res.errmsg.indexOf("hit planExecutorAlwaysDead fail point") > -1);

    // dataSize command errors if plan executor is killed.
    res = db.runCommand({dataSize: coll.getFullName()});
    assert.commandFailed(res);
    assert(res.errmsg.indexOf("hit planExecutorAlwaysDead fail point") > -1);

    // dbHash command errors if plan executor is killed.
    res = db.runCommand("dbHash");
    assert.commandFailed(res);
    assert(res.errmsg.indexOf("hit planExecutorAlwaysDead fail point") > -1);

    // count command errors if plan executor is killed.
    res = db.runCommand({count: collName});
    assert.commandFailed(res);
    assert(res.errmsg.indexOf("hit planExecutorAlwaysDead fail point") > -1);

    // distinct command errors if plan executor is killed.
    res = db.runCommand({distinct: collName, key: "_id"});
    assert.commandFailed(res);
    assert(res.errmsg.indexOf("hit planExecutorAlwaysDead fail point") > -1);

    // findAndModify command errors if plan executor is killed.
    res = db.runCommand({findAndModify: collName, filter: {}, update: {a: 1}});
    assert.commandFailed(res);
    assert(res.errmsg.indexOf("hit planExecutorAlwaysDead fail point") > -1);

    // Build geo index.
    assert.commandWorked(coll.getDB().adminCommand({
        configureFailPoint: "planExecutorAlwaysDead",
        namespace: coll.getFullName(),
        mode: "off"
    }));
    assert.commandWorked(coll.createIndex({geoField: "2dsphere"}));
    assert.commandWorked(coll.getDB().adminCommand({
        configureFailPoint: "planExecutorAlwaysDead",
        namespace: coll.getFullName(),
        mode: "alwaysOn"
    }));

    // geoNear command errors if plan executor is killed.
    res = db.runCommand(
        {geoNear: collName, near: {type: "Point", coordinates: [0, 0]}, spherical: true});
    assert.commandFailed(res);
    assert(res.errmsg.indexOf("hit planExecutorAlwaysDead fail point") > -1);

    // group command errors if plan executor is killed.
    res = db.runCommand({
        group:
            {ns: coll.getFullName(), key: "_id", $reduce: function(curr, result) {}, initial: {}}
    });
    assert.commandFailed(res);
    assert(res.errmsg.indexOf("hit planExecutorAlwaysDead fail point") > -1);

    // find throws if plan executor is killed.
    res = assert.throws(function() {
        coll.find().itcount();
    });
    assert(res.message.indexOf("hit planExecutorAlwaysDead fail point") > -1);

    // update errors if plan executor is killed.
    res = coll.update({}, {$set: {a: 1}});
    assert.writeError(res);
    assert(res.getWriteError().errmsg.indexOf("hit planExecutorAlwaysDead fail point") > -1);

    // remove errors if plan executor is killed.
    res = coll.remove({});
    assert.writeError(res);
    assert(res.getWriteError().errmsg.indexOf("hit planExecutorAlwaysDead fail point") > -1);
})();
