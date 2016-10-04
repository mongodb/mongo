(function() {
    // Tests that errors encountered on shards are correctly returned to the client when mongos uses
    // the legacy DBClientCursor method of executing commands on shards. We use aggregation here
    // specifically because it is one of the few query paths that still uses the legacy DBClient
    // classes in mongos.
    "use strict";

    var st = new ShardingTest({mongos: 1, shards: 1, rs: {nodes: 3}});

    var db = st.getDB('test');
    db.setSlaveOk(true);

    assert.writeOK(db.foo.insert({a: 1}, {writeConcern: {w: 3}}));
    assert.commandWorked(
        db.runCommand({aggregate: 'foo', pipeline: [{$project: {total: {'$add': ['$a', 1]}}}]}));

    assert.writeOK(db.foo.insert({a: [1, 2]}, {writeConcern: {w: 3}}));

    var res =
        db.runCommand({aggregate: 'foo', pipeline: [{$project: {total: {'$add': ['$a', 1]}}}]});
    assert.commandFailed(res);
    assert.eq("$add only supports numeric or date types, not array", res.errmsg, printjson(res));
}());
