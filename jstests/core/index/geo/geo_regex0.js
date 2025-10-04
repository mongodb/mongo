// From SERVER-2247
// Tests to make sure regex works with geo indices

let t = db.regex0;
t.drop();

t.createIndex({point: "2d", words: 1});
t.insert({point: [1, 1], words: ["foo", "bar"]});

let regex = {words: /^f/};
let geo = {point: {$near: [1, 1]}};
let both = {point: {$near: [1, 1]}, words: /^f/};

assert.eq(1, t.find(regex).count());
assert.eq(1, t.find(geo).count());
assert.eq(1, t.find(both).count());
