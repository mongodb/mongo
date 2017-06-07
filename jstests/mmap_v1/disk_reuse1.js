(function() {
    "use strict";
    const conn = MongoRunner.runMongod({smallfiles: "", nojournal: ""});
    assert.neq(null, conn, "mongod failed to start.");
    db = conn.getDB("test");
    const t = db.disk_reuse1;
    t.drop();

    const N = 10000;

    function k() {
        return Math.floor(Math.random() * N);
    }

    let s = "";
    while (s.length < 1024)
        s += "abc";

    var bulk = t.initializeUnorderedBulkOp();
    for (var i = 0; i < N; i++) {
        bulk.insert({_id: i, s: s});
    }
    assert.writeOK(bulk.execute());

    const orig = t.stats();

    t.remove({});

    bulk = t.initializeUnorderedBulkOp();
    for (let i = 0; i < N; i++) {
        bulk.insert({_id: i, s: s});
    }
    assert.writeOK(bulk.execute());

    assert.eq(orig.storageSize, t.stats().storageSize, "A");

    for (let j = 0; j < 100; j++) {
        for (let i = 0; i < N; i++) {
            bulk = t.initializeUnorderedBulkOp();
            var r = Math.random();
            if (r > .5)
                bulk.find({_id: i}).remove();
            else
                bulk.find({_id: i}).upsert().updateOne({_id: i, s: s});
        }

        assert.writeOK(bulk.execute());
        assert.eq(orig.storageSize, t.stats().storageSize, "B" + j);
    }

    MongoRunner.stopMongod(conn);
})();
