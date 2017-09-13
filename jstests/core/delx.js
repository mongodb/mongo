
a = db.getSisterDB("delxa");
b = db.getSisterDB("delxb");

function setup(mydb) {
    mydb.dropDatabase();
    for (i = 0; i < 100; i++) {
        mydb.foo.insert({_id: i});
    }
}

setup(a);
setup(b);

assert.eq(100, a.foo.find().itcount(), "A1");
assert.eq(100, b.foo.find().itcount(), "A2");

x = a.foo.find().sort({_id: 1}).batchSize(60);
y = b.foo.find().sort({_id: 1}).batchSize(60);

x.next();
y.next();

a.foo.remove({_id: {$gt: 50}});

assert.eq(51, a.foo.find().itcount(), "B1");
assert.eq(100, b.foo.find().itcount(), "B2");

xCount = x.itcount();
assert(xCount == 59 || xCount == 99, "C1 : " + xCount);  // snapshot or not is ok
assert.eq(
    99,
    y.itcount(),
    "C2");  // this was asserting because ClientCursor byLoc doesn't take db into consideration
