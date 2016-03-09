// background indexing test during inserts.

assert(db.getName() == "test");

t = db.bg1;
t.drop();

var a = new Mongo(db.getMongo().host).getDB(db.getName());

var bulk = t.initializeUnorderedBulkOp();
for (var i = 0; i < 100000; i++) {
    bulk.insert({y: 'aaaaaaaaaaaa', i: i});
    if (i % 10000 == 0) {
        assert.writeOK(bulk.execute());
        bulk = t.initializeUnorderedBulkOp();
        print(i);
    }
}

// start bg indexing
a.bg1.ensureIndex({i: 1}, {name: "i_1", background: true});

// add more data
bulk = t.initializeUnorderedBulkOp();
for (var i = 0; i < 100000; i++) {
    bulk.insert({i: i});
    if (i % 10000 == 0) {
        printjson(db.currentOp());
        assert.writeOK(bulk.execute());
        bulk = t.initializeUnorderedBulkOp();
        print(i);
    }
}

assert.writeOK(bulk.execute());

printjson(db.currentOp());

for (var i = 0; i < 40; i++) {
    if (db.currentOp().inprog.length == 0)
        break;
    print("waiting");
    sleep(1000);
}

var idx = t.getIndexes();
assert(idx[1].key.i == 1);
