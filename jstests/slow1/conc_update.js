(function() {
"use strict";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
db = conn.getDB("concurrency");
db.dropDatabase();

const NRECORDS = 3 * 1024 * 1024;

print("loading " + NRECORDS + " documents (progress msg every 1024*1024 documents)");
var bulk = db.conc.initializeUnorderedBulkOp();
for (var i = 0; i < NRECORDS; i++) {
    bulk.insert({x: i});
}
assert.commandWorked(bulk.execute());

print("making an index (this will take a while)");
db.conc.createIndex({x: 1});

var c1 = db.conc.count({x: {$lt: NRECORDS}});

const updater = startParallelShell(
    "db = db.getSiblingDB('concurrency');\
                                  db.concflag.insert({ inprog: true });\
                                  sleep(20);\
                                  assert.commandWorked(db.conc.update({}, \
                                                 { $inc: { x: " +
    NRECORDS +
    "}}, false, true)); \
                                  assert.commandWorked(db.concflag.update({}, { inprog: false }));");

assert.soon(function() {
    var x = db.concflag.findOne();
    return x && x.inprog;
}, "wait for fork", 30000, 1);

let querycount = 0;
let decrements = 0;
let misses = 0;

assert.soon(function() {
    const c2 = db.conc.count({x: {$lt: NRECORDS}});
    print(c2);
    querycount++;
    if (c2 < c1)
        decrements++;
    else
        misses++;
    c1 = c2;
    return !db.concflag.findOne().inprog;
}, "update never finished", 2 * 60 * 60 * 1000, 10);

print(querycount + " queries, " + decrements + " decrements, " + misses + " misses");

assert.eq(NRECORDS, db.conc.count(), "AT END 1");

updater();  // wait()

MongoRunner.stopMongod(conn);
})();
