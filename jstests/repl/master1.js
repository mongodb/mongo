// Test handling of clock skew and optimes across mongod instances
//
// There is no automatic fail-over in a master/slave deployment, so if the master goes down, no new
// master will be elected. Therefore if the master is using an ephemeral storage engine, it cannot
// be restarted without losing all data. This test expects that restarting the master will maintain
// the node's data, so cannot be run with ephemeral storage engines.
// @tags: [requires_persistence]

var baseName = "jstests_repl_master1test";

oplog = function() {
    return m.getDB("local").oplog.$main;
};

lastop = function() {
    return oplog().find().sort({$natural: -1}).next();
};

am = function() {
    return m.getDB(baseName).a;
};

rt = new ReplTest(baseName);

m = rt.start(true);

am().save({});
assert.eq("i", lastop().op);

op = lastop();
printjson(op);
op.ts.t = op.ts.t + 600000;  // 10 minutes
assert.commandWorked(m.getDB("local").runCommand({godinsert: "oplog.$main", obj: op}));

rt.stop(true);
m = rt.start(true, null, true);

assert.eq(op.ts.t, lastop().ts.t);
am().save({});
assert.eq(op.ts.t, lastop().ts.t);
assert.eq(op.ts.i + 1, lastop().ts.i);

op = lastop();
printjson(op);
op.ts.i = Math.pow(2, 31) - 1;
printjson(op);
assert.commandWorked(m.getDB("local").runCommand({godinsert: "oplog.$main", obj: op}));

rt.stop(true);
m = rt.start(true, null, true);
assert.eq(op.ts.i, lastop().ts.i);

assert.throws(function() {
    am().save({});  // triggers fassert because ofclock skew
});

assert.neq(0, rt.stop(true));  // fasserted
