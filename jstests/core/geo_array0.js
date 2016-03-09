// Make sure the very basics of geo arrays are sane by creating a few multi location docs
t = db.geoarray;

function test(index) {
    t.drop();
    t.insert({zip: "10001", loc: [[10, 10], [50, 50]]});
    t.insert({zip: "10002", loc: [[20, 20], [50, 50]]});
    var res = t.insert({zip: "10003", loc: [[30, 30], [50, 50]]});
    assert.writeOK(res);

    if (index) {
        assert.commandWorked(t.ensureIndex({loc: "2d", zip: 1}));
        assert.eq(2, t.getIndexKeys().length);
    }

    res = t.insert({zip: "10004", loc: [[40, 40], [50, 50]]});
    assert.writeOK(res);

    // test normal access
    printjson(t.find({loc: {$within: {$box: [[0, 0], [45, 45]]}}}).toArray());
    assert.eq(4, t.find({loc: {$within: {$box: [[0, 0], [45, 45]]}}}).count());
    assert.eq(4, t.find({loc: {$within: {$box: [[45, 45], [50, 50]]}}}).count());
}

// test(false); // this was removed as part of SERVER-6400
test(true);
