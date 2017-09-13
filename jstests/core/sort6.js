
t = db.sort6;

function get(x) {
    return t.find().sort({c: x}).map(function(z) {
        return z._id;
    });
}

// part 1
t.drop();

t.insert({_id: 1, c: null});
t.insert({_id: 2, c: 1});
t.insert({_id: 3, c: 2});

assert.eq([3, 2, 1], get(-1), "A1");  // SERVER-635
assert.eq([1, 2, 3], get(1), "A2");

t.ensureIndex({c: 1});

assert.eq([3, 2, 1], get(-1), "B1");
assert.eq([1, 2, 3], get(1), "B2");

// part 2
t.drop();

t.insert({_id: 1});
t.insert({_id: 2, c: 1});
t.insert({_id: 3, c: 2});

assert.eq([3, 2, 1], get(-1), "C1");  // SERVER-635
assert.eq([1, 2, 3], get(1), "C2");

t.ensureIndex({c: 1});

assert.eq([3, 2, 1], get(-1), "D1");
assert.eq([1, 2, 3], get(1), "X2");
