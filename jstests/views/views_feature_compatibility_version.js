// Test that views are only enabled if featureCompatibilityVersion is 3.4.

(function() {
    "use strict";

    const conn = MongoRunner.runMongod({});
    assert.neq(null, conn, "mongod was unable to start up");

    const viewsDB = conn.getDB("views_feature_compatibility_version");
    assert.commandWorked(viewsDB.dropDatabase());
    assert.commandWorked(viewsDB.runCommand({create: "collection"}));
    assert.commandWorked(viewsDB.runCommand({create: "collection2"}));

    const adminDB = conn.getDB("admin");

    // Ensure the featureCompatibilityVersion is 3.4.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.4"}));
    let res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq("3.4", res.featureCompatibilityVersion);

    // We can create a view when the featureCompatibilityVersion is 3.4.
    assert.commandWorked(viewsDB.runCommand({create: "view", viewOn: "collection", pipeline: []}));

    // We can update a view when the featureCompatibilityVersion is 3.4.
    assert.commandWorked(viewsDB.runCommand({collMod: "view", pipeline: []}));

    // We can perform deletes on the system.views collection when the featureCompatibilityVersion is
    // 3.4.
    assert.writeOK(viewsDB.system.views.remove({_id: "views_feature_compatibility_version.view2"}));

    // Ensure the featureCompatibilityVersion is 3.2.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.2"}));
    res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq("3.2", res.featureCompatibilityVersion);

    // We cannot create views when the featureCompatibilityVersion is 3.2.
    assert.commandFailed(viewsDB.runCommand({create: "view2", viewOn: "collection", pipeline: []}));

    // We cannot update views when the featureCompatibilityVersion is 3.2.
    assert.commandFailed(viewsDB.runCommand({collMod: "view", pipeline: []}));

    // We can read from a view when the featureCompatibilityVersion is 3.2.
    assert.writeOK(viewsDB.collection.insert({a: 5}));
    assert.eq(5, viewsDB.view.findOne().a);

    // We cannot create a collection with the same name as a view when the
    // featureCompatibilityVersion is 3.2.
    assert.commandFailed(viewsDB.runCommand({create: "view"}));
    assert.writeError(viewsDB.view.insert({a: 5}));

    // We can drop a view namespace when the featureCompatibilityVersion is 3.2.
    assert.eq(true, viewsDB.view.drop());

    // We can perform deletes on the system.views collection when the featureCompatibilityVersion is
    // 3.2.
    assert.writeOK(viewsDB.system.views.remove({_id: "views_feature_compatibility_version.view"}));

    // We can drop the system.views collection when the featureCompatibilityVersion is 3.2.
    assert.eq(true, viewsDB.system.views.drop());

    MongoRunner.stopMongod(conn);
}());
