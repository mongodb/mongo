/**
 * @tags: [
 *   assumes_balancer_off,
 *   # explain does not support majority read concern
 *   assumes_read_concern_local,
 * ]
 */

(function() {
'use strict';

const t = db.geo_circle1;
t.drop();

assert.commandWorked(t.createIndex({loc: "2d"}));

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
    // print( tojson( searches[i] ) + "\t" + correct[i].length )
    const q = {loc: {$within: {$center: searches[i]}}};

    // correct[i].forEach( printjson )
    // printjson( q );
    // t.find( q ).forEach( printjson )

    // printjson( correct[i].map( function(z){ return z._id; } ).sort() )
    // printjson( t.find(q).map( function(z){ return z._id; } ).sort() )

    assert.eq(correct[i].length, t.find(q).itcount(), "itcount : " + tojson(searches[i]));
    assert.eq(correct[i].length, t.find(q).count(), "count : " + tojson(searches[i]));
    assert.eq(correct[i].length, t.countDocuments(q), "aggregation : " + tojson(searches[i]));
    const explain = t.find(q).explain("executionStats");
    // The index should be at least minimally effective in preventing the full collection scan.
    assert.gt(num,
              explain.executionStats.totalKeysExamined,
              "nscanned : " +
                  tojson(searches[i] + "; query : " + tojson(q, '', true) +
                         "; explain : " + tojson(explain)));
}
})();
