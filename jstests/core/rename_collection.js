/**
 * Basic test around rename collection
 *
 * @tags: [
 *   # renameCollection is not supported on sharded collections
 *   assumes_unsharded_collection,
 *   requires_non_retryable_commands,
 * ]
 */

(function() {
"use strict";

const collNamePrefix = "rename_coll_test_";
let collCounter = 0;

function getNewColl() {
    let coll = db[collNamePrefix + collCounter++];
    coll.drop();
    return coll;
}

jsTest.log("Rename collection with documents");
{
    let a = getNewColl();
    let b = getNewColl();

    a.save({x: 1});
    a.save({x: 2});
    a.save({x: 3});

    assert.eq(3, a.countDocuments({}));
    assert.eq(0, b.countDocuments({}));

    assert.commandWorked(a.renameCollection(b.getName()));

    assert.eq(0, a.countDocuments({}));
    assert.eq(3, b.countDocuments({}));
}

jsTest.log("Rename collection with indexes");
{
    let a = getNewColl();
    let b = getNewColl();
    let c = getNewColl();

    a.save({a: 1});
    a.save({a: 2});
    a.createIndex({a: 1});
    a.createIndex({b: 1});

    c.save({a: 100});
    assert.commandFailed(db.adminCommand({renameCollection: a.getFullName(), to: c.getFullName()}));

    assert.commandWorked(db.adminCommand({renameCollection: a.getFullName(), to: b.getFullName()}));
    assert.eq(0, a.countDocuments({}));

    assert.eq(2, b.countDocuments({}));
    assert(db.getCollectionNames().indexOf(b.getName()) >= 0);
    assert(db.getCollectionNames().indexOf(a.getName()) < 0);
    assert.eq(3, b.getIndexes().length);
    assert.eq(0, a.getIndexes().length);
}

jsTest.log("Rename collection with existing target");
{
    let a = getNewColl();
    let b = getNewColl();

    a.save({x: 1});
    b.save({x: 2});

    assert.eq(1, a.countDocuments({x: 1}));
    assert.eq(1, b.countDocuments({x: 2}));

    assert.commandFailed(b.renameCollection(a.getName()));

    assert.eq(1, a.countDocuments({x: 1}));
    assert.eq(1, b.countDocuments({x: 2}));

    assert.commandWorked(b.renameCollection(a.getName(), true));

    assert.eq(1, a.countDocuments({x: 2}));
    assert.eq(0, b.countDocuments({}));
}
})();
