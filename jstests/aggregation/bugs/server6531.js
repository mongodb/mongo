// SERVER-6531 support $within in $match aggregation operations

c = db.s6531;
c.drop();

for (var x = 0; x < 10; x++) {
    for (var y = 0; y < 10; y++) {
        c.insert({loc: [x, y]});
    }
}

function test(variant) {
    query = {loc: {$within: {$center: [[5, 5], 3]}}};
    sort = {_id: 1};
    aggOut = c.aggregate({$match: query}, {$sort: sort});
    cursor = c.find(query).sort(sort);

    assert.eq(aggOut.toArray(), cursor.toArray());
}

test("no index");

c.ensureIndex({loc: "2d"});
test("2d index");

c.dropIndex({loc: "2d"});
c.ensureIndex({loc: "2dsphere"});
test("2dsphere index");
