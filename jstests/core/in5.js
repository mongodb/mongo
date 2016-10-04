
t = db.in5;

function go(fn) {
    t.drop();
    o = {};
    o[fn] = {a: 1, b: 2};
    t.insert(o);

    x = {};
    x[fn] = {a: 1, b: 2};
    assert.eq(1, t.find(x).itcount(), "A1 - " + fn);

    y = {};
    y[fn] = {$in: [{a: 1, b: 2}]};
    assert.eq(1, t.find(y).itcount(), "A2 - " + fn);

    z = {};
    z[fn + ".a"] = 1;
    z[fn + ".b"] = {$in: [2]};
    assert.eq(1, t.find(z).itcount(), "A3 - " + fn);  // SERVER-1366

    i = {};
    i[fn] = 1;
    t.ensureIndex(i);

    assert.eq(1, t.find(x).itcount(), "B1 - " + fn);
    assert.eq(1, t.find(y).itcount(), "B2 - " + fn);
    assert.eq(1, t.find(z).itcount(), "B3 - " + fn);  // SERVER-1366

    t.dropIndex(i);

    assert.eq(1, t.getIndexes().length, "T2");

    i = {};
    i[fn + ".a"] = 1;
    t.ensureIndex(i);
    assert.eq(2, t.getIndexes().length, "T3");

    assert.eq(1, t.find(x).itcount(), "C1 - " + fn);
    assert.eq(1, t.find(y).itcount(), "C2 - " + fn);
    assert.eq(1, t.find(z).itcount(), "C3 - " + fn);  // SERVER-1366

    t.dropIndex(i);
}

go("x");
go("_id");
