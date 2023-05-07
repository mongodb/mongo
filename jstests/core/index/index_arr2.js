// @tags: [assumes_balancer_off, requires_multi_updates, requires_non_retryable_writes]

let NUM = 20;
let M = 5;

let t = db.jstests_arr2;

function test(withIndex) {
    t.drop();

    // insert a bunch of items to force queries to use the index.
    let newObject = {_id: 1, a: [{b: {c: 1}}]};

    let now = (new Date()).getTime() / 1000;
    for (let created = now - NUM; created <= now; created++) {
        newObject['created'] = created;
        t.insert(newObject);
        newObject['_id']++;
    }

    // change the last M items.
    let query = {'created': {'$gte': now - M}};

    let Z = t.find(query).count();

    if (withIndex) {
        // t.createIndex( { 'a.b.c' : 1, 'created' : -1 } )
        // t.createIndex( { created : -1 } )
        t.createIndex({'a.b.c': 1}, {name: "x"});
    }

    var res = t.update(query, {'$set': {"a.0.b.c": 0}}, false, true);
    assert.eq(Z, res.nMatched, "num updated withIndex:" + withIndex);

    // now see how many were actually updated.
    query['a.b.c'] = 0;

    let count = t.count(query);

    assert.eq(Z, count, "count after withIndex:" + withIndex);
}

test(false);
test(true);
