/**
 * Basic test around rename collection
 *
 * @tags: [
 *   assumes_no_implicit_collection_creation_after_drop,
 *   does_not_support_zones,
 *   requires_non_retryable_commands,
 * ]
 */

import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";

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

{
    jsTest.log("Rename collection with documents");
    const src = getNewColl();
    const dstName = getNewCollName();

    assert.commandWorked(src.insert([{x: 1}, {x: 2}, {x: 3}]));

    assert.eq(3, src.countDocuments({}));

    assert.commandWorked(src.renameCollection(dstName));

    assert.eq(0, src.countDocuments({}));
    const dst = db[dstName];
    assert.eq(3, dst.countDocuments({}));
    dst.drop();
}

{
    jsTest.log("Rename non-existing collection");
    const src = getNewColl();
    const dst = getNewColl();

    assert.commandFailed(src.renameCollection(dst.getName()));
}

{
    jsTest.log("Rename collection with indexes");
    const src = getNewColl();
    const dstName = getNewCollName();
    const existingDst = getNewColl();

    assert.commandWorked(src.insert([{a: 1}, {a: 2}]));
    assert.commandWorked(src.createIndexes([{a: 1}, {b: 1}]));

    assert.commandWorked(existingDst.insert({a: 100}));
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

{
    jsTest.log("Rename collection with existing target");
    const src = getNewColl();
    const dst = getNewColl();

    assert.commandWorked(src.insert({x: 1}));
    assert.commandWorked(dst.insert({x: 2}));

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

{
    jsTest.log('Testing renameCollection with toNss == fromNss');
    const sameColl = getNewColl();
    assert.commandWorked(sameColl.insert({a: 1}));

    assert.commandFailedWithCode(
        sameColl.renameCollection(sameColl.getName(), true /* dropTarget */),
        [ErrorCodes.IllegalOperation]);

    assert.eq(1, sameColl.countDocuments({}), "Rename a collection to itself must not lose data");

    sameColl.drop();
}

// Rename non-existing source collection to a target collection/view (dropTarget=false) must
// fail with NamespaceNotFound. Make sure the check on the source is done before any check on
// the target for consistency with replicaset.
{
    jsTest.log('Testing renameCollection on non-existing source namespaces');
    const dbName = db.getName();

    // Rename non-existing source to non-existing target
    assert.commandFailedWithCode(
        db.adminCommand({renameCollection: dbName + ".nonExistingsource", to: dbName + ".target"}),
        ErrorCodes.NamespaceNotFound);

    // Rename non-existing source to existing collection
    const toColl = getNewColl();
    const toCollName = toColl.getFullName();
    toColl.insert({a: 0});

    assert.commandFailedWithCode(
        db.adminCommand({renameCollection: dbName + ".nonExistingsource", to: toCollName}),
        ErrorCodes.NamespaceNotFound);

    // Rename non-existing source to existing view
    const toViewName = dbName + ".target_view";
    assert.commandWorked(db.createView(toViewName, toCollName, []));

    assert.commandFailedWithCode(
        db.adminCommand({renameCollection: dbName + ".nonExistingsource", to: toViewName}),
        ErrorCodes.NamespaceNotFound);
}
