/**
 * Basic test around rename collection involving views
 */

(function() {
"use strict";

const collNamePrefix = "rename_collection_view_collection";
const viewNamePrefix = "rename_collection_view_view";
let collCounter = 0;
let viewCounter = 0;

function getNewCollName() {
    return collNamePrefix + collCounter++;
}

function getNewColl() {
    let coll = db[getNewCollName()];
    coll.drop();
    assert.commandWorked(db[coll.getName()].insert({_id: 1}));
    return coll;
}

/**
 * Drops a view and then recreates against an underlying collection. Handle the case for those
 * suites where the test runs multiple times against the same cluster/replicaset instance and view
 * already exists
 */
function getNewView(collName) {
    // new view with empty filter
    const viewName = viewNamePrefix + viewCounter++;
    db.runCommand({drop: viewName});
    assert.commandWorked(db.createView(viewName, collName, []));
    return db[viewName];
}

jsTest.log(
    "Rename existing view should fail with CommandNotSupportedOnView indipendently on the target type (view or collection)");
{
    const srcColl = getNewColl();
    const srcView = getNewView(srcColl.getName());
    const dstColl = getNewColl();
    const dstView = getNewView(dstColl.getName());

    assert.commandFailedWithCode(
        db.adminCommand({renameCollection: srcView.getFullName(), to: dstColl.getFullName()}),
        ErrorCodes.CommandNotSupportedOnView,
        "rename view to existing collection should fail with CommandNotSupportedOnView");

    assert.commandFailedWithCode(
        db.adminCommand(
            {renameCollection: srcView.getFullName(), to: dstColl.getFullName(), dropTarget: true}),
        ErrorCodes.CommandNotSupportedOnView,
        "rename view to existing collection should fail with CommandNotSupportedOnView even if dropTarget is true");

    assert.commandFailedWithCode(
        db.adminCommand({renameCollection: srcView.getFullName(), to: dstView.getFullName()}),
        ErrorCodes.CommandNotSupportedOnView,
        "rename view to existing view should fail with CommandNotSupportedOnView");

    assert.commandFailedWithCode(
        db.adminCommand(
            {renameCollection: srcView.getFullName(), to: dstView.getFullName(), dropTarget: true}),
        ErrorCodes.CommandNotSupportedOnView,
        "rename view to existing view should fail with CommandNotSupportedOnView even if dropTarget is true");
}

jsTest.log("Rename coll to existing view should fail with NamespaceExists");
{
    const srcColl = getNewColl();
    const dstColl = getNewColl();

    const dstView = getNewView(dstColl.getName());

    assert.commandFailedWithCode(
        db.adminCommand({renameCollection: srcColl.getFullName(), to: dstView.getFullName()}),
        ErrorCodes.NamespaceExists,
        "rename collection to existing view should fail with NamespaceExists");

    assert.commandFailedWithCode(
        db.adminCommand(
            {renameCollection: srcColl.getFullName(), to: dstView.getFullName(), dropTarget: true}),
        ErrorCodes.NamespaceExists,
        "rename collection to existing view should fail with NamespaceExists even if dropTarget is true");
}
})();
