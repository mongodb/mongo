// this is to make sure that temp collections get cleaned up on restart.
//
// This test requires persistence beacuase it assumes data will survive a restart.
// @tags: [requires_persistence, requires_replication]

testname = 'temp_namespace_sw';

var conn = MongoRunner.runMongod();
d = conn.getDB('test');
assert.commandWorked(d.runCommand({
    applyOps:
        [{op: "c", ns: d.getName() + ".$cmd", o: {create: testname + 'temp1', temp: true}}]
}));
d[testname + 'temp1'].ensureIndex({x: 1});
assert.commandWorked(d.runCommand(
    {applyOps: [{op: "c", ns: d.getName() + ".$cmd", o: {create: testname + 'temp2', temp: 1}}]}));
d[testname + 'temp2'].ensureIndex({x: 1});
assert.commandWorked(d.runCommand({
    applyOps:
        [{op: "c", ns: d.getName() + ".$cmd", o: {create: testname + 'keep1', temp: false}}]
}));
assert.commandWorked(d.runCommand(
    {applyOps: [{op: "c", ns: d.getName() + ".$cmd", o: {create: testname + 'keep2', temp: 0}}]}));
d.runCommand({create: testname + 'keep3'});
d[testname + 'keep4'].insert({});

function countCollectionNames(theDB, regex) {
    return theDB.getCollectionNames()
        .filter(function(z) {
            return z.match(regex);
        })
        .length;
}

assert.eq(countCollectionNames(d, /temp\d$/), 2);
assert.eq(countCollectionNames(d, /keep\d$/), 4);
MongoRunner.stopMongod(conn);

conn = MongoRunner.runMongod({
    restart: true,
    cleanData: false,
    dbpath: conn.dbpath,
});
d = conn.getDB('test');
assert.eq(countCollectionNames(d, /temp\d$/), 0);
assert.eq(countCollectionNames(d, /keep\d$/), 4);
MongoRunner.stopMongod(conn);
