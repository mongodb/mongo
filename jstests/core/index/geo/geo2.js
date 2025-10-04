// @tags: [
//   operations_longer_than_stepdown_interval_in_txns,
//   requires_fastcount,
// ]

let t = db.geo2;
t.drop();

let n = 1;
let arr = [];
for (let x = -100; x < 100; x += 2) {
    for (let y = -100; y < 100; y += 2) {
        arr.push({_id: n++, loc: [x, y]});
    }
}
t.insert(arr);
assert.eq(t.count(), 100 * 100);
assert.eq(t.count(), n - 1);

t.createIndex({loc: "2d"});

function a(cur) {
    let total = 0;
    let outof = 0;
    while (cur.hasNext()) {
        let o = cur.next();
        total += Geo.distance([50, 50], o.loc);
        outof++;
    }
    return total / outof;
}

assert.close(1.33333, a(t.find({loc: {$near: [50, 50]}}).limit(3)), "B2");

printjson(t.find({loc: {$near: [50, 50]}}).explain());

assert.lt(3, a(t.find({loc: {$near: [50, 50]}}).limit(50)), "C1");
assert.gt(3, a(t.find({loc: {$near: [50, 50, 3]}}).limit(50)), "C2");
assert.gt(3, a(t.find({loc: {$near: [50, 50], $maxDistance: 3}}).limit(50)), "C3");

// SERVER-8974 - test if $geoNear operator works with 2d index as well
let geoNear_cursor = t.find({loc: {$geoNear: [50, 50]}}).limit(100);
assert.eq(geoNear_cursor.count(true), 100);
