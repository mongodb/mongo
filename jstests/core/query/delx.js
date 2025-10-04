// @tags: [assumes_against_mongod_not_mongos, requires_getmore, requires_non_retryable_writes]

let a = db.getSiblingDB("delxa");
let b = db.getSiblingDB("delxb");

function setup(mydb) {
    mydb.dropDatabase();
    for (let i = 0; i < 100; i++) {
        mydb.foo.insert({_id: i});
    }
}

setup(a);
setup(b);

assert.eq(100, a.foo.find().itcount(), "A1");
assert.eq(100, b.foo.find().itcount(), "A2");

let x = a.foo.find().sort({_id: 1}).batchSize(60);
let y = b.foo.find().sort({_id: 1}).batchSize(60);

x.next();
y.next();

a.foo.remove({_id: {$gt: 50}});

assert.eq(51, a.foo.find().itcount(), "B1");
assert.eq(100, b.foo.find().itcount(), "B2");

let xCount = x.itcount();
assert(xCount == 59 || xCount == 99, "C1 : " + xCount); // snapshot or not is ok
assert.eq(99, y.itcount(), "C2"); // this was asserting because ClientCursor byLoc doesn't take db into consideration
