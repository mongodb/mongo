// btreedel.js
// @tags: [SERVER-32869]

let t = db.foo;
t.remove({});

let bulk = t.initializeUnorderedBulkOp();
for (let i = 0; i < 1000000; i++) {
    bulk.insert({_id: i, x: "a                                                              b"});
}
assert.commandWorked(bulk.execute());

print("1 insert done count: " + t.count());

let c = t.find({y: null}).sort({_id: 1});
for (let j = 0; j < 400000; j++) {
    c.next();
    if (j % 200000 == 0) printjson(c.next());
}
printjson(c.next());

let d = t.find({_id: {$gt: 300000}}).sort({_id: -1});
d.next();

print("2");

t.remove({_id: {$gt: 200000, $lt: 600000}});

print("3");
print(d.hasNext());

let n = 0;
let last = {};
printjson(c.next());
while (c.hasNext()) {
    n++;
    last = c.next();
}

print("4. n:" + n);
printjson(last);

assert(n > 100000);

print("btreedel.js success");
