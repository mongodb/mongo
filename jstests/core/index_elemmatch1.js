/**
 * Tests find with $elemMatch when supporting indexes are in place.
 * @tags: [assumes_balancer_off]
 */
(function() {
    "use strict";

    const coll = db.index_elemmatch1;
    coll.drop();

    let x = 0;
    let y = 0;
    const bulk = coll.initializeUnorderedBulkOp();
    for (let a = 0; a < 10; a++) {
        for (let b = 0; b < 10; b++) {
            bulk.insert({a: a, b: b % 10, arr: [{x: x++ % 10, y: y++ % 10}]});
        }
    }
    assert.commandWorked(bulk.execute());

    assert.commandWorked(coll.createIndex({a: 1, b: 1}));
    assert.commandWorked(coll.createIndex({"arr.x": 1, a: 1}));

    const query = {a: 5, b: {$in: [1, 3, 5]}, arr: {$elemMatch: {x: 5, y: 5}}};

    const count = coll.find(query).itcount();
    assert.eq(count, 1);

    const explain = coll.find(query).hint({"arr.x": 1, a: 1}).explain("executionStats");
    assert.commandWorked(explain);
    assert.eq(count, explain.executionStats.totalKeysExamined, explain);
})();
