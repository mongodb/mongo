
t = db.index_elemmatch1;
t.drop();

x = 0;
y = 0;
var bulk = t.initializeUnorderedBulkOp();
for (a = 0; a < 100; a++) {
    for (b = 0; b < 100; b++) {
        bulk.insert({a: a, b: b % 10, arr: [{x: x++ % 10, y: y++ % 10}]});
    }
}
assert.writeOK(bulk.execute());

t.ensureIndex({a: 1, b: 1});
t.ensureIndex({"arr.x": 1, a: 1});

assert.eq(100, t.find({a: 55}).itcount(), "A1");
assert.eq(10, t.find({a: 55, b: 7}).itcount(), "A2");

q = {
    a: 55,
    b: {$in: [1, 5, 8]}
};
assert.eq(30, t.find(q).itcount(), "A3");

q.arr = {
    $elemMatch: {x: 5, y: 5}
};
assert.eq(10, t.find(q).itcount(), "A4");

function nscannedForCursor(explain, cursor) {
    plans = explain.allPlans;
    for (i in plans) {
        if (plans[i].cursor == cursor) {
            return plans[i].nscanned;
        }
    }
    return -1;
}

var explain = t.find(q).hint({"arr.x": 1, a: 1}).explain("executionStats");
assert.eq(t.find(q).itcount(), explain.executionStats.totalKeysExamined);

printjson(t.find(q).explain());
print("Num results:");
assert.eq(10, t.find(q).itcount());
printjson(t.find(q).itcount());
