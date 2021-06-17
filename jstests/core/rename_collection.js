/**
 * Basic test around rename collection
 *
 * @tags: [
 *   requires_fcv_50,
 *   requires_non_retryable_commands,
 *   assumes_no_implicit_collection_creation_after_drop
 * ]
 */

(function() {
"use strict";

const collNamePrefix = "rename_coll_test_";
let collCounter = 0;

function getNewCollName() {
    return collNamePrefix + collCounter++;
}

function getNewColl() {
    let coll = db[getNewCollName()];
    coll.drop();
    return coll;
}

jsTest.log("Rename collection with documents (create SRC and then DST)");
{
    const src = getNewColl();
    const dstName = getNewCollName();

    src.save({x: 1});
    src.save({x: 2});
    src.save({x: 3});

    assert.eq(3, src.countDocuments({}));

    assert.commandWorked(src.renameCollection(dstName));

    assert.eq(0, src.countDocuments({}));
    const dst = db[dstName];
    assert.eq(3, dst.countDocuments({}));
    dst.drop();
}

jsTest.log("Rename collection with documents (create DST and then SRC)");
{
    const src = getNewColl();
    const dstName = getNewCollName();

    src.save({x: 1});
    src.save({x: 2});
    src.save({x: 3});

    assert.eq(3, src.countDocuments({}));

    assert.commandWorked(src.renameCollection(dstName));

    assert.eq(0, src.countDocuments({}));
    const dst = db[dstName];
    assert.eq(3, dst.countDocuments({}));
    dst.drop();
}

jsTest.log("Rename collection with indexes");
{
    const src = getNewColl();
    const dstName = getNewCollName();
    const existingDst = getNewColl();

    src.save({a: 1});
    src.save({a: 2});
    src.createIndex({a: 1});
    src.createIndex({b: 1});

    existingDst.save({a: 100});
    assert.commandFailed(
        db.adminCommand({renameCollection: src.getFullName(), to: existingDst.getFullName()}));

    const originalNumberOfIndexes = src.getIndexes().length;
    assert.commandWorked(src.renameCollection(dstName));
    assert.eq(0, src.countDocuments({}));

    const dst = db[dstName];
    assert.eq(2, dst.countDocuments({}));
    assert(db.getCollectionNames().indexOf(dst.getName()) >= 0);
    assert(db.getCollectionNames().indexOf(src.getName()) < 0);
    assert.eq(originalNumberOfIndexes, dst.getIndexes().length);
    assert.eq(0, src.getIndexes().length);
    dst.drop();
}

jsTest.log("Rename collection with existing target");
{
    const src = getNewColl();
    const dst = getNewColl();

    src.save({x: 1});
    dst.save({x: 2});

    assert.eq(1, src.countDocuments({x: 1}));
    assert.eq(1, dst.countDocuments({x: 2}));

    assert.commandFailed(src.renameCollection(dst.getName()));

    assert.eq(1, src.countDocuments({x: 1}));
    assert.eq(1, dst.countDocuments({x: 2}));

    assert.commandWorked(src.renameCollection(dst.getName(), true /* dropTarget */));

    assert.eq(0, src.countDocuments({x: 2}));
    assert.eq(1, dst.countDocuments({}));

    dst.drop();
}
})();
