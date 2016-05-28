// this is to make sure that temp collections get cleaned up on restart.
//
// This test requires persistence beacuase it assumes data will survive a restart.
// @tags: [requires_persistence]

testname = 'temp_namespace_sw';

var conn = MongoRunner.runMongod({smallfiles: "", noprealloc: "", nopreallocj: ""});
d = conn.getDB('test');
d.runCommand({create: testname + 'temp1', temp: true});
d[testname + 'temp1'].ensureIndex({x: 1});
d.runCommand({create: testname + 'temp2', temp: 1});
d[testname + 'temp2'].ensureIndex({x: 1});
d.runCommand({create: testname + 'keep1', temp: false});
d.runCommand({create: testname + 'keep2', temp: 0});
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
    smallfiles: "",
    noprealloc: "",
    nopreallocj: ""
});
d = conn.getDB('test');
assert.eq(countCollectionNames(d, /temp\d$/), 0);
assert.eq(countCollectionNames(d, /keep\d$/), 4);
MongoRunner.stopMongod(conn);
