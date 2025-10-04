// background indexing test during inserts.

assert(db.getName() == "test");

let t = db.bg1;
t.drop();

let a = new Mongo(db.getMongo().host).getDB(db.getName());

let bulk = t.initializeUnorderedBulkOp();
for (var i = 0; i < 100000; i++) {
    bulk.insert({y: "aaaaaaaaaaaa", i: i});
    if (i % 10000 == 0) {
        assert.commandWorked(bulk.execute());
        bulk = t.initializeUnorderedBulkOp();
        print(i);
    }
}

// start bg indexing
a.bg1.createIndex({i: 1}, {name: "i_1"});

// add more data
bulk = t.initializeUnorderedBulkOp();
for (var i = 0; i < 100000; i++) {
    bulk.insert({i: i});
    if (i % 10000 == 0) {
        printjson(db.currentOp());
        assert.commandWorked(bulk.execute());
        bulk = t.initializeUnorderedBulkOp();
        print(i);
    }
}

assert.commandWorked(bulk.execute());

printjson(db.currentOp());

for (var i = 0; i < 40; i++) {
    if (db.currentOp().inprog.length == 0) break;
    print("waiting");
    sleep(1000);
}

let idx = t.getIndexes();
assert(idx[1].key.i == 1);
