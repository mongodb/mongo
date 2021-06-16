(function() {
"use strict";

function benchrun_sub_insert() {
    const t = db.benchrun_sub;
    t.drop();
    const offset = 10000;
    const ops = [{
        op: "insert",
        ns: "test.benchrun_sub",
        doc: {
            x: {"#RAND_INT": [0, 100]},
            curDate: {"#CUR_DATE": 0},
            futureDate: {"#CUR_DATE": offset},
            pastDate: {"#CUR_DATE": (0 - offset)}
        },
        writeCmd: true
    }];

    const res = benchRun({parallel: 1, seconds: 10, ops: ops, host: db.getMongo().host});

    assert.gt(res.insert, 0);

    t.find().forEach(function(doc) {
        const field = doc.x;
        assert.gte(field, 0);
        assert.lt(field, 100);
        assert.lt(doc.pastDate, doc.curDate);
        assert.lt(doc.curDate, doc.futureDate);
    });
}

function benchrun_sub_update() {
    const t = db.benchrun_sub;
    t.drop();

    const ops = [{
        op: "update",
        ns: "test.benchrun_sub",
        query: {x: {"#RAND_INT": [0, 100]}},
        update: {$inc: {x: 1}},
        writeCmd: true
    }];

    for (let i = 0; i < 100; ++i) {
        assert.commandWorked(t.insert({x: i}));
    }

    const res = benchRun({parallel: 1, seconds: 10, ops: ops, host: db.getMongo().host});

    let field_sum = 0;
    t.find().forEach(function(doc) {
        field_sum += doc.x;
    });

    assert.gt(field_sum, 4950);  // 1 + 2 + .. 99 = 4950
}

function benchrun_sub_remove() {
    const t = db.benchrun_sub;
    t.drop();

    const ops = [
        {op: "remove", ns: "test.benchrun_sub", query: {x: {"#RAND_INT": [0, 100]}}, writeCmd: true}
    ];

    for (let i = 0; i < 100; ++i) {
        assert.commandWorked(t.insert({x: i}));
    }

    const res = benchRun({parallel: 1, seconds: 10, ops: ops, host: db.getMongo().host});

    assert.lt(t.count(), 100);
}

benchrun_sub_insert();
benchrun_sub_update();
benchrun_sub_remove();
})();
