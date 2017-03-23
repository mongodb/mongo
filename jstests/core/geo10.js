// Test for SERVER-2746

coll = db.geo10;
coll.drop();

assert.commandWorked(db.geo10.ensureIndex({c: '2d', t: 1}, {min: 0, max: Math.pow(2, 40)}));
assert.eq(2, db.geo10.getIndexes().length, "A3");

assert.writeOK(db.geo10.insert({c: [1, 1], t: 1}));
assert.writeOK(db.geo10.insert({c: [3600, 3600], t: 1}));
assert.writeOK(db.geo10.insert({c: [0.001, 0.001], t: 1}));

printjson(
    db.geo10
        .find({
            c: {
                $within:
                    {$box: [[0.001, 0.001], [Math.pow(2, 40) - 0.001, Math.pow(2, 40) - 0.001]]}
            },
            t: 1
        })
        .toArray());
