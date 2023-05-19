// Test sorting with long longs and doubles - SERVER-3719

let t = db.jstests_numberlong3;
t.drop();

let s = "11235399833116571";
for (let i = 10; i >= 0; --i) {
    let n = NumberLong(s + i);
    t.save({x: n});
    if (0) {  // SERVER-3719
        t.save({x: n.floatApprox});
    }
}

let ret = t.find().sort({x: 1}).toArray().filter(function(x) {
    return typeof (x.x.floatApprox) != 'undefined';
});

// printjson( ret );

for (let i = 1; i < ret.length; ++i) {
    let first = ret[i - 1].x.toString();
    let second = ret[i].x.toString();
    if (first.length == second.length) {
        assert.lte(ret[i - 1].x.toString(), ret[i].x.toString());
    }
}
