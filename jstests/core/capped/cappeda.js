/**
 * @tags: [
 *   requires_capped,
 *   # capped collections connot be sharded
 *   assumes_unsharded_collection,
 * ]
 */

let t = db[jsTestName()];
t.drop();

let x = t.runCommand("create", {capped: true, size: 10000});
assert(x.ok);

for (let i = 0; i < 100; i++) t.insert({_id: i, x: 1});

function q() {
    return t.findOne({_id: 5});
}

function u() {
    let res = t.update({_id: 5}, {$set: {x: 2}});
    if (res.hasWriteError()) throw res;
}

// SERVER-3064
// assert.throws( q , [] , "A1" );
// assert.throws( u , [] , "B1" );

t.createIndex({_id: 1});

assert.eq(1, q().x);
q();
u();

assert.eq(2, q().x);
