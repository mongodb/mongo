/**
 * Basic test around rename collection involving views
 */

const collNamePrefix = jsTestName() + "_collection";
const viewNamePrefix = jsTestName() + "_view";
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

function getNewView(collName) {
    // new view with empty filter
    const viewName = viewNamePrefix + viewCounter++;
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
