// Cannot implicitly shard accessed collections because of extra shard key index in sharded
// collection.
// @tags: [
//   assumes_no_implicit_index_creation,
//   requires_getmore
// ]

let t = db.in5;

function go(fn) {
    t.drop();
    let o = {};
    o[fn] = {a: 1, b: 2};
    t.insert(o);

    let x = {};
    x[fn] = {a: 1, b: 2};
    assert.eq(1, t.find(x).itcount(), "A1 - " + fn);

    let y = {};
    y[fn] = {$in: [{a: 1, b: 2}]};
    assert.eq(1, t.find(y).itcount(), "A2 - " + fn);

    let z = {};
    z[fn + ".a"] = 1;
    z[fn + ".b"] = {$in: [2]};
    assert.eq(1, t.find(z).itcount(), "A3 - " + fn); // SERVER-1366

    let i = {};
    i[fn] = 1;
    t.createIndex(i);

    assert.eq(1, t.find(x).itcount(), "B1 - " + fn);
    assert.eq(1, t.find(y).itcount(), "B2 - " + fn);
    assert.eq(1, t.find(z).itcount(), "B3 - " + fn); // SERVER-1366

    t.dropIndex(i);

    assert.eq(1, t.getIndexes().length, "T2");

    i = {};
    i[fn + ".a"] = 1;
    t.createIndex(i);
    assert.eq(2, t.getIndexes().length, "T3");

    assert.eq(1, t.find(x).itcount(), "C1 - " + fn);
    assert.eq(1, t.find(y).itcount(), "C2 - " + fn);
    assert.eq(1, t.find(z).itcount(), "C3 - " + fn); // SERVER-1366

    t.dropIndex(i);
}

go("x");
go("_id");
