(function() {
'use strict';

const t = db.count4;
t.drop();

let docs = [];
for (let i = 0; i < 100; i++) {
    docs.push({_id: i, x: i});
}
assert.commandWorked(t.insert(docs));

const q = {
    x: {$gt: 25, $lte: 75}
};

assert.eq(50, t.find(q).count(), 'collection scan fast count: ' + tojson(q));
assert.eq(50, t.find(q).itcount(), 'collection scan find iterator count: ' + tojson(q));
assert.eq(50, t.countDocuments(q), 'collection scan aggregation count: ' + tojson(q));

assert.commandWorked(t.createIndex({x: 1}));

assert.eq(50, t.find(q).count(), 'index scan fast count: ' + tojson(q));
assert.eq(50, t.find(q).itcount(), 'index scan find iterator count: ' + tojson(q));
assert.eq(50, t.countDocuments(q), 'index scan aggregation count: ' + tojson(q));
})();
