/**
 * Test projecting DBRef fields ($ref, $id, $db) in views.
 *
 * Legacy find() queries do not support views, so must use the find() command.
 * @tags: [
 *   assumes_unsharded_collection,
 *   requires_find_command,
 * ]
 */
(function() {
"use strict";

const viewsDB = db.getSiblingDB("views_dbref_projection");
assert.commandWorked(viewsDB.dropDatabase());

assert.commandWorked(
    viewsDB.baseColl.insert({_id: 0, link: new DBRef("otherColl", "someId", viewsDB.getName())}));

assert.commandWorked(viewsDB.runCommand({create: "view", viewOn: "baseColl"}));

// Check that the view and base collection return the same thing.
function checkViewAndBaseCollection(projection, expectedResult) {
    const baseRes = viewsDB.baseColl.find({}, projection).toArray();
    const viewRes = viewsDB.view.find({}, projection).toArray();
    assert.eq(baseRes, viewRes);
    assert.eq(expectedResult, baseRes);
}

checkViewAndBaseCollection({"link.$ref": 1}, [{_id: 0, link: {$ref: "otherColl"}}]);
checkViewAndBaseCollection({"link.$db": 1}, [{_id: 0, link: {$db: viewsDB.getName()}}]);
checkViewAndBaseCollection({"link.$id": 1}, [{_id: 0, link: {$id: "someId"}}]);
}());
