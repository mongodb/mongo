let res;
const t = db[jsTestName()];
t.drop();

let orig = {
    _id: 1,
    a: [
        {x: 1, y: 2},
        {x: 10, y: 11},
    ],
};

res = t.save(orig);
assert.commandWorked(res);

// SERVER-181
res = t.update({}, {$set: {"a.0.x": 3}});
assert.commandWorked(res);
orig.a[0].x = 3;
assert.eq(orig, t.findOne(), "A1");

res = t.update({}, {$set: {"a.1.z": 17}});
assert.commandWorked(res);
orig.a[1].z = 17;
assert.eq(orig, t.findOne(), "A2");

// SERVER-273
res = t.update({}, {$unset: {"a.1.y": 1}});
assert.commandWorked(res);
delete orig.a[1].y;
assert.eq(orig, t.findOne(), "A3");

// SERVER-333
assert(t.drop());
orig = {
    _id: 1,
    comments: [{name: "blah", rate_up: 0, rate_ups: []}],
};
res = t.save(orig);
assert.commandWorked(res);

res = t.update({}, {$inc: {"comments.0.rate_up": 1}, $push: {"comments.0.rate_ups": 99}});
assert.commandWorked(res);
orig.comments[0].rate_up++;
orig.comments[0].rate_ups.push(99);
assert.eq(orig, t.findOne(), "B1");

assert(t.drop());
orig = {
    _id: 1,
    a: [],
};
for (let i = 0; i < 12; i++) orig.a.push(i);

res = t.save(orig);
assert.commandWorked(res);
assert.eq(orig, t.findOne(), "C1");

res = t.update({}, {$inc: {"a.0": 1}});
assert.commandWorked(res);
orig.a[0]++;
assert.eq(orig, t.findOne(), "C2");

res = t.update({}, {$inc: {"a.10": 1}});
assert.commandWorked(res);
orig.a[10]++;

// SERVER-3218
assert(t.drop());
t.insert({"a": {"c00": 1}, "c": 2});
res = t.update({"c": 2}, {"$inc": {"a.c000": 1}});
assert.commandWorked(res);

assert.eq({"c00": 1, "c000": 1}, t.findOne().a, "D1");

// SERVER-103960: Field paths cannot contain more than 255 dots.
assert(t.drop());

const longPath = ".".repeat(256);

assert.commandWorked(t.insertMany([{a: 1}, {b: 1}]));
assert.commandFailedWithCode(t.update({a: 1}, {$set: {[longPath]: "y"}}), 10396001);
