// SERVER-7343: allow $within without a geo index.

(function() {
'use strict';

const t = db.geo_circle1_noindex;
t.drop();

const searches = [
    [[5, 5], 3],
    [[5, 5], 1],
    [[5, 5], 5],
    [[0, 5], 5],
];
let correct = searches.map(function(z) {
    return [];
});

let num = 0;

let docs = [];
for (let x = 0; x <= 20; x++) {
    for (let y = 0; y <= 20; y++) {
        const o = {_id: num++, loc: [x, y]};
        docs.push(o);
        for (let i = 0; i < searches.length; i++)
            if (Geo.distance([x, y], searches[i][0]) <= searches[i][1])
                correct[i].push(o);
    }
}
assert.commandWorked(t.insert(docs));

for (let i = 0; i < searches.length; i++) {
    const q = {loc: {$within: {$center: searches[i]}}};
    assert.eq(correct[i].length, t.find(q).itcount(), "itcount : " + tojson(searches[i]));
    assert.eq(correct[i].length, t.find(q).count(), "count : " + tojson(searches[i]));
    assert.eq(correct[i].length, t.countDocuments(q), "aggregation : " + tojson(searches[i]));
}
})();
