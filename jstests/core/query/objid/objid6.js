let o = new ObjectId();
assert(o.getTimestamp);

let a = new ObjectId("4c17f616a707427266a2801a");
let b = new ObjectId("4c17f616a707428966a2801c");
assert.eq(a.getTimestamp(), b.getTimestamp());
