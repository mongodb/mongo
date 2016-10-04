function benchrun_sub_insert(use_write_command) {
    t = db.benchrun_sub;
    t.drop();
    var offset = 10000;
    ops = [{
        op: "insert",
        ns: "test.benchrun_sub",
        doc: {
            x: {"#RAND_INT": [0, 100]},
            curDate: {"#CUR_DATE": 0},
            futureDate: {"#CUR_DATE": offset},
            pastDate: {"#CUR_DATE": (0 - offset)}
        },
        writeCmd: use_write_command,
    }];

    res = benchRun({parallel: 1, seconds: 10, ops: ops, host: db.getMongo().host});

    assert.gt(res.insert, 0);

    t.find().forEach(function(doc) {
        var field = doc.x;
        assert.gte(field, 0);
        assert.lt(field, 100);
        assert.lt(doc.pastDate, doc.curDate);
        assert.lt(doc.curDate, doc.futureDate);
    });
}

function benchrun_sub_update(use_write_command) {
    t = db.benchrun_sub;
    t.drop();

    ops = [{
        op: "update",
        ns: "test.benchrun_sub",
        query: {x: {"#RAND_INT": [0, 100]}},
        update: {$inc: {x: 1}},
        writeCmd: use_write_command
    }];

    for (var i = 0; i < 100; ++i) {
        t.insert({x: i});
    }

    res = benchRun({parallel: 1, seconds: 10, ops: ops, host: db.getMongo().host});

    var field_sum = 0;
    t.find().forEach(function(doc) {
        field_sum += doc.x;
    });

    assert.gt(field_sum, 4950);  // 1 + 2 + .. 99 = 4950
}

function benchrun_sub_remove(use_write_command) {
    t = db.benchrun_sub;
    t.drop();

    ops = [{
        op: "remove",
        ns: "test.benchrun_sub",
        query: {x: {"#RAND_INT": [0, 100]}},
        writeCmd: use_write_command,
    }];

    for (var i = 0; i < 100; ++i) {
        t.insert({x: i});
    }

    res = benchRun({parallel: 1, seconds: 10, ops: ops, host: db.getMongo().host});

    assert.eq(t.count(), 0);
}

benchrun_sub_insert(true);
benchrun_sub_insert(false);
benchrun_sub_update(true);
benchrun_sub_update(false);
benchrun_sub_remove(true);
benchrun_sub_remove(false);
