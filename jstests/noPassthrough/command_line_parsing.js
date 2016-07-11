// validate command line parameter parsing

var baseName = "jstests_slowNightly_command_line_parsing";

// test notablescan
var m = MongoRunner.runMongod({notablescan: ""});
m.getDB(baseName).getCollection(baseName).save({a: 1});
assert.throws(function() {
    m.getDB(baseName).getCollection(baseName).find({a: 1}).toArray();
});

// test config file
var m2 = MongoRunner.runMongod({config: "jstests/libs/testconfig"});

var m2expected = {
    "parsed": {
        "config": "jstests/libs/testconfig",
        "storage": {"dbPath": m2.dbpath},
        "net": {"port": m2.port},
        "help": false,
        "version": false,
        "sysinfo": false
    }
};
var m2result = m2.getDB("admin").runCommand("getCmdLineOpts");

// remove variables that depend on the way the test is started.
delete m2result.parsed.nopreallocj;
delete m2result.parsed.setParameter;
delete m2result.parsed.storage.engine;
delete m2result.parsed.storage.journal;
delete m2result.parsed.storage.rocksdb;
delete m2result.parsed.storage.wiredTiger;
assert.docEq(m2expected.parsed, m2result.parsed);

// test JSON config file
var m3 = MongoRunner.runMongod({config: "jstests/libs/testconfig"});

var m3expected = {
    "parsed": {
        "config": "jstests/libs/testconfig",
        "storage": {"dbPath": m3.dbpath},
        "net": {"port": m3.port},
        "help": false,
        "version": false,
        "sysinfo": false
    }
};
var m3result = m3.getDB("admin").runCommand("getCmdLineOpts");

// remove variables that depend on the way the test is started.
delete m3result.parsed.nopreallocj;
delete m3result.parsed.setParameter;
delete m3result.parsed.storage.engine;
delete m3result.parsed.storage.journal;
delete m3result.parsed.storage.rocksdb;
delete m3result.parsed.storage.wiredTiger;
assert.docEq(m3expected.parsed, m3result.parsed);
