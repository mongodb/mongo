// @tags: [requires_getmore, requires_fastcount]

let t = db.cursor1;
t.drop();

let big = "";
while (big.length < 50000)
    big += "asdasdasdasdsdsdadsasdasdasD";

let num = Math.ceil(10000000 / big.length);

for (var i = 0; i < num; i++) {
    t.save({num: i, str: big});
}

assert.eq(num, t.find().count());
assert.eq(num, t.find().itcount());

assert.eq(num / 2, t.find().limit(num / 2).itcount());

t.drop();  // save some space
