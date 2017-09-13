/**
 * Ensure that $match is always applied after $limit.
 */
(function() {
    "use strict";

    let coll = db.jstests_match_swap_limit;
    coll.drop();

    assert.writeOK(coll.insert({_id: 0, x: 1, y: 3}));
    assert.writeOK(coll.insert({_id: 1, x: 2, y: 2}));
    assert.writeOK(coll.insert({_id: 2, x: 3, y: 1}));

    assert.eq([{_id: 1, x: 2, y: 2}],
              coll.aggregate([{$sort: {x: -1}}, {$limit: 2}, {$match: {y: {$gte: 2}}}]).toArray());

    assert.writeOK(coll.createIndex({x: 1}));
    assert.eq([{_id: 1, x: 2, y: 2}],
              coll.aggregate([{$sort: {x: -1}}, {$limit: 2}, {$match: {y: {$gte: 2}}}]).toArray());
}());
