// SERVER-6531 support $within in $match aggregation operations

let c = db.s6531;
c.drop();

for (let x = 0; x < 10; x++) {
    for (let y = 0; y < 10; y++) {
        c.insert({loc: [x, y]});
    }
}

function test(variant) {
    let query = {loc: {$within: {$center: [[5, 5], 3]}}};
    let sort = {_id: 1};
    let aggOut = c.aggregate({$match: query}, {$sort: sort});
    let cursor = c.find(query).sort(sort);

    assert.eq(aggOut.toArray(), cursor.toArray());
}

test("no index");

c.createIndex({loc: "2d"});
test("2d index");

c.dropIndex({loc: "2d"});
c.createIndex({loc: "2dsphere"});
test("2dsphere index");
