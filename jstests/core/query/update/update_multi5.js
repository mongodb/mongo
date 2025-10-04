// @tags: [assumes_balancer_off, requires_multi_updates, requires_non_retryable_writes]

// tests that $addToSet works in a multi-update.
let t = db[jsTestName()];
t.drop();

assert.commandWorked(t.insert({path: "r1", subscribers: [1, 2]}));
assert.commandWorked(t.insert({path: "r2", subscribers: [3, 4]}));

let res = assert.commandWorked(t.update({}, {$addToSet: {subscribers: 5}}, {upsert: false, multi: true}));

assert.eq(res.nMatched, 2, tojson(res));

t.find().forEach(function (z) {
    assert.eq(3, z.subscribers.length, tojson(z));
});
