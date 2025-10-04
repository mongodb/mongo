/**
 * Test renaming capped collection
 *
 * @tags: [
 *   # capped collections can't be sharded
 *   assumes_unsharded_collection,
 *   requires_non_retryable_commands,
 *   requires_capped,
 *   requires_collstats,
 *   no_selinux,
 * ]
 */

let a = db.jstests_rename_a;
let b = db.jstests_rename_b;
let c = db.jstests_rename_c;

a.drop();
b.drop();
c.drop();

// TODO: too many numbers hard coded here
// this test depends precisely on record size and hence may not be very reliable
// note we use floats to make sure numbers are represented as doubles for SpiderMonkey, since test
// relies on record size
db.createCollection("jstests_rename_a", {capped: true, size: 10000});
for (let i = 0.1; i < 10; ++i) {
    a.save({i: i});
}
assert.commandWorked(db.adminCommand({renameCollection: "test.jstests_rename_a", to: "test.jstests_rename_b"}));
assert.eq(1, b.countDocuments({i: 9.1}));
printjson(b.stats());
for (var i = 10.1; i < 1000; ++i) {
    b.save({i: i});
}
printjson(b.stats());
// res = b.find().sort({i:1});
// while (res.hasNext()) printjson(res.next());

assert.eq(1, b.countDocuments({i: i - 1})); // make sure last is there
assert.eq(0, b.countDocuments({i: 9.1})); // make sure early one is gone

assert(db.getCollectionNames().indexOf("jstests_rename_b") >= 0);
assert(db.getCollectionNames().indexOf("jstests_rename_a") < 0);
assert(db.jstests_rename_b.stats().capped);

assert.throws(
    function () {
        db.jstests_rename.renameCollection({fail: "fail fail fail"});
    },
    [],
    "renameCollection should fail when passed a garbage object",
);

// Users should not be able to create a collection beginning with '.' through renameCollection.
// Auth suites throw InvalidNamespace and others throw IllegalOperation error.
assert.commandFailedWithCode(b.renameCollection(".foo"), [ErrorCodes.InvalidNamespace, ErrorCodes.IllegalOperation]);

db.jstests_rename_d.drop();
db.jstests_rename_e.drop();

db.jstests_rename_d.save({a: 222});
assert.commandWorked(db.jstests_rename_d.renameCollection("jstests_rename_e"));

assert(db.getCollectionNames().indexOf("jstests_rename_d") < 0);
assert(db.getCollectionNames().indexOf("jstests_rename_e") >= 0);
assert.eq(db.jstests_rename_e.findOne().a, 222);

assert.commandWorked(db.jstests_rename_e.renameCollection({to: "jstests_rename_d", dropTarget: true}));

assert(db.getCollectionNames().indexOf("jstests_rename_d") >= 0);
assert(db.getCollectionNames().indexOf("jstests_rename_e") < 0);
assert.eq(db.jstests_rename_d.findOne().a, 222);

db["jstests_rename_f"].drop();

assert.commandWorked(db.createCollection("jstests_rename_f"));
assert.commandFailedWithCode(
    db.getCollection("jstests_rename_f").renameCollection({to: "jstests_rename_$cmd"}),
    ErrorCodes.IllegalOperation,
);
